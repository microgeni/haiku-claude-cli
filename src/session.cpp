#include "session.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
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
		~StatusFrameGuard() { tui::TeardownStatusBar(); }
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

	std::cout << tui::Bold("Claude CLI interactive mode") << tui::Dim(" (model: " + model + ")") << ".\n"
			  << tui::Dim("Type /help for commands, /exit or Ctrl+D to leave.") << "\n"
			  << tui::Dim(IsSshSession()
				  ? "Multi-line input: \\ + Enter  [Ctrl+J/Alt+Enter may not work over SSH]."
				  : "Multi-line input: Ctrl+J or Alt+Enter (or \\ + Enter).") << "\n\n";

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

	while (true) {
		// Resize events rebuild the scroll region and redraw the
		// fixed rows so the frame stays correct after the user
		// drags the terminal window.
		if (tui::ConsumeResizePending()) tui::RedrawStatusBar();
		tui::ShowCursor();
		tui::PositionCursorForInput();

		std::string line;
		if (!pending.empty()) {
			line    = std::move(pending);
			pending.clear();
			// Erase the input row, echo "> line" into the scroll
			// region at N-4 (where \n scrolls it into history),
			// then position at N-4 for spinner / response.
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
			// After ReadMessage() returns we:
			//  1. Erase N-2 (wipes "> hi" from the fixed row).
			//  2. Echo "> hi\n" at the scroll-region bottom N-4 so
			//     it scrolls into history.
			//  3. Position cursor at N-4 for spinner / response.
			if (!repl::ReadMessage(tui::UserPrompt(),
									tui::ContinuationPrompt(),
									line)) {
				tui::ClearInputRow();
				break;
			}
			tui::ClearInputRow();
			tui::PositionCursorForChat();
			std::cout << tui::UserPrompt() << line << "\n" << std::flush;
			tui::PositionCursorForChat();
		}

		while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
			line.pop_back();
		}
		if (line.empty()) continue;

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
		messages.push_back({{"role", "user"}, {"content", api_content}});

		const auto turn_start = std::chrono::steady_clock::now();
		const std::string system_for_turn = config::ComposeSystem(custom_system);
		// Refresh the OAuth token if it's about to expire so
		// long-running REPL sessions don't fail mid-conversation.
		auth = config::ResolveAuth();
		if (auth.kind == config::AuthKind::None) {
			std::cout << "\n" << tui::Meta("[error: authentication expired — run /exit and `claude login`]") << "\n";
			messages = snapshot;
			continue;
		}

		// StreamProgress lives here so its lifetime covers the
		// entire window [AcquireTurn … ReleaseTurn].
		api::StreamProgress stream_progress;
		api::SendResult result;

		if (remote && remote->Running()) {
			// Correct ordering for bidirectional Telegram/console turns:
			//
			//  1. AcquireTurn() — block until any running Telegram
			//     turn finishes.  Everything below is protected by
			//     the turn lock so no Telegram turn can start.
			//
			//  2. Set g_stream_progress while the lock is held so
			//     a Telegram turn that starts right after
			//     ReleaseTurn() cannot race our assignment.
			//
			//  3. MirrorPrompt() — now safe: no Telegram updater
			//     thread is running, and g_stream_progress already
			//     points to our StreamProgress.
			//
			//  4. StartThinkingUpdater(&stream_progress) — pass the
			//     pointer directly; the thread never touches the
			//     global and cannot pick up a stale or wrong object.
			//
			//  5. SendWithTools()
			//
			//  6. StopThinkingUpdater() — joins the updater thread
			//     while we still hold the turn lock and while
			//     stream_progress is still on our stack.
			//
			//  7. g_stream_progress = nullptr — nulled before
			//     ReleaseTurn() so an incoming Telegram turn never
			//     races our write.
			//
			//  8. ReleaseTurn() — Telegram turns may now proceed;
			//     g_stream_progress is guaranteed null.
			//
			//  9. MirrorToPrimary() / MirrorCancel() — outside the
			//     lock, just Telegram API calls.
			remote->AcquireTurn();
			api::g_stream_progress = &stream_progress;
			remote->MirrorPrompt(line);
			remote->StartThinkingUpdater(&stream_progress);
			result = api::SendWithTools(auth, model, max_tokens, messages, system_for_turn);
			remote->StopThinkingUpdater();
			api::g_stream_progress = nullptr;
			remote->ReleaseTurn();
		} else {
			result = api::SendWithTools(auth, model, max_tokens, messages, system_for_turn);
		}

		const double elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - turn_start).count();

		if (result.exit_code != 0) {
			messages = snapshot;
			if (remote && remote->Running())
				remote->MirrorCancel();
			continue;
		}

		++turn_count;
		session_input  += result.input_tokens;
		session_output += result.output_tokens;
		stats::RecordTurn(result.input_tokens, result.output_tokens,
						  result.cache_read_input_tokens,
						  result.cache_creation_input_tokens);

		// Harvest any URLs Claude mentioned so `/open N` can launch
		// them later. Dedup in insertion order so the index stays
		// stable across turns.
		for (auto& url : notify::ExtractUrls(result.assistant_text)) {
			if (std::find(session_urls.begin(), session_urls.end(), url)
				== session_urls.end()) {
				session_urls.push_back(std::move(url));
			}
		}

		// Desktop notification for slow turns.
		if (notify_enabled && elapsed >= notify_min_duration) {
			notify::Send(
				notify::PickPlayfulTitle(elapsed),
				notify::FirstSentence(result.assistant_text, 120));
		}

		// Auto-compact: if this turn's input-token count crosses
		// the threshold share of the model's context window, fire
		// /compact immediately (no confirmation prompt).
		if (compact_auto_threshold > 0.0) {
			const int window = models::DetectContextWindow(model, compact_window_override);
			const int trigger = static_cast<int>(window * compact_auto_threshold);
			if (result.input_tokens >= trigger) {
				char note[160];
				std::snprintf(note, sizeof(note),
					"[auto-compact: context at %d%% (%d / %d tokens) — compacting now]",
					(result.input_tokens * 100) / window,
					result.input_tokens, window);
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

		char cache_tail[64] = {0};
		const int c_read  = result.cache_read_input_tokens;
		const int c_write = result.cache_creation_input_tokens;
		if (c_read > 0 || c_write > 0) {
			std::snprintf(cache_tail, sizeof(cache_tail),
				"  \xC2\xB7 cache R:%d W:%d", c_read, c_write);
		}
		config::LogLine("turn " + std::to_string(turn_count)
				 + " model=" + model
				 + " in=" + std::to_string(result.input_tokens)
				 + " out=" + std::to_string(result.output_tokens)
				 + " cache_r=" + std::to_string(c_read)
				 + " cache_w=" + std::to_string(c_write));

		// Push the updated session counters into the fixed-bottom
		// status row.
		tui::SetStatusBar(compose_status());

		// Mirror the local turn to the primary Telegram chat.
		if (remote && remote->Running()) {
			remote->MirrorToPrimary(result.assistant_text);
		}

		config::SaveHistory(messages, model, resume_name);

		hooks::Fire(hooks::Event::Stop, json{{"assistant_text", result.assistant_text}});
	}
	return 0;
}

} // namespace session
