#include "session.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "api.h"
#include "commands.h"
#include "hooks.h"
#include "models.h"
#include "notify.h"
#include "paths.h"
#include "repl.h"
#include "stats.h"
#include "telegram.h"
#include "tui.h"

namespace session {

using json = nlohmann::json;

namespace {

// Break a libedit line into shell-style tokens so a drop of one or
// more paths from Tracker is correctly split. Single and double
// quotes preserve internal spaces; a literal `\` escapes the next
// char. Stops short of full shell expansion — no $VAR, no ~/, no
// globbing. Tracker drops arrive as absolute POSIX paths.
std::vector<std::string> shell_tokenize(const std::string& s) {
	std::vector<std::string> out;
	std::string cur;
	char quote = 0;
	bool in_tok = false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (quote) {
			if (c == quote) { quote = 0; }
			else            { cur += c; }
			in_tok = true;
			continue;
		}
		if (c == '"' || c == '\'') { quote = c; in_tok = true; continue; }
		if (c == '\\' && i + 1 < s.size()) {
			cur += s[++i];
			in_tok = true;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(c))) {
			if (in_tok) { out.push_back(std::move(cur)); cur.clear(); in_tok = false; }
			continue;
		}
		cur += c;
		in_tok = true;
	}
	if (in_tok) out.push_back(std::move(cur));
	return out;
}

// Returns true when the process is running inside an SSH session.
// Used to suppress the bracketed-paste multi-line input hint that
// is unreliable over SSH.
bool IsSshSession() {
	return std::getenv("SSH_CLIENT")     != nullptr  // flawfinder: ignore
		|| std::getenv("SSH_TTY")        != nullptr  // flawfinder: ignore
		|| std::getenv("SSH_CONNECTION") != nullptr;  // flawfinder: ignore
}

// True if `line` is a drag-and-drop from Tracker: one or more
// tokens, every one an absolute path that stat-resolves. Bare
// filenames without a leading slash are NOT treated as drops so the
// user can still type "main.cpp" as a literal question.
//
// __attribute__((noinline)): GCC 13 LTO loses track of the vector
// element pointer across inlining and emits a spurious
// -Wfree-nonheap-object. Keeping this function out-of-line gives the
// optimiser a clean boundary and silences the false positive.
__attribute__((noinline))
bool line_is_path_drop(const std::string& line,
                       std::vector<std::string>& out_abs_paths) {
	auto tokens = shell_tokenize(line);
	if (tokens.empty()) return false;
	std::vector<std::string> resolved;
	resolved.reserve(tokens.size());
	for (const auto& t : tokens) {
		if (t.empty() || t.front() != '/') return false;
		struct stat st;
		if (stat(t.c_str(), &st) != 0) return false;
		char abs[PATH_MAX];
		const char* use = realpath(t.c_str(), abs) ? abs : t.c_str();  // flawfinder: ignore
		resolved.emplace_back(use);
	}
	out_abs_paths = std::move(resolved);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// LocalWorker — runs SendWithTools (and post-turn bookkeeping that must stay
// on the same thread as the API call) on a background thread so the main
// thread can return to ReadMessage() immediately after the user presses Enter.
//
// Ownership protocol:
//   • fWorkerOwnsDisplay == true  → worker is writing to stdout; main thread
//     must not call ReadMessage() or print anything.
//   • fWorkerOwnsDisplay == false → main thread owns stdout; worker is idle.
//
// Lifecycle: created once at InteractiveLoop entry, destroyed at exit.
//   Enqueue() hands a job to the worker and sets fWorkerOwnsDisplay = true.
//   The worker sets it back to false when it finishes, then posts the result.
//   The main thread waits on fDisplayCv, drains the result, then shows the
//   prompt again.
// ---------------------------------------------------------------------------

struct TurnJob {
	// Inputs — snapshot taken at enqueue time.
	std::string     userText;       // raw user message (display copy)
	std::string     apiContent;     // may differ (attachment preamble prepended)
	json            snapshot;       // messages[] before this turn (for rollback)
	std::string     model;
	int             maxTokens;
	config::Auth    auth;
	std::string     systemPrompt;
	bool            hasTelegram;    // true → call Mirror*/Acquire/Release
};

struct TurnResult {
	bool            ok          = false;
	int             exitCode    = 0;
	int             inputTokens = 0;
	int             outputTokens= 0;
	int             cacheRead   = 0;
	int             cacheWrite  = 0;
	double          elapsed     = 0.0;
	std::string     assistantText;
	std::vector<std::string> newUrls;
	// Paths whose claude:summary BFS attribute was written during this
	// turn (via WriteAttr tool calls). Used to refresh the in-process
	// snapshot without a full filesystem walk.
	std::vector<std::string> writtenSummaryPaths;
	// When non-empty, the turn was cancelled via Ctrl+X and this is
	// the user's original input — restore it to the edit buffer.
	std::string     cancelledInput;
};

// All fields accessed from both threads are protected by fMu except
// fWorkerOwnsDisplay which has its own mutex so the main thread can
// wait on it without holding fMu.
struct LocalWorker {
	// ── job queue (main → worker) ─────────────────────────────────
	std::mutex              fMu;
	std::condition_variable fJobCv;
	std::optional<TurnJob>  fPendingJob;   // at most one job queued
	bool                    fShutdown = false;

	// ── result (worker → main) ────────────────────────────────────
	std::condition_variable fResultCv;
	std::optional<TurnResult> fResult;

	// ── display ownership ─────────────────────────────────────────
	// Separate mutex so the main thread can do a clean wait without
	// holding fMu (which the worker also needs).
	std::mutex              fDisplayMu;
	std::condition_variable fDisplayCv;
	bool                    fWorkerOwnsDisplay = false;

	// ── back-pointers set before thread starts ────────────────────
	// These are the InteractiveLoop-owned variables the worker writes.
	// Access is safe because the main thread waits on fDisplayCv
	// (i.e. is idle) while the worker is running.
	json*                   fMessages       = nullptr;
	int*                    fTurnCount      = nullptr;
	int*                    fSessionInput   = nullptr;
	int*                    fSessionOutput  = nullptr;
	std::vector<std::string>* fSessionUrls  = nullptr;
	std::string*            fModel          = nullptr;  // read-only during turn
	std::string*            fResumeName     = nullptr;
	const json*             fPrices         = nullptr;
	bool*                   fNotifyEnabled  = nullptr;
	double*                 fNotifyMinDur   = nullptr;
	double                  fCompactThresh  = 0.0;
	int                     fCompactWindow  = 0;
	config::Auth*           fAuth           = nullptr;  // read-only during turn
	std::string*            fCustomSystem   = nullptr;  // read-only during turn
	telegram::RemoteControl* fRemote        = nullptr;  // may be null
	std::function<void()>   fUpdateStatus;

	std::thread             fThread;
};

// The background thread function. Loops waiting for jobs, executes
// SendWithTools, posts the result, then idles.
static void LocalWorkerFunc(LocalWorker& w)
{
	while (true) {
		// Wait for a job or shutdown signal.
		TurnJob job;
		{
			std::unique_lock<std::mutex> lk(w.fMu);
			w.fJobCv.wait(lk, [&]{
				return w.fPendingJob.has_value() || w.fShutdown;
			});
			if (w.fShutdown && !w.fPendingJob.has_value()) break;
			job = std::move(*w.fPendingJob);
			w.fPendingJob.reset();
		}

		// ── Execute the turn ──────────────────────────────────────
		TurnResult result;
		const auto turn_start = std::chrono::steady_clock::now();

		api::StreamProgress stream_progress;

		if (job.hasTelegram && w.fRemote) {
			w.fRemote->AcquireTurn();
			api::g_stream_progress = &stream_progress;
			w.fRemote->MirrorPrompt(job.userText);
			w.fRemote->StartThinkingUpdater(&stream_progress);
		}

		// Append user turn to the shared messages array.
		// Safe: main thread is waiting on fDisplayCv and not touching
		// fMessages while fWorkerOwnsDisplay == true.
		w.fMessages->push_back({{"role", "user"}, {"content", job.apiContent}});

		const api::SendResult api_result = api::SendWithTools(
			job.auth, job.model, job.maxTokens,
			*w.fMessages, job.systemPrompt);

		if (job.hasTelegram && w.fRemote) {
			w.fRemote->StopThinkingUpdater();
			api::g_stream_progress = nullptr;
			w.fRemote->ReleaseTurn();
		}

		result.elapsed     = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - turn_start).count();
		result.exitCode    = api_result.exit_code;
		result.ok          = (api_result.exit_code == 0);
		result.inputTokens = api_result.input_tokens;
		result.outputTokens= api_result.output_tokens;
		result.cacheRead   = api_result.cache_read_input_tokens;
		result.cacheWrite  = api_result.cache_creation_input_tokens;
		result.assistantText = api_result.assistant_text;

		// If the turn was cancelled via Ctrl+X, record the original
		// user text so InteractiveLoop can restore it to the edit
		// buffer. Clear g_cancel_retype so the next turn starts clean.
		if (g_cancel_retype) {
			result.cancelledInput = job.userText;
			g_cancel_retype = 0;
		}

		if (!result.ok) {
			// Roll messages back to the snapshot so the failed turn
			// is not permanently in the history.
			*w.fMessages = job.snapshot;
		} else {
			// Extract URLs for /open.
			for (auto& url : notify::ExtractUrls(result.assistantText)) {
				if (std::find(w.fSessionUrls->begin(), w.fSessionUrls->end(), url)
						== w.fSessionUrls->end()) {
					result.newUrls.push_back(url);
				}
			}
			// Collect paths whose claude:summary was written this turn
			// so the main thread can refresh the in-process snapshot.
			result.writtenSummaryPaths = api::DrainWrittenSummaryPaths();
		}

		// ── Post result, release display ──────────────────────────
		{
			std::lock_guard<std::mutex> lk(w.fMu);
			w.fResult = std::move(result);
		}
		w.fResultCv.notify_one();

		{
			std::lock_guard<std::mutex> lk(w.fDisplayMu);
			w.fWorkerOwnsDisplay = false;
		}
		w.fDisplayCv.notify_all();
	}
}

std::string ComposeAttachmentPreamble(const std::vector<std::string>& paths) {
	if (paths.empty()) return {};
	std::string s = "Files attached to this session:\n";
	for (const auto& p : paths) { s += "- "; s += p; s += '\n'; }
	s += '\n';
	return s;
}

std::string FormatAttachedLine(const std::vector<std::string>& paths) {
	std::string shown;
	for (size_t i = 0; i < paths.size() && i < 3; ++i) {
		if (i > 0) shown += ", ";
		shown += paths[i];
	}
	if (paths.size() > 3) {
		shown += ", +" + std::to_string(paths.size() - 3) + " more";
	}
	return "[attached: " + shown + "]";
}

int InteractiveLoop(const config::Auth& initial_auth, const config::Config& cfg,
                    const std::string& initial_model, int max_tokens,
                    const std::string& custom_system, const json& prices, bool resume,
                    const std::string& resume_name,
                    const std::string& initial_message,
                    std::vector<std::string> initial_attachments) {
	api::InterruptGuard interrupt_guard;
	json messages = json::array();
	std::string model = initial_model;

	repl::Init(paths::ReplHistoryPath());

	commands::Load(paths::ConfigDir() + "/commands");
	std::vector<std::string> all_slash = {
		"/help", "/clear", "/model", "/compact", "/usage",
		"/todos", "/memory", "/stats", "/open", "/notify",
		"/remote-control", "/ludicrous", "/exit", "/quit",
	};
	for (const auto& c : commands::Names()) all_slash.push_back("/" + c);
	repl::SetSlashCommands(all_slash);

	// Mutable copy of the initial auth so we can refresh tokens
	// in-place before each turn without touching the caller's
	// reference.
	config::Auth auth = initial_auth;

	hooks::Fire(hooks::Event::SessionStart, json::object());
	stats::RecordSession();
	config::LogLine("session start (model=" + model + ")");

	int turn_count         = 0;
	int session_input      = 0;
	int session_output     = 0;

	// Notification state — start from config, toggleable at runtime
	// via `/notify on|off|<seconds>`.
	bool   notify_enabled       = cfg.notify_enabled;
	double notify_min_duration  = cfg.notify_min_duration_sec;

	// Auto-compact thresholds. context_window resolves to the
	// model-specific cap on first use (handled inside the loop so
	// a `/model` swap mid-session picks up the new window).
	const double compact_auto_threshold  = cfg.compact_auto_threshold;
	const int    compact_window_override = cfg.compact_context_window;

	std::unique_ptr<telegram::RemoteControl> remote;

	// Remote-control is off by default even when telegram config is
	// present. Use /remote-control to start it explicitly.

	// Compose the status row including a green "Remote Control
	// active" right-label whenever the remote poller is running.
	// Mute state gets an appended "· muted" marker.
	auto compose_status = [&]() {
		std::string right;
		if (api::g_ludicrous_mode.load()) {
			right = tui::Yellow("\xE2\x9A\xA1 LUDICROUS");
		}
		if (remote && remote->Running()) {
			if (!right.empty()) right += tui::Dim("  \xC2\xB7  ");
			right += tui::Green("Remote Control active");
			if (telegram::g_muted.load()) {
				right += tui::Yellow(" \xC2\xB7 muted");
			}
		}
		return models::FormatStatusRow(model, turn_count, session_input,
								 session_output, max_tokens, right);
	};

	// Install the fixed-bottom status frame first. This clears the
	// terminal so output from any prior session in the same window
	// doesn't bleed into the new session's scroll area.
	tui::InstallStatusBar(compose_status());
	struct StatusFrameGuard {
		~StatusFrameGuard() {
			// Disable bracketed paste and drain stale input BEFORE
			// resetting the scroll region so the terminal is in a clean
			// state when the shell takes over.
			repl::Deinit();
			tui::TeardownStatusBar();
		}
	} status_frame_guard;

	// Welcome text and optional resume header appear inside the
	// newly-cleared scroll region.
	if (resume) {
		if (auto loaded = config::LoadHistory(resume_name); loaded && loaded->is_array()) {
			messages = *loaded;
			const std::string hist_path = resume_name.empty()
				? paths::HistoryPath()
				: paths::NamedHistoryPath(resume_name);
			std::cout << tui::Meta("[resumed " + std::to_string(messages.size())
								   + " messages from " + hist_path + "]")
					  << "\n";
		} else {
			const std::string hist_path = resume_name.empty()
				? paths::HistoryPath()
				: paths::NamedHistoryPath(resume_name);
			std::cout << tui::Meta("[no prior session to resume at " + hist_path + "]")
					  << "\n";
		}
	}

	// ASCII art logo — Braille-dot art of the Claude icon.
	if (tui::ColorEnabled()) {
		#define O "\x1b[38;2;255;138;24m"   // orange/amber
		#define R "\x1b[0m"
		std::cout
		<< O "⠀⠀⠀⠀⠀⠀⠀⢀⣠⣤⣤⣶⣶⣶⣶⣤⣤⣄⡀⠀⠀⠀⠀⠀⠀⠀" R "\n"
		<< O "⠀⠀⠀⠀⢀⣤⣾⣿⣿⣿⣿⡿⠿⠿⢿⣿⣿⣿⣿⣷⣤⡀⠀⠀⠀⠀" R "\n"
		<< O "⠀⠀⠀⣴⣿⣿⣿⠟⠋⣻⣤⣤⣤⣤⣤⣄⣉⠙⠻⣿⣿⣿⣦⠀⠀⠀" R "\n"
		<< O "⠀⢀⣾⣿⣿⣿⣇⣤⣾⠿⠛⠉⠉⠉⠉⠛⠿⣷⣶⣿⣿⣿⣿⣷⡀⠀" R "\n"
		<< O "⠀⣾⣿⣿⣿⣿⣿⡟⠁⠀⠀⠀⠀⠀⠀⠀⠀⠈⢻⣿⣿⣿⣿⣿⣷⠀" R "\n"
		<< O "⢠⣿⣿⣿⣿⣿⡟⠀⠀⠀⠀⢸⣿⣿⣿⠀⠀⠀⠀⢻⣿⣿⣿⣿⣿⡄" R "\n"
		<< O "⢸⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⢸⣿⣿⣿⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⡇" R "\n"
		<< O "⠘⣿⣿⣿⣿⣿⣧⠀⠀⠀⠀⠘⠛⠛⠛⠀⠀⠀⠀⣼⣿⣿⣿⣿⣿⠃" R "\n"
		<< O "⠀⢿⣿⣿⣿⣿⣿⣧⡀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣼⣿⣿⣿⣿⣿⡿⠀" R "\n"
		<< O "⠀⠈⢿⣿⣿⣿⣿⣿⣿⣶⣤⣀⣀⣀⣀⣤⣶⣿⣿⣿⣿⣿⣿⡿⠁⠀" R "\n"
		<< O "⠀⠀⠀⠻⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠟⠀⠀⠀" R "\n"
		<< O "⠀⠀⠀⠀⠈⠛⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠛⠁⠀⠀⠀⠀" R "\n"
		<< O "⠀⠀⠀⠀⠀⠀⠀⠈⠙⠛⠛⠿⠿⠿⠿⠛⠛⠋⠁⠀⠀⠀⠀⠀⠀⠀" R "\n"
		<< "\n";
		#undef O
		#undef R
	}

	std::cout << tui::Bold("Claude CLI interactive mode") << tui::Dim(" (model: " + model + ")") << ".\n"
			  << tui::Dim("Type /help for commands, /exit or Ctrl+D to leave.") << "\n"
			  << tui::Dim("Multi-line input: Ctrl+J or \\ + Enter.") << "\n\n";

	// Drain any bytes the terminal sent in response to our init
	// sequences (bracketed-paste enable, DECSTBM scroll-region setup,
	// etc.) before handing stdin to readline.  Without this drain,
	// bracketed_getc mis-parses the unsolicited escape sequences and
	// effectively swallows the user's first Enter keypress, requiring
	// two Enters to submit the first prompt.
	repl::DrainStaleInput();

	std::string pending = initial_message;

	// Paths announced to Claude on the next outgoing user turn.
	std::vector<std::string> pending_paths = std::move(initial_attachments);

	// URLs seen in assistant replies so far this session. Populated
	// after each turn by notify::ExtractUrls; consumed by `/open`.
	std::vector<std::string> session_urls;

	// ── LocalWorker — background thread for API calls ──────────────
	// Wire back-pointers before starting the thread.  All pointed-to
	// variables are declared above and outlive the worker thread which
	// is joined before InteractiveLoop returns.
	LocalWorker worker;
	worker.fMessages       = &messages;
	worker.fTurnCount      = &turn_count;
	worker.fSessionInput   = &session_input;
	worker.fSessionOutput  = &session_output;
	worker.fSessionUrls    = &session_urls;
	worker.fModel          = &model;
	worker.fResumeName     = const_cast<std::string*>(&resume_name);
	worker.fPrices         = &prices;
	worker.fNotifyEnabled  = &notify_enabled;
	worker.fNotifyMinDur   = &notify_min_duration;
	worker.fCompactThresh  = compact_auto_threshold;
	worker.fCompactWindow  = compact_window_override;
	worker.fAuth           = &auth;
	worker.fCustomSystem   = const_cast<std::string*>(&custom_system);
	worker.fRemote         = remote.get();   // updated via lambda below
	worker.fUpdateStatus   = [&]() { tui::SetStatusBar(compose_status()); };
	worker.fThread = std::thread(LocalWorkerFunc, std::ref(worker));

	// Ensure the worker thread is stopped and joined on any exit path.
	struct WorkerGuard {
		LocalWorker& w;
		~WorkerGuard() {
			{
				std::lock_guard<std::mutex> lk(w.fMu);
				w.fShutdown = true;
			}
			w.fJobCv.notify_all();
			if (w.fThread.joinable()) w.fThread.join();
		}
	} worker_guard{worker};

	// Queued input: typed by the user while a turn was in flight.
	// Dispatched as the next turn immediately after the current one
	// completes, without returning to ReadMessage().
	bool        turn_active = false;   // true while worker owns display

	// Helper: drain the worker result and run all post-turn bookkeeping.
	// Called from the main loop when fWorkerOwnsDisplay has gone false.
	// Returns false if the session should exit.
	// allowNotify should be true only when the turn finished while the
	// user was idle (top-of-loop poll). Pass false when the user has
	// already typed new input or is exiting — they are clearly aware
	// the turn is done, so a desktop notification would be spurious.
	auto drain_turn = [&](bool allowNotify = true) -> bool {
		// Restore stdout to direct mode; flush any buffered output
		// the worker left in the pending buffer.
		tui::EndTurn();

		// Drain the result.
		TurnResult result;
		{
			std::lock_guard<std::mutex> lk(worker.fMu);
			if (worker.fResult.has_value()) {
				result = std::move(*worker.fResult);
				worker.fResult.reset();
			}
		}

		// Restore normal status bar (hint was shown while active).
		tui::SetStatusBar(compose_status());

		if (!result.ok) {
			if (remote && remote->Running())
				remote->MirrorCancel();
			if (!result.cancelledInput.empty()) {
				repl::RemoveLastRecord();
				repl::RestoreInput(result.cancelledInput);
			}
			return true; // session continues
		}

		++turn_count;
		session_input  += result.inputTokens;
		session_output += result.outputTokens;
		stats::RecordTurn(result.inputTokens, result.outputTokens,
						  result.cacheRead, result.cacheWrite);

		for (auto& url : result.newUrls)
			session_urls.push_back(std::move(url));

		if (!result.writtenSummaryPaths.empty())
			config::RefreshSummarySnapshot(result.writtenSummaryPaths);

		if (allowNotify && notify_enabled && result.elapsed >= notify_min_duration) {
			notify::Send(
				notify::PickPlayfulTitle(result.elapsed),
				notify::FirstSentence(result.assistantText, 120));
		}

		if (compact_auto_threshold > 0.0) {
			const int window  = models::DetectContextWindow(model, compact_window_override);
			const int trigger = static_cast<int>(window * compact_auto_threshold);
			if (result.inputTokens >= trigger) {
				char note[160];
				std::snprintf(note, sizeof(note),
					"[auto-compact: context at %d%% (%d / %d tokens) — compacting now]",
					(result.inputTokens * 100) / window,
					result.inputTokens, window);
				std::cout << tui::Meta(note) << "\n";
				commands::LoopCtx ac_ctx{auth, max_tokens, custom_system, prices, model,
				                         turn_count, session_input, session_output, messages,
				                         session_urls, notify_enabled, notify_min_duration,
				                         [&]() { tui::SetStatusBar(compose_status()); }};
				ac_ctx.auto_compact          = true;
				ac_ctx.resume_name           = resume_name;
				ac_ctx.compact_auto_threshold  = compact_auto_threshold;
				ac_ctx.compact_window_override = compact_window_override;
				std::string dummy;
				commands::Dispatch("/compact", ac_ctx, dummy);
			}
		}

		config::LogLine("turn " + std::to_string(turn_count)
				 + " model=" + model
				 + " in=" + std::to_string(result.inputTokens)
				 + " out=" + std::to_string(result.outputTokens)
				 + " cache_r=" + std::to_string(result.cacheRead)
				 + " cache_w=" + std::to_string(result.cacheWrite));

		tui::SetStatusBar(compose_status());

		if (remote && remote->Running())
			remote->MirrorToPrimary(result.assistantText);

		config::SaveHistory(messages, model, resume_name);
		hooks::Fire(hooks::Event::Stop, json{{"assistant_text", result.assistantText}});
		// Discard any keystrokes typed during the turn (e.g. menu
		// approval digits) so they don't contaminate the next prompt.
		repl::DrainStaleInput();
		return true;
	};

	// Helper: enqueue a turn to the worker and install the output
	// interceptor so worker writes go through the pending buffer.
	auto dispatch_turn = [&](const std::string& line,
	                         std::string api_content,
	                         json snapshot,
	                         std::string system_for_turn) {
		TurnJob job;
		job.userText     = line;
		job.apiContent   = std::move(api_content);
		job.snapshot     = std::move(snapshot);
		job.model        = model;
		job.maxTokens    = max_tokens;
		job.auth         = auth;
		job.systemPrompt = std::move(system_for_turn);
		job.hasTelegram  = (remote && remote->Running());

		// Install the stdout interceptor before waking the worker so
		// the very first byte it writes goes into the pending buffer.
		tui::BeginTurn();
		turn_active = true;

		// Install the turn-done callback so the flush timer can wake
		// ReadMessage() when the worker finishes, without requiring a
		// real keypress from the user.
		tui::SetTurnDoneCheck([&worker]() -> bool {
			std::lock_guard<std::mutex> lk(worker.fDisplayMu);
			return !worker.fWorkerOwnsDisplay;
		});

		{
			std::lock_guard<std::mutex> dlk(worker.fDisplayMu);
			worker.fWorkerOwnsDisplay = true;
		}
		{
			std::lock_guard<std::mutex> lk(worker.fMu);
			worker.fPendingJob = std::move(job);
		}
		worker.fJobCv.notify_one();

		tui::SetStatusBar(compose_status()
			+ "  " + tui::Dim("ctrl+x: amend · enter waits"));
	};

	while (true) {
		// ── Check whether the active turn has finished ────────────────
		// Do this at the top of every iteration so we drain the result
		// whether we return from ReadMessage (user typed) or loop back
		// after a queued input.
		if (turn_active) {
			bool done;
			{
				std::lock_guard<std::mutex> dlk(worker.fDisplayMu);
				done = !worker.fWorkerOwnsDisplay;
			}
			if (done) {
				turn_active = false;
				if (!drain_turn()) break; // session exit
			}
		}

		// Resize events rebuild the scroll region and redraw the
		// fixed rows so the frame stays correct after the user
		// drags the terminal window.
		if (tui::ConsumeResizePending()) {
			// Flush buffered turn output before redrawing so the
			// scroll region is consistent.
			tui::FlushTurnOutput();
			tui::RedrawStatusBar();
		}
		tui::ShowCursor();
		tui::PositionCursorForInput();

		// If a tool menu was shown during the previous turn, libedit may
		// have captured the approval keystroke into its internal buffer.
		// Clear it here (on the main thread) before the next ReadMessage.
		if (repl::ConsumeClearEditBufferRequest()) {
			repl::ClearEditBuffer();
		}

		std::string line;

		if (!pending.empty()) {
			line    = std::move(pending);
			pending.clear();
			tui::ClearInputRow();
			tui::PositionCursorForChat();
			std::cout << tui::UserPrompt() << line << "\n" << std::flush;
			tui::PositionCursorForChat();
		} else {
			// libedit draws the prompt at the fixed input row N-2
			// (outside the scroll region). On Enter libedit moves the
			// cursor down but does NOT trigger a DECSTBM scroll (N-2
			// is outside the scroll region). "> hi" stays at N-2.
			//
			// While a turn is active, FlushTurnOutput() fires inside
			// bracketed_getc (before each blocking read) so streamed
			// response chunks appear in the scroll region while the
			// user types on row N-2.
			if (!repl::ReadMessage(tui::UserPrompt(),
									tui::ContinuationPrompt(),
									line)) {
				tui::ClearInputRow();
				// EOF / Ctrl+D — wait for any active turn to finish
				// before breaking so we don't orphan the worker.
				if (turn_active) {
					std::unique_lock<std::mutex> dlk(worker.fDisplayMu);
					worker.fDisplayCv.wait(dlk, [&]{
						return !worker.fWorkerOwnsDisplay;
					});
					dlk.unlock();
					drain_turn(/*allowNotify=*/false); // user is exiting
				}
				break;
			}
			tui::ClearInputRow();
			// Trim trailing whitespace/control chars before the empty
			// check so a synthetic '\r' from the flush-timer wake path
			// (bracketed_getc returning '\r' when rl_end==0 and the
			// turn just completed) doesn't produce a phantom "> \n"
			// echo in the scroll region.
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
									|| line.back() == ' ' || line.back() == '\t')) {
				line.pop_back();
			}
			// Don't echo or dispatch an empty line — just loop back so
			// the turn-completion check at the top can drain the result.
			if (line.empty()) continue;
			tui::PositionCursorForChat();
			std::cout << tui::UserPrompt() << line << "\n" << std::flush;
			tui::PositionCursorForChat();
		}

		// Trim trailing whitespace from the pending path. The ReadMessage
		// path trims inline above; this handles the !pending.empty() branch.
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
								|| line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty()) continue;

		// ── If a turn is still active, wait for it to finish ────────
		// We must drain before doing anything — slash commands need
		// stdout restored (EndTurn) before they can print or show menus,
		// and normal prompts need the conversation to be sequential.
		if (turn_active) {
			{
				std::unique_lock<std::mutex> dlk(worker.fDisplayMu);
				worker.fDisplayCv.wait(dlk, [&]{
					return !worker.fWorkerOwnsDisplay;
				});
			}
			turn_active = false;
			// User actively typed new input, so no notification — they
			// are clearly already watching the terminal.
			if (!drain_turn(/*allowNotify=*/false)) break;
			// The user's input was already echoed into the TurnOutputBuf
			// above (line 690); EndTurn() flushed it in drain_turn().
			// No second echo needed.
		}

		// Drag-and-drop from Tracker: if libedit hands back a line
		// that's purely one-or-more absolute paths that all exist on
		// disk, treat it as an attachment event rather than a prompt.
		{
			std::vector<std::string> dropped;
			if (line_is_path_drop(line, dropped)) {
				for (auto& p : dropped) pending_paths.push_back(std::move(p));
				std::cout << tui::Meta(FormatAttachedLine(pending_paths)) << "\n";
				continue;
			}
		}

		bool recordedBySlashCmd = false;
		if (!line.empty() && line.front() == '/') {
			// /remote-control toggles the background Telegram
			// poller. commands::Dispatch doesn't know about it
			// because the REPL owns the lifecycle directly.
			auto starts_with = [](const std::string& s, const char* prefix) {
				const size_t n = std::strlen(prefix);
				return s.size() >= n && s.compare(0, n, prefix) == 0;
			};
			if (starts_with(line, "/remote-control")) {
				std::string arg;
				if (line.size() > std::strlen("/remote-control")) {
					arg = line.substr(std::strlen("/remote-control"));
					size_t a = 0, b = arg.size();
					while (a < b && (arg[a] == ' ' || arg[a] == '\t')) ++a;
					while (b > a && (arg[b - 1] == ' ' || arg[b - 1] == '\t')) --b;
					arg = arg.substr(a, b - a);
				}
				config::LogLine("slash remote-control arg='" + arg + "'");

				const bool currently_running = remote && remote->Running();
				const bool want_off = (arg == "off")
					|| (arg.empty() && currently_running);
				const bool want_on  = (arg == "on")
					|| (arg.empty() && !currently_running);

				if (want_off) {
					if (currently_running) {
						remote->Stop();
						worker.fRemote = nullptr;
						std::cout << tui::Meta("[remote control: telegram poller stopped]") << "\n";
						config::LogLine("remote control stopped from /remote-control" +
								 (arg.empty() ? std::string(" (toggle)") : std::string(" off")));
						tui::SetStatusBar(compose_status());
					} else {
						std::cout << tui::Meta("[remote control is not active]") << "\n";
					}
				} else if (want_on) {
					if (currently_running) {
						std::cout << tui::Meta("[remote control already active]") << "\n";
					} else {
						std::string why;
						if (!telegram::RemoteControl::ConfigIsValid(cfg, &why)) {
							std::cout << tui::Meta("[remote control: " + why + "]") << "\n"
									  << tui::Dim("  Add a 'telegram' block to config.json with\n"
												  "  bot_token and allowed_user_ids.\n"
												  "  See the Telegram setup section in README.md.") << "\n";
							config::LogLine("remote control config invalid: " + why);
						} else {
							try {
								if (!remote) {
									remote = std::make_unique<telegram::RemoteControl>(
										cfg,
										[&auth]{ return auth; },
										custom_system);
								}
								if (remote->Start()) {
									std::cout << tui::Meta("[remote control: telegram poller started]") << "\n";
									config::LogLine("remote control started from /remote-control");
									tui::SetStatusBar(compose_status());
									// Keep the worker's remote pointer in sync.
									worker.fRemote = remote.get();
									// Share a read-only snapshot of the local
									// messages array with the Telegram bridge so
									// Claude sees both sides of the conversation
									// on every remote turn (ping-pong fix).
									remote->SetSharedHistory([&messages]() -> json {
										return messages;
									});
									// Write-back: append each completed Telegram
									// turn (user + assistant) into the local REPL
									// messages[] so the exchange appears in the
									// scroll history and is saved to history.json.
									remote->SetSharedHistoryAppender(
										[&messages, &model, &resume_name](json user_msg, json asst_msg) {
											messages.push_back(std::move(user_msg));
											messages.push_back(std::move(asst_msg));
											config::SaveHistory(messages, model, resume_name);
										});
								} else {
									std::cout << tui::Meta("[remote control: Start() returned false — already running?]") << "\n";
								}
							} catch (const std::exception& e) {
								std::cout << tui::Meta(std::string("[remote control error: ") + e.what() + "]") << "\n";
								config::LogLine(std::string("remote control construction failed: ") + e.what());
							}
						}
					}
				} else {
					std::cout << tui::Meta("[remote control: unknown argument '" + arg
										   + "' — use /remote-control, /remote-control on, or /remote-control off]") << "\n";
				}
				repl::Record(line);
				continue;
			}

			commands::LoopCtx ctx{auth, max_tokens, custom_system, prices, model,
			                     turn_count, session_input, session_output, messages,
			                     session_urls, notify_enabled, notify_min_duration,
			                     [&]() { tui::SetStatusBar(compose_status()); }};
			ctx.resume_name             = resume_name;
			ctx.compact_auto_threshold  = compact_auto_threshold;
			ctx.compact_window_override = compact_window_override;
			std::string expanded;
			const commands::SlashAction action = commands::Dispatch(line, ctx, expanded);
			repl::Record(line);
			recordedBySlashCmd = true;
			if (action == commands::SlashAction::Quit) break;
			if (action == commands::SlashAction::Continue) continue;
			if (action == commands::SlashAction::Passthrough) {
				// Custom command resolved to a prompt; fall through
				// with the expanded text as the actual user message.
				line = std::move(expanded);
			}
		}

		if (!recordedBySlashCmd) repl::Record(line);

		if (hooks::Fire(hooks::Event::UserPromptSubmit, json{{"prompt", line}}) == hooks::Outcome::Block) {
			std::cout << tui::Meta("[hook blocked prompt]") << "\n";
			continue;
		}

		// Prepend the accumulated attachment preamble silently to
		// the outgoing API content. The replay and hooks payload
		// keep only the user's actual typed text.
		std::string api_content = line;
		if (!pending_paths.empty()) {
			api_content = ComposeAttachmentPreamble(pending_paths) + api_content;
			pending_paths.clear();
		}
		const json snapshot = messages;

		const std::string system_for_turn = config::ComposeSystem(custom_system);
		// Refresh the OAuth token if it's about to expire so
		// long-running REPL sessions don't fail mid-conversation.
		auth = config::ResolveAuth();
		if (auth.kind == config::AuthKind::None) {
			std::cout << "\n" << tui::Meta("[error: authentication expired — run /exit and `claude login`]") << "\n";
			continue;
		}

		dispatch_turn(line, std::move(api_content), std::move(snapshot), system_for_turn);
	}
	return 0;
}

} // namespace session
