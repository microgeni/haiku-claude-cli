#include "api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/stat.h>
#include <sstream>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <curl/curl.h>

#include "hooks.h"
#include "stats.h"
#include "tools.h"
#include "tui.h"

// Shared cancellation flag used across tools.cpp and api.cpp. Lives
// at global scope so the SIGINT handler (C callback) can write it.
volatile sig_atomic_t g_interrupted  = 0;

// Set when Ctrl+X (0x18) is detected during a streaming turn.
// Signals "cancel and restore input" to InteractiveLoop.
volatile sig_atomic_t g_cancel_retype = 0;

namespace api {

// Thread-local accumulator for WriteAttr calls that touch
// claude:summary during a SendWithTools invocation. Drained by
// DrainWrittenSummaryPaths() after each turn.
static thread_local std::vector<std::string> tl_written_summary_paths;

// Definitions for the globals declared in api.h.
StreamProgress* g_stream_progress = nullptr;
std::function<void(const std::string&)> g_tool_status_hook;
std::function<Permission(const std::string&, const std::string&,
                          std::atomic<bool>*)> g_telegram_permission_hook;
bool g_non_interactive_tools             = false;
bool g_non_interactive_allow_destructive = false;
bool g_allow_destructive_tools           = false;
std::atomic<bool> g_ludicrous_mode       { false };
std::atomic<bool> g_telegram_updater_paused { false };
std::map<std::string, std::string> g_last_rate_headers;

std::unordered_set<std::string>& AlwaysAllowed() {
	static std::unordered_set<std::string> s;
	return s;
}

namespace {

constexpr const char* kApiUrl      = "https://api.anthropic.com/v1/messages";
constexpr const char* kOAuthSystem = "You are Claude Code, Anthropic's official CLI for Claude.";

extern "C" void handle_sigint(int) {
	g_interrupted = 1;
}

int xfer_callback(void* /*clientp*/,
				  curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
				  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
	return g_interrupted ? 1 : 0;
}

// RAII helper that puts stdin into cbreak mode during an in-flight
// HTTP stream and spawns a background thread polling stdin for a
// bare ESC keypress. On ESC it sets g_interrupted so the existing
// xfer_callback aborts the curl transfer — the same path Ctrl+C
// uses. Destructor tears down the thread and restores termios.
//
// No-op when stdin isn't a TTY (piped input, CI, subprocess).
class EscInterruptGuard {
public:
	EscInterruptGuard() {
		if (!isatty(STDIN_FILENO)) return;
		if (tcgetattr(STDIN_FILENO, &fSaved) != 0) return;
		fSavedValid = true;

		termios raw = fSaved;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN]  = 0;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
			fSavedValid = false;
			return;
		}

		fRunning.store(true);
		fThread = std::thread([this]() {
			while (fRunning.load()) {
				// When paused, spin on a short sleep without touching
				// stdin so tui::SelectOption() has exclusive access.
				if (fPaused.load()) {
					fPausedAck.store(true);
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				fPausedAck.store(false);

				struct pollfd pfd {};
				pfd.fd     = STDIN_FILENO;
				pfd.events = POLLIN;
				const int r = ::poll(&pfd, 1, 100);
				if (!fRunning.load() || fPaused.load()) continue;
				if (r <= 0) continue;
				if (!(pfd.revents & POLLIN)) continue;

				char buf[16];
				const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
				if (n <= 0) continue;

				// A bare ESC keypress arrives as a single 0x1B byte.
				// Arrow keys, Home/End, etc. arrive as multi-byte
				// CSI sequences starting with 0x1B. Only treat the
				// lone-byte case as cancel so a twitchy arrow-key
				// press during streaming doesn't kill the turn.
				if (n == 1 && buf[0] == '\x1b') {
					g_interrupted = 1;
					return;
				}
				// Ctrl+X (0x18) — cancel and restore input to the
				// edit buffer ("amend" rather than discard).
				if (n == 1 && buf[0] == '\x18') {
					g_cancel_retype = 1;
					g_interrupted   = 1;
					return;
				}
			}
		});
	}

	~EscInterruptGuard() {
		fRunning.store(false);
		if (fThread.joinable()) fThread.join();
		if (fSavedValid) {
			tcsetattr(STDIN_FILENO, TCSANOW, &fSaved);
		}
	}

	// Temporarily stop reading stdin so another component (e.g.
	// tui::SelectOption) has exclusive access. Blocks until the
	// background thread has acknowledged the pause — confirmed to
	// be in its sleep loop and not mid-read.
	void pause() {
		fPaused.store(true);
		// Upper bound: the poll() timeout is 100 ms, so we wait at
		// most ~110 ms in the worst case.
		for (int i = 0; i < 120 && !fPausedAck.load(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	void resume() {
		fPausedAck.store(false);
		fPaused.store(false);
	}

	EscInterruptGuard(const EscInterruptGuard&) = delete;
	EscInterruptGuard& operator=(const EscInterruptGuard&) = delete;

private:
	std::atomic<bool> fRunning   { false };
	std::atomic<bool> fPaused    { false };
	std::atomic<bool> fPausedAck { false };
	std::thread       fThread;
	termios           fSaved {};
	bool              fSavedValid = false;
};

// Pointer to the currently active EscInterruptGuard (if any). Set
// in SendWithTools so PromptPermission can pause/resume it around
// tui::SelectOption() calls.
EscInterruptGuard* g_active_esc_guard = nullptr;

struct StreamState {
	std::string          sse_buffer;
	std::string          raw_buffer;
	std::string          text;
	std::atomic<int>     input_tokens        { 0 };
	std::atomic<int>     output_tokens       { 0 };
	std::atomic<int>     cache_creation_input_tokens { 0 };
	std::atomic<int>     cache_read_input_tokens     { 0 };
	bool                 saw_text            = false;
	bool                 stream_error        = false;
	std::string          stream_error_type;
	std::string          stream_error_message;
	tui::Spinner*        spinner             = nullptr;
	tui::MarkdownRenderer renderer;

	// Structured content accumulation for tool-use support.
	std::vector<json>    content_blocks;
	std::string          current_type;
	std::string          current_text;
	std::string          current_tool_id;
	std::string          current_tool_name;
	std::string          current_tool_input_raw;
	std::string          stop_reason;
};

size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* /*userp*/) {
	const size_t total = size * nitems;
	std::string  line(buffer, total);
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}
	const auto colon = line.find(':');
	if (colon == std::string::npos) return total;

	std::string name = line.substr(0, colon);
	for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (name.compare(0, 10, "anthropic-") != 0) return total;

	std::string value = line.substr(colon + 1);
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.erase(0, 1);
	}
	g_last_rate_headers[name] = value;
	return total;
}

void ProcessSseEvent(const std::string& event, StreamState* state) {
	std::string data;
	std::istringstream iss(event);
	std::string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("data:", 0) != 0) continue;
		std::string payload = line.substr(5);
		if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
		if (!data.empty()) data += '\n';
		data += payload;
	}
	if (data.empty()) return;

	try {
		const json j = json::parse(data);
		const std::string type = j.value("type", "");

		if (type == "content_block_start") {
			const auto& cb = j.value("content_block", json::object());
			state->current_type = cb.value("type", std::string{});
			state->current_text.clear();
			state->current_tool_id.clear();
			state->current_tool_name.clear();
			state->current_tool_input_raw.clear();
			if (state->current_type == "tool_use") {
				state->current_tool_id   = cb.value("id",   std::string{});
				state->current_tool_name = cb.value("name", std::string{});
			}
		} else if (type == "content_block_delta") {
			const auto& delta = j.value("delta", json::object());
			const std::string dtype = delta.value("type", std::string{});
			if (dtype == "text_delta") {
				const std::string chunk = delta.value("text", "");
				state->renderer.Write(chunk);
				state->current_text += chunk;
				state->text += chunk;
				state->saw_text = true;
				if (g_stream_progress) {
					std::lock_guard<std::mutex> lk(g_stream_progress->mu);
					g_stream_progress->text += chunk;
					g_stream_progress->version.fetch_add(1, std::memory_order_relaxed);
				}
			} else if (dtype == "input_json_delta") {
				state->current_tool_input_raw += delta.value("partial_json", "");
			}
		} else if (type == "content_block_stop") {
			if (state->current_type == "text") {
				state->content_blocks.push_back({
					{"type", "text"},
					{"text", state->current_text},
				});
			} else if (state->current_type == "tool_use") {
				json parsed_input = json::object();
				try {
					if (!state->current_tool_input_raw.empty()) {
						parsed_input = json::parse(state->current_tool_input_raw);
					}
				} catch (const json::exception&) {
					parsed_input = json::object();
				}
				state->content_blocks.push_back({
					{"type",  "tool_use"},
					{"id",    state->current_tool_id},
					{"name",  state->current_tool_name},
					{"input", parsed_input},
				});
			}
			state->current_type.clear();
		} else if (type == "message_start") {
			// Intentionally leave the spinner running — we want the
			// "(elapsed · ↑ N tokens)" tail to render during the
			// window between prompt ingestion and the first
			// text_delta. MarkdownRenderer::Write() stops the
			// spinner on first real output.
			if (j.contains("message") && j["message"].contains("usage")) {
				const auto& u = j["message"]["usage"];
				state->input_tokens.store(u.value("input_tokens",  0),
										  std::memory_order_relaxed);
				state->output_tokens.store(u.value("output_tokens", 0),
										   std::memory_order_relaxed);
				state->cache_creation_input_tokens.store(
					u.value("cache_creation_input_tokens", 0),
					std::memory_order_relaxed);
				state->cache_read_input_tokens.store(
					u.value("cache_read_input_tokens", 0),
					std::memory_order_relaxed);
			}
		} else if (type == "message_delta") {
			if (j.contains("delta") && j["delta"].contains("stop_reason")
				&& j["delta"]["stop_reason"].is_string()) {
				state->stop_reason = j["delta"]["stop_reason"].get<std::string>();
			}
			if (j.contains("usage")) {
				const auto& u = j["usage"];
				state->output_tokens.store(
					u.value("output_tokens",
							state->output_tokens.load(std::memory_order_relaxed)),
					std::memory_order_relaxed);
			}
		} else if (type == "error") {
			state->stream_error = true;
			if (j.contains("error") && j["error"].is_object()) {
				const auto& e = j["error"];
				if (e.contains("type") && e["type"].is_string())
					state->stream_error_type = e["type"].get<std::string>();
				if (e.contains("message") && e["message"].is_string())
					state->stream_error_message = e["message"].get<std::string>();
			}
		}
	} catch (const json::exception&) {
		// Ignore partial/invalid payloads (e.g. ping events).
	}
}

size_t StreamWriteCallback(char* data, size_t size, size_t nmemb, void* userp) {
	const size_t total = size * nmemb;
	auto* state = static_cast<StreamState*>(userp);
	state->raw_buffer.append(data, total);
	state->sse_buffer.append(data, total);

	size_t pos;
	while ((pos = state->sse_buffer.find("\n\n")) != std::string::npos) {
		const std::string event = state->sse_buffer.substr(0, pos);
		state->sse_buffer.erase(0, pos + 2);
		ProcessSseEvent(event, state);
	}
	return total;
}

// Session-scoped curl handle. Reused across all SendConversation
// calls so DNS cache, TLS session, and TCP connections persist
// between turns. Created lazily; cleaned up via atexit.
CURL* g_curl = nullptr;

CURL* get_curl() {
	if (!g_curl) {
		g_curl = curl_easy_init();
		std::atexit([]() {
			if (g_curl) { curl_easy_cleanup(g_curl); g_curl = nullptr; }
		});
	}
	return g_curl;
}

std::string ShortInputSummary(const json& input) {
	// For Bash, show the full command string untruncated so the user
	// can see exactly what will be executed. Other tools keep the
	// 80-char cap since their inputs (paths, patterns) are brief.
	if (input.contains("command") && input["command"].is_string()) {
		return input["command"].get<std::string>();
	}
	const std::string dumped = input.dump(-1, ' ', false, json::error_handler_t::replace);
	if (dumped.size() <= 80) return dumped;
	return dumped.substr(0, 77) + "...";
}

Permission PromptPermission(const std::string& tool_name, const json& input,
                            std::string* denial_reason = nullptr) {
	if (AlwaysAllowed().count(tool_name)) return Permission::Allow;
	if (!tools::RequiresPermission(tool_name)) return Permission::Allow;

	// Ludicrous mode: all permissions auto-approved, no prompts.
	if (g_ludicrous_mode.load()) {
		std::cout << tui::Dim("  \xE2\x9A\xA1 ludicrous: auto-approved " + tool_name) << "\n";
		return Permission::Allow;
	}

	// Telegram remote-control hook: fires for ALL turns (local or
	// Telegram-origin) when /remote-control is active. Checked
	// before the g_non_interactive_tools gate so a local turn that
	// triggers Bash still asks the Telegram user for approval.
	if (g_telegram_permission_hook) {
		const std::string extra = tools::Preview(tool_name, input);
		const std::string preview = extra.empty()
			? (tool_name + " " + ShortInputSummary(input))
			: extra;
		if (!extra.empty()) std::cout << tui::Dim(extra) << "\n";
		else std::cout << tui::Meta("  -> " + tool_name + " " + ShortInputSummary(input)) << "\n";

		const bool has_tty = isatty(fileno(stdin)) && isatty(fileno(stdout));
		// For Telegram-origin turns (g_non_interactive_tools == true)
		// we must NOT call tui::SelectOption — the main-thread REPL
		// is also reading stdin. Route straight through the Telegram
		// hook regardless of whether a TTY is attached.
		if (!has_tty || g_non_interactive_tools) {
			std::cout << tui::Bold("allow " + tool_name + "? ")
					  << tui::Dim("[awaiting Telegram response]") << "\n" << std::flush;
			const Permission p = g_telegram_permission_hook(tool_name, preview, nullptr);
			const char* lbl = (p == Permission::Allow) ? "yes" : "no";
			std::cout << tui::Dim(std::string("  -> ") + lbl) << "\n";
			return p;
		}

		// Race: show the local menu in a thread while the Telegram
		// hook waits for a button tap. A shared atomic lets the
		// winner signal the loser to stop blocking.
		std::atomic<bool> local_answered { false };
		std::atomic<bool> tg_answered    { false };
		Permission         result         { Permission::Deny };
		std::mutex         race_mu;
		std::condition_variable race_cv;

		auto tg_hook = g_telegram_permission_hook; // capture while still set
		std::thread tg_thread([&]() {
			const Permission p = tg_hook(tool_name, preview, &local_answered);
			std::unique_lock<std::mutex> lk(race_mu);
			if (!local_answered.load()) {
				result = p;
				tg_answered.store(true);
			}
			race_cv.notify_one();
		});

		const std::vector<std::string> choices = {
			"Yes",
			"Yes, allow all " + tool_name + " this session  (shift+tab)",
			"No",
		};
		std::cout << tui::Dim("[also awaiting Telegram — or answer locally]") << "\n" << std::flush;
		if (g_active_esc_guard) g_active_esc_guard->pause();
		tui::PauseFlushTimer();
		const int race_pre_lines =
			(extra.empty()
				? 1
				: static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1)
			+ 1; // the "[also awaiting Telegram]" line
		const int picked = tui::SelectOption(choices, "allow " + tool_name + "?",
											 &tg_answered, race_pre_lines);
		if (g_active_esc_guard) g_active_esc_guard->resume();
		tui::ResumeFlushTimer();
		tui::PositionCursorForChat();

		{
			std::unique_lock<std::mutex> lk(race_mu);
			if (!tg_answered.load()) {
				if (picked == -2) {
					// Tab/amend: cancel turn and restore input to edit buffer.
					g_cancel_retype = 1;
					g_interrupted   = 1;
					if (denial_reason)
						*denial_reason = "user chose to amend the prompt";
					result = Permission::Deny;
				} else if (picked == 1) {
					AlwaysAllowed().insert(tool_name);
					result = Permission::Allow;
				} else if (picked == 0) {
					result = Permission::Allow;
				} else {
					if (denial_reason)
						*denial_reason = "user declined permission for " + tool_name;
					result = Permission::Deny;
				}
				local_answered.store(true);
			}
		}
		race_cv.notify_one();

		tg_thread.join();

		const char* lbl = (result == Permission::Allow) ? "yes" : "no";
		std::cout << tui::Dim(std::string("  -> ") + lbl) << "\n";
		return result;
	}

	if (g_non_interactive_tools) {
		if (g_non_interactive_allow_destructive) return Permission::Allow;
		if (denial_reason) {
			*denial_reason =
				"destructive tool " + tool_name + " is blocked in non-interactive "
				"mode. Set \"fAllowDestructiveTools\": true in config.json "
				"(or telegram.fAllowDestructiveTools for the bridge) to allow it.";
		}
		return Permission::Deny;
	}

	// One-shot runs with no usable stdin (piped, closed, redirected)
	// cannot show a y/a/n dialog. Either auto-approve from config /
	// -y, or fail loudly with a clear reason.
	if (!isatty(fileno(stdin))) {
		if (g_allow_destructive_tools) {
			AlwaysAllowed().insert(tool_name);
			return Permission::Allow;
		}
		const std::string msg =
			"cannot prompt for " + tool_name + " permission: stdin is not a "
			"terminal. Re-run with -y/--yes to auto-approve destructive tools "
			"for this invocation, set \"fAllowDestructiveTools\": true in "
			"config.json, or use -i/--interactive from a real terminal.";
		std::cerr << tui::Meta("[tool: " + tool_name + " -> denied: no TTY to prompt]") << "\n"
				  << tui::Dim("  " + msg) << "\n";
		if (denial_reason) *denial_reason = msg;
		return Permission::Deny;
	}

	// Print the Preview + question text into the scroll region so
	// the history shows what was asked.
	const std::string extra = tools::Preview(tool_name, input);
	int pre_lines = 0;
	if (!extra.empty()) {
		std::cout << tui::Dim(extra) << "\n";
		pre_lines = static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1;
	} else {
		// Fallback for tools without a rich preview: show a compact
		// one-liner with the tool name and a summary of its input.
		const std::string action_line = "  \xE2\x86\x92 " + tool_name
		                              + "  " + ShortInputSummary(input);
		std::cout << tui::Meta(action_line) << "\n";
		pre_lines = 1;
	}

	// Derive a file basename and directory scope for the option labels.
	// e.g. path="src/tools.cpp" → basename="tools.cpp", dir_scope="src/"
	const std::string file_path = input.value("path", std::string{});
	const auto slash = file_path.rfind('/');
	const std::string basename  = (slash == std::string::npos)
	                            ? file_path : file_path.substr(slash + 1);
	const auto prev_slash = (slash == std::string::npos || slash == 0)
	                      ? std::string::npos : file_path.rfind('/', slash - 1);
	const std::string dir_scope = (!file_path.empty() && slash != std::string::npos)
	    ? file_path.substr(prev_slash == std::string::npos ? 0 : prev_slash + 1,
	                       slash - (prev_slash == std::string::npos ? 0 : prev_slash) )
	    : std::string{};

	// Natural-language question matching Claude Code's style.
	const std::string question = basename.empty()
	    ? "Do you want to proceed with " + tool_name + "?"
	    : "Do you want to make this edit to " + basename + "?";

	// Option 2: scope to directory when available, otherwise session-wide.
	const std::string allow_session_label = dir_scope.empty()
	    ? "Yes, allow all " + tool_name + " this session  (shift+tab)"
	    : "Yes, allow all " + tool_name + " in " + dir_scope + " this session  (shift+tab)";

	const std::vector<std::string> choices = {
		"Yes",
		allow_session_label,
		"No",
	};
	if (g_active_esc_guard) g_active_esc_guard->pause();
	tui::PauseFlushTimer();
	const int picked = tui::SelectOption(choices, question, nullptr, pre_lines);
	tui::ResumeFlushTimer();
	if (g_active_esc_guard) g_active_esc_guard->resume();
	tui::PositionCursorForChat();

	if (g_interrupted && picked >= static_cast<int>(choices.size()) - 1) {
		if (denial_reason) *denial_reason = "interrupted — permission denied for " + tool_name;
		return Permission::Deny;
	}
	if (picked == -2) {
		// Tab pressed — "amend": cancel the turn and restore the user's
		// original input to the edit buffer so they can retype/edit it.
		g_cancel_retype = 1;
		g_interrupted   = 1;
		if (denial_reason) *denial_reason = "user chose to amend the prompt";
		return Permission::Deny;
	}
	if (picked == 1) {
		AlwaysAllowed().insert(tool_name);
		return Permission::Allow;
	}
	if (picked == 0) return Permission::Allow;
	if (denial_reason) {
		*denial_reason = "user declined permission for " + tool_name;
	}
	return Permission::Deny;
}

} // namespace

InterruptGuard::InterruptGuard() {
	g_interrupted = 0;
	struct sigaction sa {};
	sa.sa_handler = handle_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, &fPrev);
}

InterruptGuard::~InterruptGuard() {
	sigaction(SIGINT, &fPrev, nullptr);
}

SendResult SendConversation(config::Auth auth, const std::string& model,
                            int max_tokens, const json& messages,
                            const std::string& custom_system, bool include_tools) {
	constexpr int kMaxRetries = 3;
	constexpr int kBaseDelay  = 1000; // ms; doubles on each retry

	CURL* curl = get_curl();
	if (!curl) {
		std::cerr << "error: curl_easy_init failed\n";
		return {1, {}, 0, 0, {}, {}};
	}

	// Sanitize the system prompt once before entering the retry loop.
	// CLAUDE.md files and the BFS snapshot are read as raw bytes and
	// may contain non-UTF-8 sequences. nlohmann::json::dump() throws
	// type_error.316 on any invalid byte.
	const std::string safe_system = config::SanitizeUtf8(custom_system);

	for (int attempt = 1; /* break/return inside */; ++attempt) {
	// Reset per-request state on the reused handle so stale headers
	// / callbacks from the previous call don't leak.
	curl_easy_reset(curl);

	// Mutable copy of messages so we can stamp cache_control on the
	// most recent user turn without disturbing the caller's history.
	json cached_messages = messages;
	if (!cached_messages.empty()) {
		auto& last = cached_messages.back();
		if (last.contains("content")) {
			auto& content = last["content"];
			if (content.is_string()) {
				const std::string text = content.get<std::string>();
				content = json::array({
					{
						{"type", "text"},
						{"text", text},
						{"cache_control", {{"type", "ephemeral"}}},
					},
				});
			} else if (content.is_array() && !content.empty()) {
				content.back()["cache_control"] = {{"type", "ephemeral"}};
			}
		}
	}

	json body = {
		{"model",      model},
		{"max_tokens", max_tokens},
		{"stream",     true},
		{"messages",   cached_messages},
	};
	if (include_tools) {
		body["tools"] = tools::Definitions();
	}

	// When OAuth is active, Anthropic gates the request unless the
	// system field's first entry is the Claude Code preamble.
	// Sending `system` as an array with the preamble as element 0
	// and any extra content as element 1 passes the check.
	//
	// Prompt caching: mark the LAST system block with
	// cache_control: ephemeral. Render order is tools → system →
	// messages, so one marker here caches both the tools array and
	// the system prompt together.
	if (auth.kind == config::AuthKind::OAuth) {
		json system_array = json::array();
		system_array.push_back({{"type", "text"}, {"text", kOAuthSystem}});
		if (!safe_system.empty()) {
			system_array.push_back({{"type", "text"}, {"text", safe_system}});
		}
		system_array.back()["cache_control"] = {{"type", "ephemeral"}};
		body["system"] = system_array;
	} else if (!safe_system.empty()) {
		body["system"] = json::array({
			{
				{"type", "text"},
				{"text", safe_system},
				{"cache_control", {{"type", "ephemeral"}}},
			},
		});
	}
	std::string body_str;
	try {
		body_str = body.dump();
	} catch (const json::exception& e) {
		std::cerr << "\nerror: failed to serialize request body: " << e.what() << "\n"
				  << "  (hint: a system prompt or message may contain invalid UTF-8)\n";
		return {1, {}, 0, 0, {}, {}};
	}

	curl_slist* headers = nullptr;
	if (auth.kind == config::AuthKind::OAuth) {
		headers = curl_slist_append(headers, ("authorization: Bearer " + auth.credential).c_str());
		headers = curl_slist_append(headers, (std::string("anthropic-beta: ") + config::kOAuthBeta).c_str());
	} else {
		headers = curl_slist_append(headers, ("x-api-key: " + auth.credential).c_str());
	}
	headers = curl_slist_append(headers, (std::string("anthropic-version: ") + config::kApiVersion).c_str());
	headers = curl_slist_append(headers, "content-type: application/json");
	headers = curl_slist_append(headers, "accept: text/event-stream");

	StreamState state;
	tui::Spinner spinner("thinking");
	spinner.SetLiveInputTokens(&state.input_tokens);
	state.spinner = &spinner;
	state.renderer.SetSpinner(&spinner);
	// The "claude> " label is printed by the renderer the moment the
	// first streamed character arrives — after the spinner has cleared
	// its own line — so the spinner never overwrites it.
	state.renderer.SetResponsePrefix(tui::ClaudePrompt());

	curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
	const std::string ua = std::string("haiku-claude-cli/") + config::kVersion;
	curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

	g_interrupted = 0;
	// ESC guard covers just the curl transfer. When called from
	// SendWithTools, g_active_esc_guard is already set and the outer
	// guard's thread is handling ESC detection for the whole turn —
	// creating a second concurrent reader on the same stdin fd
	// causes a race. Skip the inner guard entirely when an outer one
	// is live. When called standalone (e.g. direct one-shot), no
	// outer guard exists so we create one here.
	CURLcode res;
	{
		std::unique_ptr<EscInterruptGuard> inner_esc_guard;
		if (!g_active_esc_guard) {
			inner_esc_guard = std::make_unique<EscInterruptGuard>();
		}
		res = curl_easy_perform(curl);
		spinner.Stop();
	}
	long http_status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
	curl_slist_free_all(headers);

	if (g_interrupted) {
		state.renderer.Flush();
		std::cout << "\n" << tui::Meta("[interrupted]") << "\n";
		return {1, state.text, state.input_tokens.load(), state.output_tokens.load(),
				state.content_blocks, state.stop_reason,
				state.cache_creation_input_tokens.load(),
				state.cache_read_input_tokens.load()};
	}

	if (res != CURLE_OK) {
		// Curl-level transient errors (timeout, connection reset,
		// partial transfer) are retryable. Abort-by-callback is
		// intentional and should NOT be retried.
		const bool curl_retryable =
			res == CURLE_OPERATION_TIMEDOUT ||
			res == CURLE_COULDNT_CONNECT ||
			res == CURLE_PARTIAL_FILE ||
			res == CURLE_GOT_NOTHING ||
			res == CURLE_RECV_ERROR ||
			res == CURLE_SEND_ERROR;
		if (curl_retryable && !g_interrupted && attempt < kMaxRetries) {
			const int delay = kBaseDelay << (attempt - 1);
			std::cerr << tui::Dim("[retry " + std::to_string(attempt)
								  + "/" + std::to_string(kMaxRetries)
								  + " in " + std::to_string(delay) + "ms: "
								  + curl_easy_strerror(res) + "]")
					  << "\n";
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " curl=" + std::to_string(res));
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue; // retry
		}
		std::cerr << "\nerror: request failed: " << curl_easy_strerror(res) << "\n";
		return {1, {}, 0, 0, {}, {}};
	}

	if (http_status < 200 || http_status >= 300) {
		// Parse Anthropic's error envelope for a user-friendly message.
		std::string api_msg;
		try {
			const json err = json::parse(state.raw_buffer);
			if (err.contains("error") && err["error"].is_object()
				&& err["error"].contains("message")
				&& err["error"]["message"].is_string()) {
				api_msg = err["error"]["message"].get<std::string>();
			}
		} catch (const json::exception&) {}

		// 401 with an OAuth token means the access token has expired
		// mid-session (common in long-running Telegram bridge sessions).
		// Attempt a silent token refresh and retry once before giving up.
		if (http_status == 401
			&& auth.kind == config::AuthKind::OAuth
			&& !g_interrupted
			&& attempt < kMaxRetries) {
			std::cerr << tui::Dim("[HTTP 401 — refreshing OAuth token and retrying]") << "\n";
			config::LogLine("HTTP 401 — attempting token refresh (attempt="
					 + std::to_string(attempt) + ")");
			const config::Auth refreshed = config::ResolveAuth();
			if (refreshed.kind == config::AuthKind::OAuth) {
				auth = refreshed;
				continue; // retry with the new token; no delay needed
			}
			// Refresh failed (no stored tokens or refresh rejected) —
			// fall through to the hard-error path below.
			std::cerr << tui::Dim("[token refresh failed — cannot recover]") << "\n";
			config::LogLine("token refresh failed after 401");
		}

		// 429 (rate limit) and 5xx (server errors) are transient —
		// retry with exponential backoff before giving up.
		const bool http_retryable =
			(http_status == 429 || http_status >= 500) && !g_interrupted;
		if (http_retryable && attempt < kMaxRetries) {
			const int delay = kBaseDelay << (attempt - 1);
			std::cerr << tui::Dim("[retry " + std::to_string(attempt)
								  + "/" + std::to_string(kMaxRetries)
								  + " in " + std::to_string(delay) + "ms: HTTP "
								  + std::to_string(http_status) + "]")
					  << "\n";
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " http=" + std::to_string(http_status));
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue; // retry
		}

		std::cerr << "\n" << tui::ErrorLabel() << " ";
		switch (http_status) {
			case 401:
				std::cerr << "unauthorized (HTTP 401) — token refresh failed. "
							 "Run `claude logout` then `claude login` to re-authenticate.";
				break;
			case 403:
				std::cerr << "forbidden (HTTP 403) — this client or account is not "
							 "permitted to use the endpoint.";
				break;
			case 429:
				if (api_msg == "Error") {
					std::cerr << "gated (HTTP 429) — Anthropic's OAuth-client check "
								 "rejected the request shape; see project notes.";
				} else {
					std::cerr << "rate limited (HTTP 429)";
					if (!api_msg.empty()) std::cerr << ": " << api_msg;
				}
				break;
			case 500: case 502: case 503: case 504:
				std::cerr << "server error (HTTP " << http_status << ")";
				if (!api_msg.empty()) std::cerr << ": " << api_msg;
				break;
			default:
				std::cerr << "HTTP " << http_status;
				if (!api_msg.empty()) std::cerr << ": " << api_msg;
				break;
		}
		std::cerr << "\n";
		config::LogLine("error http=" + std::to_string(http_status)
				 + (api_msg.empty() ? "" : " msg=" + api_msg));
		return {1, {}, 0, 0, {}, {}};
	}

	if (state.stream_error) {
		// Overloaded errors arrive as SSE error events after HTTP 200, so the
		// HTTP-level retry path above never fires.  Treat them the same way:
		// retry with exponential backoff, then give a friendly final message.
		const bool stream_retryable =
			(state.stream_error_type == "overloaded_error"
			 || state.stream_error_type == "api_error")
			&& !g_interrupted && attempt < kMaxRetries;
		if (stream_retryable) {
			const int delay = kBaseDelay << (attempt - 1);
			std::cerr << tui::Dim("[retry " + std::to_string(attempt)
								  + "/" + std::to_string(kMaxRetries)
								  + " in " + std::to_string(delay) + "ms: "
								  + state.stream_error_type + "]")
					  << "\n";
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " stream_error=" + state.stream_error_type);
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue; // retry
		}

		// Non-retryable or retries exhausted — surface a clear message.
		std::cerr << "\n" << tui::ErrorLabel() << " ";
		if (state.stream_error_type == "overloaded_error") {
			std::cerr << "Anthropic's servers are overloaded — please try again in a moment.";
		} else {
			std::cerr << "stream error";
			if (!state.stream_error_type.empty())
				std::cerr << " (" << state.stream_error_type << ")";
			if (!state.stream_error_message.empty())
				std::cerr << ": " << state.stream_error_message;
		}
		std::cerr << "\n";
		config::LogLine("stream_error type=" + state.stream_error_type
				 + (state.stream_error_message.empty()
					? "" : " msg=" + state.stream_error_message));
		return {1, state.text, state.input_tokens.load(), state.output_tokens.load(),
				state.content_blocks, state.stop_reason,
				state.cache_creation_input_tokens.load(),
				state.cache_read_input_tokens.load()};
	}

	if (state.content_blocks.empty()) {
		// Empty is fine if the turn ended cleanly — but attribute
		// the cause. The most common reason in practice is a
		// max_tokens cap so tight that the first tool_use block
		// never got a content_block_stop event.
		if (state.stop_reason == "max_tokens") {
			return {0, state.text, state.input_tokens.load(), state.output_tokens.load(),
					state.content_blocks, state.stop_reason,
					state.cache_creation_input_tokens.load(),
					state.cache_read_input_tokens.load()};
		}
		std::cerr << "error: no content received in stream\n";
		std::cerr << "response body: " << state.raw_buffer << "\n";
		return {1, {}, 0, 0, {}, {}};
	}

	state.renderer.Flush();
	std::cout << "\n";
	return {0, state.text, state.input_tokens.load(), state.output_tokens.load(),
			state.content_blocks, state.stop_reason,
			state.cache_creation_input_tokens.load(),
			state.cache_read_input_tokens.load()};

	} // end retry loop — only reached via continue; all exits are return
}

SendResult SendWithTools(const config::Auth& auth, const std::string& model,
                         int max_tokens, json& messages,
                         const std::string& custom_system) {
	SendResult aggregate;
	aggregate.exit_code = 0;

	// Clear any stale interrupt and flush the tty input queue BEFORE
	// starting the EscInterruptGuard thread. This eliminates the
	// race where a stale ESC byte left in the kernel tty buffer is
	// read by the new guard thread and sets g_interrupted=1 AFTER
	// the main thread's g_interrupted=0 clear.
	g_interrupted = 0;
	if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);

	// Keep stdin in cbreak mode for the entire multi-turn tool loop
	// so Esc is detected not just during HTTP streaming but also
	// while tools are executing (Bash, WebFetch, etc.).
	EscInterruptGuard esc_guard;
	g_active_esc_guard = &esc_guard;
	// Clear the global pointer when SendWithTools returns so nobody
	// holds a dangling reference to esc_guard after it is destroyed.
	struct EscGuardScope {
		~EscGuardScope() { g_active_esc_guard = nullptr; }
	} esc_guard_scope;

	while (true) {
		if (g_interrupted) {
			std::cout << tui::Meta("[interrupted]") << "\n";
			aggregate.exit_code = 1;
			return aggregate;
		}

		SendResult result = SendConversation(auth, model, max_tokens, messages,
											  custom_system, /*include_tools=*/true);
		aggregate.input_tokens                 += result.input_tokens;
		aggregate.output_tokens                += result.output_tokens;
		aggregate.cache_creation_input_tokens  += result.cache_creation_input_tokens;
		aggregate.cache_read_input_tokens      += result.cache_read_input_tokens;
		aggregate.assistant_text = result.assistant_text;
		aggregate.stop_reason    = result.stop_reason;

		if (result.exit_code != 0) {
			aggregate.exit_code = result.exit_code;
			return aggregate;
		}

		// If the response was cut off at the max_tokens cap, any
		// tool_use block in the round is almost certainly incomplete
		// (partial JSON input) and never got executed. Drop those
		// orphan blocks from the history so a REPL continuation
		// doesn't send an assistant turn with tool_use_ids that have
		// no matching tool_result — the API rejects that with 400.
		const bool truncated = (result.stop_reason == "max_tokens");
		if (truncated) {
			std::vector<json> safe_blocks;
			for (const auto& block : result.content_blocks) {
				if (block.value("type", "") != "tool_use") {
					safe_blocks.push_back(block);
				}
			}
			if (safe_blocks.empty()) {
				// Nothing salvageable — don't push an empty assistant turn.
			} else {
				messages.push_back({{"role", "assistant"}, {"content", safe_blocks}});
			}
		} else {
			messages.push_back({{"role", "assistant"}, {"content", result.content_blocks}});
		}

		if (result.stop_reason != "tool_use") {
			// Surface non-normal terminations loudly instead of
			// exiting silently.
			if (result.stop_reason == "max_tokens") {
				const int used = result.output_tokens;
				std::cerr << "\n" << tui::ErrorLabel()
						  << " response truncated at the max_tokens cap"
						  << " (output=" << used << " / max=" << max_tokens << ")."
						  << "\n  Re-run with -t N (or set \"max_tokens\" in config.json)"
							 " to raise the cap."
						  << (truncated ? "\n  The in-flight tool call was"
										  " dropped from history; the file/command"
										  " it would have produced was NOT executed."
										: "")
						  << "\n";
				config::LogLine("truncated stop_reason=max_tokens output=" + std::to_string(used));
			} else if (result.stop_reason == "refusal") {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " the model declined to answer (stop_reason=refusal).\n";
				config::LogLine("stop_reason=refusal");
			} else if (result.stop_reason == "pause_turn") {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " the model paused its turn (stop_reason=pause_turn);"
							 " re-send to continue.\n";
				config::LogLine("stop_reason=pause_turn");
			} else if (result.stop_reason != "end_turn"
					   && result.stop_reason != "stop_sequence"
					   && !result.stop_reason.empty()) {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " unexpected stop_reason=" << result.stop_reason << "\n";
				config::LogLine("stop_reason=" + result.stop_reason);
			}
			return aggregate;
		}

		json tool_results = json::array();
		for (const auto& block : result.content_blocks) {
			if (block.value("type", "") != "tool_use") continue;
			const std::string tname = block.value("name", std::string{});
			const std::string tid   = block.value("id",   std::string{});
			const json        tinput = block.value("input", json::object());

			const std::string tool_notice = "[tool: " + tname + " " + ShortInputSummary(tinput) + "]";
			std::cout << tui::Meta(tool_notice) << "\n";
			config::LogLine("tool " + tname + " input=" + ShortInputSummary(tinput));
			// Notify Telegram: tool about to run. Include the rich
			// preview (diff, file header, etc.) when available so the
			// remote user sees what they are about to approve.
			if (g_tool_status_hook) {
				const std::string preview = tools::Preview(tname, tinput);
				const std::string notice  = preview.empty()
					? tool_notice
					: tool_notice + "\n" + preview;
				g_tool_status_hook(notice);
			}

			tools::ToolResult tres;
			const json pre_payload = { {"tool_input", tinput} };
			if (hooks::Fire(hooks::Event::PreToolUse, pre_payload, tname) == hooks::Outcome::Block) {
				tres.content  = "hook blocked " + tname;
				tres.is_error = true;
				const std::string blocked_notice = "[tool: " + tname + " -> blocked by hook]";
				std::cout << tui::Meta(blocked_notice) << "\n";
				if (g_tool_status_hook) g_tool_status_hook(blocked_notice);
			} else if (std::string denial;
					   PromptPermission(tname, tinput, &denial) == Permission::Deny) {
				tres.content  = denial.empty()
								? "user denied permission to run " + tname
								: denial;
				tres.is_error = true;
				const std::string denied_notice = "[tool: " + tname + " -> denied]";
				std::cout << tui::Meta(denied_notice) << "\n";
				if (g_tool_status_hook) g_tool_status_hook(denied_notice);
			} else if (tname == "Task") {
				// Spawn a no-tools sub-agent: fresh messages array,
				// single round-trip via SendConversation. Streams to
				// the terminal like a normal turn.
				const std::string sub_prompt = tinput.value("prompt", std::string{});
				if (sub_prompt.empty()) {
					tres.content  = "error: Task requires a `prompt` argument";
					tres.is_error = true;
				} else {
					const std::string sub_label = tinput.value("description", std::string{"sub-agent"});
					std::cout << tui::Meta("  -> " + sub_label + ":") << "\n"
							  << tui::ClaudePrompt();
					json sub_messages = json::array({{{"role", "user"}, {"content", sub_prompt}}});
					const auto sub = SendConversation(auth, model, max_tokens,
													   sub_messages, custom_system,
													   /*include_tools=*/false);
					std::cout << "\n";
					if (sub.exit_code != 0) {
						tres.content  = "error: sub-agent failed";
						tres.is_error = true;
					} else {
						tres.content  = sub.assistant_text;
						tres.is_error = false;
						aggregate.input_tokens  += sub.input_tokens;
						aggregate.output_tokens += sub.output_tokens;
					}
				}
				const std::string task_result_notice = tres.is_error
					? "[tool: Task -> error]"
					: "[tool: Task -> " + std::to_string(tres.content.size()) + " bytes]";
				std::cout << tui::Meta(task_result_notice) << "\n";
				if (g_tool_status_hook) g_tool_status_hook(task_result_notice);
				const json post_payload = {
					{"tool_input",  tinput},
					{"tool_result", tres.content},
					{"is_error",    tres.is_error},
				};
				hooks::Fire(hooks::Event::PostToolUse, post_payload, tname);
			} else {
				// Keep a spinner spinning for the duration of the
				// tool run so the user sees continuous feedback.
				if (g_stream_progress) {
					std::lock_guard<std::mutex> lk(g_stream_progress->mu);
					g_stream_progress->tool_phase = "\xF0\x9F\x94\xA7 running " + tname + "\xE2\x80\xA6"; // 🔧 running ToolName…
				}
				{
					tui::Spinner tool_spinner("running " + tname);
					tres = tools::Run(tname, tinput);
					tool_spinner.Stop();
				}
				if (g_stream_progress) {
					std::lock_guard<std::mutex> lk(g_stream_progress->mu);
					g_stream_progress->tool_phase.clear();
				}
				const std::string rsize = std::to_string(tres.content.size());
				const std::string result_notice = tres.is_error
					? "[tool: " + tname + " -> error]"
					: "[tool: " + tname + " -> " + rsize + " bytes]";
				std::cout << tui::Meta(result_notice) << "\n";
				if (g_tool_status_hook) g_tool_status_hook(result_notice);
				const json post_payload = {
					{"tool_input",  tinput},
					{"tool_result", tres.content},
					{"is_error",    tres.is_error},
				};
				hooks::Fire(hooks::Event::PostToolUse, post_payload, tname);
			}

			// Auto-seed BFS cache: if Claude just Read a file without
			// a claude:summary attribute, write a heuristic summary
			// derived from the content already in memory.
			if (tname == "Read" && !tres.is_error) {
				config::AutoWriteSummaryIfMissing(
					tinput.value("path", std::string{}),
					tres.content);
			}

			// Track WriteAttr calls that touch claude:summary so the
			// LocalWorker can refresh the in-process snapshot after
			// the turn without a full filesystem walk.
#ifdef __HAIKU__
			if (tname == "WriteAttr" && !tres.is_error) {
				const std::string attr_name = tinput.value("name", std::string{});
				if (attr_name == "claude:summary") {
					const std::string attr_path = tinput.value("path", std::string{});
					if (!attr_path.empty())
						tl_written_summary_paths.push_back(attr_path);
				}
			}
#endif

			// For BFS-native tools, measure actual bytes saved by
			// stat-ing the target file(s) and subtracting the tool's
			// own output size.
			long savedBytes = 0;
			if (tname == "ReadAttr") {
				const std::string path = tinput.value("path", std::string{});
				struct stat st;
				if (!path.empty() && ::stat(path.c_str(), &st) == 0) {
					const long s = static_cast<long>(st.st_size)
								 - static_cast<long>(tres.content.size());
					if (s > 0) savedBytes = s;
				}
			} else if (tname == "Query") {
				long total = 0;
				std::istringstream iss(tres.content);
				std::string p;
				while (std::getline(iss, p)) {
					if (p.empty()) continue;
					struct stat st;
					if (::stat(p.c_str(), &st) == 0) total += st.st_size;
				}
				const long s = total - static_cast<long>(tres.content.size());
				if (s > 0) savedBytes = s;
			}
			stats::RecordTool(tname, static_cast<int>(tres.content.size()), savedBytes);

			// Sanitize tool output before handing it to nlohmann::json.
			// Binary tool output will throw json::type_error.316 if
			// passed raw.
			tres.content = config::SanitizeUtf8(tres.content);

			tool_results.push_back({
				{"type",        "tool_result"},
				{"tool_use_id", tid},
				{"content",     tres.content},
				{"is_error",    tres.is_error},
			});
		}

		if (tool_results.empty()) {
			// stop_reason said tool_use but no tool_use blocks — bail to avoid a loop.
			return aggregate;
		}
		messages.push_back({{"role", "user"}, {"content", tool_results}});
	}
}

std::vector<std::string> DrainWrittenSummaryPaths() {
	std::vector<std::string> out;
	out.swap(tl_written_summary_paths);
	return out;
}

} // namespace api
