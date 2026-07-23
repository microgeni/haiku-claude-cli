#include "api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/stat.h>
#include <sstream>
#include <termios.h>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <unistd.h>

#include <curl/curl.h>

#include "agents.h"
#include "editor_integration.h"
#include "hooks.h"
#include "output_sink.h"
#include "repl.h"
#include "sse_parser.h"
#include "stats.h"
#include "terminal_sink.h"
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
StreamProgress* g_stream_progress        = nullptr; // local-mirror only
bool g_allow_destructive_tools           = false;
std::atomic<bool> g_ludicrous_mode       { false };
std::atomic<int>  g_thinking_budget      { 0 };
std::atomic<bool> g_plan_mode            { false };
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
class EscInterruptGuard : public EscInterruptGuardHandle {
public:
	EscInterruptGuard() {
		// Always operate on the real tty fd, not STDIN_FILENO, because
		// BlockStdin() may redirect STDIN_FILENO to a blocking pipe while
		// we are alive.  repl::RealTtyFd() returns the /dev/tty fd
		// created at Init() time, which is always the actual terminal.
		const int tty = repl::RealTtyFd() >= 0 ? repl::RealTtyFd() : STDIN_FILENO;
		if (!isatty(tty)) return;
		fTtyFd = tty;
		if (tcgetattr(fTtyFd, &fSaved) != 0) return;
		fSavedValid = true;

		termios raw = fSaved;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN]  = 0;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(fTtyFd, TCSANOW, &raw) != 0) {
			fSavedValid = false;
			return;
		}

		// Self-pipe used by pause() to kick the background thread out of
		// its poll() call immediately rather than waiting up to 100 ms
		// for the poll timeout to expire.  Without this, pause() could
		// time out before the thread acknowledged, leaving both the
		// guard thread and SelectOption() reading from the same tty fd
		// concurrently — causing SelectOption() to hang forever because
		// the guard thread stole the keypress.
		if (::pipe(fWakePipe) == 0) {
			::fcntl(fWakePipe[0], F_SETFD, FD_CLOEXEC);
			::fcntl(fWakePipe[1], F_SETFD, FD_CLOEXEC);
			::fcntl(fWakePipe[1], F_SETFL,
				::fcntl(fWakePipe[1], F_GETFL) | O_NONBLOCK);
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

				struct pollfd pfds[2] {};
				pfds[0].fd     = fTtyFd;      // real tty — ESC/Ctrl+X detection
				pfds[0].events = POLLIN;
				pfds[1].fd     = fWakePipe[0]; // pause() wake-pipe
				pfds[1].events = POLLIN;
				const int nfds = (fWakePipe[0] >= 0) ? 2 : 1;
				const int r    = ::poll(pfds, nfds, 100);
				if (!fRunning.load()) break;

				// Drain the wake pipe if it fired (pause() or destructor).
				if (nfds == 2 && (pfds[1].revents & POLLIN)) {
					char discard[16];
					::read(fWakePipe[0], discard, sizeof(discard));
				}

				// Re-check pause after the wake — pause() may have fired.
				if (fPaused.load()) continue;

				if (r <= 0) continue;
				if (!(pfds[0].revents & POLLIN)) continue;

				char buf[16];
				const ssize_t n = ::read(fTtyFd, buf, sizeof(buf));
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
		// Wake the thread out of poll() so it exits promptly.
		if (fWakePipe[1] >= 0) {
			const char b = 1;
			::write(fWakePipe[1], &b, 1);
		}
		if (fThread.joinable()) fThread.join();
		if (fWakePipe[0] >= 0) { ::close(fWakePipe[0]); fWakePipe[0] = -1; }
		if (fWakePipe[1] >= 0) { ::close(fWakePipe[1]); fWakePipe[1] = -1; }
		if (fSavedValid) {
			tcsetattr(fTtyFd, TCSANOW, &fSaved);
		}
	}

	// Temporarily stop reading stdin so another component (e.g.
	// tui::SelectOption) has exclusive access. Writes to the wake-pipe
	// to kick the background thread out of poll() immediately, then
	// waits for the acknowledgement — guaranteed within one loop
	// iteration (~10 ms) rather than the old ~110 ms worst case.
	void pause() {
		fPaused.store(true);
		// Kick the thread out of its poll() so it checks fPaused ASAP.
		if (fWakePipe[1] >= 0) {
			const char b = 1;
			::write(fWakePipe[1], &b, 1);
		}
		// Wait for the thread to acknowledge — it will do so within one
		// 10 ms sleep cycle after exiting poll().  200 iterations = 2 s
		// upper bound (should never be needed in practice).
		for (int i = 0; i < 200 && !fPausedAck.load(); ++i)
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
	int               fTtyFd      = STDIN_FILENO; // real tty fd (never the block pipe)
	int               fWakePipe[2] = {-1, -1};    // self-pipe: pause() → thread
};

} // anonymous namespace

// Definition of the global declared extern in api.h.
EscInterruptGuardHandle* g_active_esc_guard = nullptr;

// StreamState and ProcessSseEvent moved to sse_parser.{h,cpp} so the SSE
// state machine can be unit-tested without a live connection. Included via
// api.h below; the anonymous-namespace callbacks here (HeaderCallback,
// StreamWriteCallback) still drive it.

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

// Per-thread curl handle. Reused across all SendConversation calls
// *on the same thread* so DNS cache, TLS session, and TCP connections
// persist between turns. The GUI runs SendWithTools on a worker thread
// while the main thread may start the next turn or load a session, so a
// single process-global handle would be shared across threads — which
// libcurl forbids and which segfaults inside curl_easy_perform(). Making
// the handle thread_local gives each worker its own handle and keeps the
// CLI (single-threaded) behaviour identical. Each thread cleans up its
// own handle on exit via a thread_local guard object.
thread_local CURL* g_curl = nullptr;

namespace {
// Cleans up the calling thread's curl handle when the thread exits.
struct CurlThreadCleanup {
	~CurlThreadCleanup() {
		if (g_curl) { curl_easy_cleanup(g_curl); g_curl = nullptr; }
	}
};
thread_local CurlThreadCleanup g_curlCleanup;
}

CURL* get_curl() {
	if (!g_curl) {
		g_curl = curl_easy_init();
		// Touch the thread_local guard so it is instantiated and will
		// run its destructor when this thread exits.
		(void)&g_curlCleanup;
	}
	return g_curl;
}

void GlobalInit() {
	static std::once_flag once;
	std::call_once(once, []() {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		std::atexit([]() { curl_global_cleanup(); });
	});
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
                            const std::string& custom_system, bool include_tools,
                            OutputSink* sink_in) {
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

	// Create the terminal sink once for this conversation.
	// SendWithTools passes its own sink; standalone calls get a fresh one.
	std::unique_ptr<TerminalSink> owned_sink;
	if (!sink_in) {
		owned_sink = std::make_unique<TerminalSink>();
		sink_in    = owned_sink.get();
	}
	OutputSink& sink = *sink_in;

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

	// Extended thinking: when a budget is set, the model reasons in a
	// visible "thinking" block before answering. The API requires
	// max_tokens > budget_tokens (the budget is drawn from the same
	// output allowance), so bump max_tokens to leave room for a real
	// reply if the caller's cap is too tight. Thinking also forces
	// temperature=1, so we simply never send temperature.
	const int think_budget = g_thinking_budget.load(std::memory_order_relaxed);
	if (think_budget > 0) {
		int effective_max = max_tokens;
		if (effective_max <= think_budget)
			effective_max = think_budget + 4096;
		body["max_tokens"] = effective_max;
		body["thinking"] = {
			{"type",          "enabled"},
			{"budget_tokens", think_budget},
		};
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
	state.sink = &sink;
	// Wire the live token counter to the sink's spinner.
	if (auto* ts = dynamic_cast<TerminalSink*>(&sink))
		ts->SetLiveInputTokens(&state.input_tokens);

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
		// Stop the spinner (lives in TerminalSink) now that the HTTP
		// transfer is done.
		if (auto* ts = dynamic_cast<TerminalSink*>(&sink)) ts->Flush();
	}
	long http_status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
	curl_slist_free_all(headers);

	if (g_interrupted) {
		std::cout << "\n";
		sink.OnMeta("[interrupted]");
		return {1, state.text, state.input_tokens.load(), state.output_tokens.load(),
				state.content_blocks, state.stop_reason,
				state.cache_creation_input_tokens.load(),
				state.cache_read_input_tokens.load()};
	}

	if (res != CURLE_OK) {
		const bool curl_retryable =
			res == CURLE_OPERATION_TIMEDOUT ||
			res == CURLE_COULDNT_CONNECT ||
			res == CURLE_PARTIAL_FILE ||
			res == CURLE_GOT_NOTHING ||
			res == CURLE_RECV_ERROR ||
			res == CURLE_SEND_ERROR;
		if (curl_retryable && !g_interrupted && attempt < kMaxRetries) {
			const int delay = kBaseDelay << (attempt - 1);
			sink.OnDiag("[retry " + std::to_string(attempt)
			            + "/" + std::to_string(kMaxRetries)
			            + " in " + std::to_string(delay) + "ms: "
			            + curl_easy_strerror(res) + "]");
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " curl=" + std::to_string(res));
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue;
		}
		sink.OnError(std::string("request failed: ") + curl_easy_strerror(res));
		return {1, {}, 0, 0, {}, {}};
	}

	if (http_status < 200 || http_status >= 300) {
		std::string api_msg;
		try {
			const json err = json::parse(state.raw_buffer);
			if (err.contains("error") && err["error"].is_object()
				&& err["error"].contains("message")
				&& err["error"]["message"].is_string()) {
				api_msg = err["error"]["message"].get<std::string>();
			}
		} catch (const json::exception&) {}

		if (http_status == 401
			&& auth.kind == config::AuthKind::OAuth
			&& !g_interrupted
			&& attempt < kMaxRetries) {
			sink.OnDiag("[HTTP 401 — refreshing OAuth token and retrying]");
			config::LogLine("HTTP 401 — attempting token refresh (attempt="
					 + std::to_string(attempt) + ")");
			const config::Auth refreshed = config::ResolveAuth();
			if (refreshed.kind == config::AuthKind::OAuth) {
				auth = refreshed;
				continue;
			}
			sink.OnDiag("[token refresh failed — cannot recover]");
			config::LogLine("token refresh failed after 401");
		}

		const bool http_retryable =
			(http_status == 429 || http_status >= 500) && !g_interrupted;
		if (http_retryable && attempt < kMaxRetries) {
			const int delay = kBaseDelay << (attempt - 1);
			sink.OnDiag("[retry " + std::to_string(attempt)
			            + "/" + std::to_string(kMaxRetries)
			            + " in " + std::to_string(delay) + "ms: HTTP "
			            + std::to_string(http_status) + "]");
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " http=" + std::to_string(http_status));
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue;
		}

		std::string err_msg;
		switch (http_status) {
			case 401:
				err_msg = "unauthorized (HTTP 401) — token refresh failed. "
				          "Run `claude logout` then `claude login` to re-authenticate.";
				break;
			case 403:
				err_msg = "forbidden (HTTP 403) — this client or account is not "
				          "permitted to use the endpoint.";
				break;
			case 429:
				err_msg = (api_msg == "Error")
				    ? "gated (HTTP 429) — Anthropic's OAuth-client check rejected "
				      "the request shape; see project notes."
				    : "rate limited (HTTP 429)" + (api_msg.empty() ? "" : ": " + api_msg);
				break;
			case 500: case 502: case 503: case 504:
				err_msg = "server error (HTTP " + std::to_string(http_status) + ")"
				        + (api_msg.empty() ? "" : ": " + api_msg);
				break;
			default:
				err_msg = "HTTP " + std::to_string(http_status)
				        + (api_msg.empty() ? "" : ": " + api_msg);
				break;
		}
		sink.OnError(err_msg);
		config::LogLine("error http=" + std::to_string(http_status)
				 + (api_msg.empty() ? "" : " msg=" + api_msg));
		return {1, {}, 0, 0, {}, {}};
	}

	if (state.stream_error) {
		const bool stream_retryable =
			(state.stream_error_type == "overloaded_error"
			 || state.stream_error_type == "api_error")
			&& !g_interrupted && attempt < kMaxRetries;
		if (stream_retryable) {
			const int delay = kBaseDelay << (attempt - 1);
			sink.OnDiag("[retry " + std::to_string(attempt)
			            + "/" + std::to_string(kMaxRetries)
			            + " in " + std::to_string(delay) + "ms: "
			            + state.stream_error_type + "]");
			config::LogLine("retry attempt=" + std::to_string(attempt)
					 + " stream_error=" + state.stream_error_type);
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue;
		}

		if (!state.text.empty()) std::cout << "\n";
		std::string stream_err_msg;
		if (state.stream_error_type == "overloaded_error") {
			stream_err_msg = "Anthropic's servers are overloaded — please try again in a moment.";
		} else {
			stream_err_msg = "stream error";
			if (!state.stream_error_type.empty())
				stream_err_msg += " (" + state.stream_error_type + ")";
			if (!state.stream_error_message.empty())
				stream_err_msg += ": " + state.stream_error_message;
		}
		sink.OnError(stream_err_msg);
		config::LogLine("stream_error type=" + state.stream_error_type
				 + (state.stream_error_message.empty()
					? "" : " msg=" + state.stream_error_message));
		return {1, state.text, state.input_tokens.load(), state.output_tokens.load(),
				state.content_blocks, state.stop_reason,
				state.cache_creation_input_tokens.load(),
				state.cache_read_input_tokens.load()};
	}

	if (state.content_blocks.empty()) {
		if (state.stop_reason == "max_tokens") {
			if (!state.text.empty()) std::cout << "\n";
			return {0, state.text, state.input_tokens.load(), state.output_tokens.load(),
					state.content_blocks, state.stop_reason,
					state.cache_creation_input_tokens.load(),
					state.cache_read_input_tokens.load()};
		}
		sink.OnError("no content received in stream\nresponse body: " + state.raw_buffer);
		return {1, {}, 0, 0, {}, {}};
	}

	std::cout << "\n";
	return {0, state.text, state.input_tokens.load(), state.output_tokens.load(),
			state.content_blocks, state.stop_reason,
			state.cache_creation_input_tokens.load(),
			state.cache_read_input_tokens.load()};

	} // end retry loop
}

SendResult SendWithTools(const config::Auth& auth, const std::string& model,
                         int max_tokens, json& messages,
                         const std::string& custom_system,
                         OutputSink* sink_in) {
	// Use the injected sink (e.g. TelegramSink from ProcessUpdate) or
	// create a fresh TerminalSink for the local REPL path.
	std::unique_ptr<TerminalSink> owned_sink;
	if (!sink_in) {
		owned_sink = std::make_unique<TerminalSink>();
		sink_in    = owned_sink.get();
	}
	OutputSink& sink = *sink_in;

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
	struct EscGuardScope {
		~EscGuardScope() { g_active_esc_guard = nullptr; }
	} esc_guard_scope;

	while (true) {
		if (g_interrupted) {
			sink.OnMeta("[interrupted]");
			aggregate.exit_code = 1;
			return aggregate;
		}

		SendResult result = SendConversation(auth, model, max_tokens, messages,
		                                     custom_system, /*include_tools=*/true,
		                                     &sink);
		aggregate.input_tokens                += result.input_tokens;
		aggregate.output_tokens               += result.output_tokens;
		aggregate.cache_creation_input_tokens += result.cache_creation_input_tokens;
		aggregate.cache_read_input_tokens     += result.cache_read_input_tokens;
		aggregate.assistant_text = result.assistant_text;
		aggregate.stop_reason    = result.stop_reason;

		if (result.exit_code != 0) {
			aggregate.exit_code = result.exit_code;
			return aggregate;
		}

		const bool truncated = (result.stop_reason == "max_tokens");
		if (truncated) {
			std::vector<json> safe_blocks;
			for (const auto& block : result.content_blocks) {
				if (block.value("type", "") != "tool_use")
					safe_blocks.push_back(block);
			}
			if (!safe_blocks.empty())
				messages.push_back({{"role", "assistant"}, {"content", safe_blocks}});
		} else {
			messages.push_back({{"role", "assistant"}, {"content", result.content_blocks}});
		}

		if (result.stop_reason != "tool_use") {
			if (result.stop_reason == "max_tokens") {
				const int used = result.output_tokens;
				sink.OnError("response truncated at the max_tokens cap"
				    " (output=" + std::to_string(used) + " / max=" + std::to_string(max_tokens) + ")."
				    "\n  Re-run with -t N (or set \"max_tokens\" in config.json) to raise the cap."
				    + (truncated ? "\n  The in-flight tool call was dropped from history; "
				                   "the file/command it would have produced was NOT executed." : ""));
				config::LogLine("truncated stop_reason=max_tokens output=" + std::to_string(used));
			} else if (result.stop_reason == "refusal") {
				sink.OnError("the model declined to answer (stop_reason=refusal).");
				config::LogLine("stop_reason=refusal");
			} else if (result.stop_reason == "pause_turn") {
				sink.OnError("the model paused its turn (stop_reason=pause_turn); re-send to continue.");
				config::LogLine("stop_reason=pause_turn");
			} else if (result.stop_reason != "end_turn"
			           && result.stop_reason != "stop_sequence"
			           && !result.stop_reason.empty()) {
				sink.OnError("unexpected stop_reason=" + result.stop_reason);
				config::LogLine("stop_reason=" + result.stop_reason);
			}
			return aggregate;
		}

		json tool_results = json::array();
		for (const auto& block : result.content_blocks) {
			if (block.value("type", "") != "tool_use") continue;
			const std::string tname  = block.value("name",  std::string{});
			const std::string tid    = block.value("id",    std::string{});
			const json        tinput = block.value("input", json::object());

			const std::string tool_notice = "[tool: " + tname + " " + ShortInputSummary(tinput) + "]";
			sink.OnMeta(tool_notice);
			config::LogLine("tool " + tname + " input=" + ShortInputSummary(tinput));

			tools::ToolResult tres;
			const json pre_payload = { {"tool_input", tinput} };
			if (hooks::Fire(hooks::Event::PreToolUse, pre_payload, tname) == hooks::Outcome::Block) {
				tres.content  = "hook blocked " + tname;
				tres.is_error = true;
				sink.OnMeta("[tool: " + tname + " -> blocked by hook]");
			} else if (std::string denial;
			           sink.AskPermission(tname, tinput, &denial) == Permission::Deny) {
				tres.content  = denial.empty()
				              ? "user denied permission to run " + tname
				              : denial;
				tres.is_error = true;
				sink.OnMeta("[tool: " + tname + " -> denied]");
			} else if (tname == "Task") {
				const std::string sub_prompt = tinput.value("prompt", std::string{});
				if (sub_prompt.empty()) {
					tres.content  = "error: Task requires a `prompt` argument";
					tres.is_error = true;
				} else {
					// Resolve an optional subagent definition. When the
					// model passes a subagent_type matching a loaded
					// agent, run the sub-turn with that agent's system
					// prompt and model override; otherwise fall back to
					// the generic single-shot sub-agent.
					const std::string sub_type =
						tinput.value("subagent_type", std::string{});
					const agents::Agent* agent =
						sub_type.empty() ? nullptr : agents::Find(sub_type);

					std::string sub_label =
						tinput.value("description", std::string{"sub-agent"});
					if (agent) sub_label = agent->name + ": " + sub_label;

					// The sub-agent's system prompt is the parent system
					// (so memory/behavior still apply) with the agent's
					// body prepended as its specialization.
					std::string sub_system = custom_system;
					if (agent && !agent->prompt.empty()) {
						sub_system = agent->prompt
						           + (custom_system.empty() ? "" : "\n\n" + custom_system);
					}
					const std::string sub_model =
						agent ? agents::ResolveModel(*agent, model) : model;

					sink.OnMeta("  -> " + sub_label + ":");
					std::cout << tui::ClaudePrompt();
					json sub_messages = json::array({{{"role", "user"}, {"content", sub_prompt}}});
					const auto sub = SendConversation(auth, sub_model, max_tokens,
					                                  sub_messages, sub_system,
					                                  /*include_tools=*/false,
					                                  &sink);
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
				sink.OnMeta(tres.is_error
					? "[tool: Task -> error]"
					: "[tool: Task -> " + std::to_string(tres.content.size()) + " bytes]");
				hooks::Fire(hooks::Event::PostToolUse,
				            json{{"tool_input", tinput}, {"tool_result", tres.content},
				                 {"is_error", tres.is_error}}, tname);
			} else {
				// Pass maxLen == 0 so the full command reaches the sink. The
				// CLI's TerminalSink ignores this phase string; the GUI tool
				// log shows the whole command (no "…" truncation).
				const std::string args = tools::ArgSummary(tname, tinput, 0);
				std::string phase = "\xF0\x9F\x94\xA7 running " + tname;
				if (!args.empty()) phase += ": " + args;
				phase += "\xE2\x80\xA6";
				sink.OnToolStatus(phase);
				tres = tools::Run(tname, tinput);
				sink.OnToolStatus("");   // done
				sink.OnMeta(tres.is_error
					? "[tool: " + tname + " -> error]"
					: "[tool: " + tname + " -> " + std::to_string(tres.content.size()) + " bytes]");
				hooks::Fire(hooks::Event::PostToolUse,
				            json{{"tool_input", tinput}, {"tool_result", tres.content},
				                 {"is_error", tres.is_error}}, tname);
			}

			if (tname == "Read" && !tres.is_error)
				config::AutoWriteSummaryIfMissing(tinput.value("path", std::string{}), tres.content);

			// When launched from Genio (Tools ▸ Claude), open each file
			// Claude writes or edits in the live Genio editor, jumping the
			// cursor to the edited line. Inert otherwise, so the CLI and a
			// directly-launched GUI are unaffected.
			if (!tres.is_error)
				editor::NotifyFileChanged(tname, tinput.value("path", std::string{}),
				                          tools::EditedLine(tname, tinput));

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

			tres.content = config::SanitizeUtf8(tres.content);

			tool_results.push_back({
				{"type",        "tool_result"},
				{"tool_use_id", tid},
				{"content",     tres.content},
				{"is_error",    tres.is_error},
			});
		}

		if (tool_results.empty()) return aggregate;
		messages.push_back({{"role", "user"}, {"content", tool_results}});
	}
}

std::vector<std::string> DrainWrittenSummaryPaths() {
	std::vector<std::string> out;
	out.swap(tl_written_summary_paths);
	return out;
}

} // namespace api
