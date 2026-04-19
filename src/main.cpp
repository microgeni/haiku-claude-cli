#include <algorithm>
#include <numeric>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "commands.h"
#include "hooks.h"
#include "mcp.h"
#include "notify.h"
#include "oauth.h"
#include "paths.h"
#include "repl.h"
#include "stats.h"
#include "telegram.h"
#include "tools.h"
#include "tui.h"

using json = nlohmann::json;

// Shared cancellation flag — declared extern in tools.cpp so the tool
// runner can react to Esc/Ctrl+C without a cross-TU function call.
// Must be at global (non-anonymous-namespace) scope for external linkage.
volatile sig_atomic_t g_interrupted = 0;

namespace {

constexpr const char* kVersion      = "1.5.1";
constexpr const char* kDefaultModel = "claude-sonnet-4-6";
constexpr const char* kApiUrl       = "https://api.anthropic.com/v1/messages";
constexpr const char* kApiVersion   = "2023-06-01";
constexpr const char* kOAuthBeta    = "oauth-2025-04-20";
constexpr const char* kOAuthSystem  = "You are Claude Code, Anthropic's official CLI for Claude.";
constexpr int         kMaxTokens    = 8192;

std::ofstream g_log;

void InitLogging(bool enabled) {
	if (!enabled) return;
	const std::string dir = paths::LogDir();
	if (!paths::MkdirP(dir)) return;

	const std::time_t t = std::time(nullptr);
	std::tm            tm {};
	localtime_r(&t, &tm);
	char date[32];
	std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);

	g_log.open(dir + "/claude-" + date + ".log", std::ios::app);
}

void LogLine(const std::string& msg) {
	if (!g_log.is_open()) return;
	const std::time_t t = std::time(nullptr);
	std::tm            tm {};
	localtime_r(&t, &tm);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
	g_log << "[" << ts << "] " << msg << "\n";
	g_log.flush();
}

std::string LoadOptionalFile(const std::string& path) {
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Compose the effective system prompt from (in order):
//   1. User-level memory at ~/config/settings/claude-cli/CLAUDE.md
//   2. Project memory at <cwd>/CLAUDE.md
//   3. The --system flag value (or config's "system")
// The required Claude Code preamble for OAuth is prepended inside
// SendConversation, so we don't repeat it here. Called per-turn so
// edits to the CLAUDE.md files take effect immediately.
// Snapshot of claude:summary attributes across the project
// cwd. Taken once per session (first ComposeSystem call) so
// later turns use the cached result instead of re-walking the
// filesystem. Claude sees the list as part of its system
// prompt and prefers ReadAttr over Read for files that appear
// here.
bool        g_bfs_loaded = false;
std::string g_bfs_snapshot;

// Strict UTF-8 validation — returns true only if every byte
// is part of a well-formed sequence. catattr output from a
// raw-typed or binary-valued claude:summary attribute can
// contain 0xFF / 0xFE bytes that nlohmann::json refuses to
// serialize, so lines that fail this check get dropped from
// the preload snapshot to keep the system prompt clean.
// Note: used exclusively by PreloadBfsSummaries() as a line-level
// pre-flight guard before appending to g_bfs_snapshot. SanitizeUtf8()
// (below) is the complementary recovery path used at API request time —
// it replaces bad bytes with U+FFFD rather than discarding the string.
bool IsValidUtf8(const char* data, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = static_cast<unsigned char>(data[i]);
		int need = 0;
		if (c < 0x80) { ++i; continue; }
		else if ((c & 0xE0) == 0xC0) need = 1;
		else if ((c & 0xF0) == 0xE0) need = 2;
		else if ((c & 0xF8) == 0xF0) need = 3;
		else                         return false;
		if (i + need >= len) return false;
		for (int k = 1; k <= need; ++k) {
			unsigned char cc = static_cast<unsigned char>(data[i + k]);
			if ((cc & 0xC0) != 0x80) return false;
		}
		i += need + 1;
	}
	return true;
}

// Replace every invalid UTF-8 byte (or truncated sequence) with the
// Unicode replacement character U+FFFD (0xEF 0xBF 0xBD) so that
// nlohmann::json never sees a byte that would trigger type_error.316.
// Tool output from Bash commands that cat binary files (driver blobs,
// /dev entries, etc.) can easily contain raw 0x80-0xFF bytes.
std::string SanitizeUtf8(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	const unsigned char* p   = reinterpret_cast<const unsigned char*>(s.data());
	const unsigned char* end = p + s.size();
	while (p < end) {
		unsigned char c = *p;
		int need = 0;
		if      (c < 0x80)             { out += static_cast<char>(c); ++p; continue; }
		else if ((c & 0xE0) == 0xC0)   need = 1;
		else if ((c & 0xF0) == 0xE0)   need = 2;
		else if ((c & 0xF8) == 0xF0)   need = 3;
		else { out += "\xEF\xBF\xBD"; ++p; continue; }   // bad lead byte

		if (p + need >= end) {                             // truncated sequence
			out += "\xEF\xBF\xBD"; p = end; continue;
		}
		bool ok = true;
		for (int k = 1; k <= need; ++k) {
			if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
		}
		if (!ok) { out += "\xEF\xBF\xBD"; ++p; continue; }

		// valid sequence — copy it verbatim
		for (int k = 0; k <= need; ++k) out += static_cast<char>(p[k]);
		p += need + 1;
	}
	return out;
}

#ifdef __HAIKU__
void PreloadBfsSummaries() {
	if (g_bfs_loaded) return;
	g_bfs_loaded = true;

	// Walk the project cwd, collect claude:summary values via
	// catattr. `find | while read` is simple and handles
	// non-ASCII paths adequately for this use. Excludes the
	// usual noise directories so we don't spend time on
	// node_modules/.git/build.
	const char* cmd =
		"find . -type f "
		"  -not -path './.git/*' "
		"  -not -path './build/*' "
		"  -not -path './node_modules/*' "
		"  -not -path '*/\\.*' 2>/dev/null | while read f; do "
		"    s=$(catattr -d claude:summary \"$f\" 2>/dev/null) || continue; "
		"    [ -n \"$s\" ] && printf '%s :: %s\\n' \"$f\" \"$s\"; "
		"done";
	FILE* p = popen(cmd, "r");  // flawfinder: ignore
	if (!p) return;
	char buf[4096];
	while (std::fgets(buf, sizeof(buf), p)) {
		const size_t n = std::strlen(buf);
		// Drop the whole line if it contains non-UTF-8 bytes
		// — catattr's raw-typed attribute values will crash
		// nlohmann::json on serialization otherwise.
		if (!IsValidUtf8(buf, n)) continue;
		g_bfs_snapshot.append(buf, n);
	}
	pclose(p);
}
#else
void PreloadBfsSummaries() { g_bfs_loaded = true; }
#endif

std::string BfsSystemBlock() {
#ifdef __HAIKU__
	if (!g_bfs_loaded) PreloadBfsSummaries();

	std::string s =
		"Haiku BFS attribute tools — prefer these on this project:\n"
		"- ReadAttr: read `claude:summary` (or any named attribute) from a "
		  "file. Check this BEFORE calling Read on a source file — summaries "
		  "cost ~10-30 tokens vs. thousands for full reads.\n"
		"- WriteAttr: write a one-line `claude:summary` after reading a file "
		  "for the first time. Later sessions will use ReadAttr and skip the "
		  "full read. Only use the `claude:*` namespace; never overwrite "
		  "BEOS:*/MAIL:*/Audio:* or other system attributes.\n"
		"- Query: BFS query expression for filesystem searches. Fast when an "
		  "index exists; use for file-metadata lookups.\n";

	if (!g_bfs_snapshot.empty()) {
		s += "\nFiles in this project with existing claude:summary "
			 "(prefer ReadAttr over Read for these):\n";
		s += g_bfs_snapshot;
	} else {
		s += "\n(No claude:summary attributes seeded yet — writing summaries "
			 "for source files you read this session will let later sessions "
			 "save tokens via ReadAttr.)\n";
	}
	return s;
#else
	return {};
#endif
}

std::string ComposeSystem(const std::string& flag_system) {
	std::string out;
	auto append = [&](const std::string& chunk) {
		if (chunk.empty()) return;
		if (!out.empty()) out += "\n\n";
		out += chunk;
	};
	append(LoadOptionalFile(paths::UserMemoryPath()));
	append(LoadOptionalFile(paths::ProjectMemoryPath()));
	append(BfsSystemBlock());
	append(flag_system);
	return out;
}

// ── History persistence ───────────────────────────────────────

// Maximum tool-result content stored in history (bytes). The full
// output was already streamed to the terminal; storing only the head
// keeps history.json small while preserving enough for Claude to
// follow the conversation on --resume.
static constexpr size_t kHistoryToolResultCap = 4096;

// Maximum number of messages kept when loading a saved session.
// A hard cap prevents unbounded growth from sessions that never
// trigger auto-compact (many short turns, low token counts).
// 200 messages ≈ 100 turns — enough context for any real session.
static constexpr size_t kHistoryMessageCap = 200;

// Trim tool_result content blocks inside a messages array so each
// individual result is at most kHistoryToolResultCap bytes. Returns
// a new array; the in-memory messages vector is not mutated so the
// live session context stays intact (only the saved file is capped).
static json TrimToolResults(const json& messages) {
	json out = json::array();
	for (const auto& msg : messages) {
		// tool_result blocks live in user turns whose "content" is
		// an array of typed blocks.
		if (msg.value("role", "") == "user" && msg["content"].is_array()) {
			json trimmed_content = json::array();
			for (const auto& block : msg["content"]) {
				if (block.value("type", "") == "tool_result") {
					std::string content = block.value("content", "");
					if (content.size() > kHistoryToolResultCap) {
						content.resize(kHistoryToolResultCap);
						content += "\n[... truncated for history storage ...]";
					}
					json b = block;
					b["content"] = content;
					trimmed_content.push_back(std::move(b));
				} else {
					trimmed_content.push_back(block);
				}
			}
			json m = msg;
			m["content"] = trimmed_content;
			out.push_back(std::move(m));
		} else {
			out.push_back(msg);
		}
	}
	return out;
}

std::optional<json> LoadHistory(const std::string& name = "") {
	const std::string path = name.empty()
		? paths::HistoryPath()
		: paths::NamedHistoryPath(name);
	std::ifstream f(path);
	if (!f.is_open()) return std::nullopt;
	try {
		json j = json::parse(f);
		if (!j.contains("messages") || !j["messages"].is_array())
			return std::nullopt;
		json msgs = j["messages"];
		// Apply turn cap: keep only the last kHistoryMessageCap messages.
		if (msgs.size() > kHistoryMessageCap) {
			json capped = json::array();
			const size_t start = msgs.size() - kHistoryMessageCap;
			for (size_t i = start; i < msgs.size(); ++i)
				capped.push_back(msgs[i]);
			msgs = std::move(capped);
		}
		return msgs;
	} catch (...) {
		// fall through
	}
	return std::nullopt;
}

bool SaveHistory(const json& messages, const std::string& model,
				  const std::string& name = "") {
	const std::string path = name.empty()
		? paths::HistoryPath()
		: paths::NamedHistoryPath(name);
	const auto slash = path.rfind('/');
	if (slash == std::string::npos) return false;
	if (!paths::MkdirP(path.substr(0, slash))) return false;

	const json j = {
		{"messages", TrimToolResults(messages)},
		{"model",    model},
		{"saved_at", static_cast<long>(std::time(nullptr))},
	};

	// Atomic write: serialize to a tmp file alongside the real path,
	// then rename(2) into place. A crash or power loss mid-write
	// leaves the previous good file intact.
	//
	// Open with O_CREAT|0600 so the file is never world-readable even
	// transiently — avoids the chmod-after-open TOCTOU race (CWE-362).
	const std::string tmp_path = path + ".tmp";
	{
		const int fd = ::open(tmp_path.c_str(),
		                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) return false;
		FILE* fp = ::fdopen(fd, "w");
		if (!fp) { ::close(fd); return false; }
		const std::string serialized = j.dump(2) + "\n";
		const bool ok = std::fwrite(serialized.data(), 1,
		                            serialized.size(), fp) == serialized.size();
		std::fclose(fp); // also closes fd
		if (!ok) {
			std::remove(tmp_path.c_str());
			return false;
		}
	}
	if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
		std::remove(tmp_path.c_str());
		return false;
	}
	return true;
}

// Cross-thread progress handle used by the Telegram bridge to watch a
// streaming response and push incremental edits to the chat. Written
// by ProcessSseEvent's text_delta branch; read by the bridge's
// updater thread. Nulled out when no remote consumer is attached.
struct StreamProgress {
	std::mutex        mu;
	std::string       text;
	std::atomic<int>  version {0};
	// Non-empty while a tool is actively running. The updater thread
	// shows this string instead of the "thinking…" animation so the
	// Telegram user sees e.g. "🔧 running Bash…" during tool execution.
	std::string       tool_phase;
};
StreamProgress* g_stream_progress = nullptr;

// Optional hook installed by Telegram bridge contexts to forward tool
// lifecycle notices (start, result, denial) as separate Telegram
// messages so the remote user has the same visibility as the local
// operator. Signature: (notice_text). Cleared to nullptr after each turn.
std::function<void(const std::string&)> g_tool_status_hook;

std::map<std::string, std::string> g_last_rate_headers;

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

extern "C" void handle_sigint(int) {
	g_interrupted = 1;
}

struct InterruptGuard {
	InterruptGuard() {
		g_interrupted = 0;
		struct sigaction sa {};
		sa.sa_handler = handle_sigint;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, &fPrev);
	}
	~InterruptGuard() {
		sigaction(SIGINT, &fPrev, nullptr);
	}
	InterruptGuard(const InterruptGuard&) = delete;
	InterruptGuard& operator=(const InterruptGuard&) = delete;
  private:
	struct sigaction fPrev {};
};

int xfer_callback(void* /*clientp*/,
				  curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
				  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
	return g_interrupted ? 1 : 0;
}

// RAII helper that temporarily puts stdin into cbreak mode while an
// HTTP stream is in flight and spawns a background thread polling
// stdin for a bare ESC keypress. On ESC it sets g_interrupted so the
// existing xfer_callback aborts the curl transfer — the same path
// that Ctrl+C uses. Destructor tears down the thread and restores
// the saved termios so libedit gets stdin back in cooked mode for
// the next prompt.
//
// No-op when stdin isn't a TTY (piped input, CI, subprocess) so
// scripted invocations keep behaving normally.
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
				// When paused, spin on a short sleep without touching stdin
				// so that tui::SelectOption() has exclusive access to it.
				// Set fPausedAck while inside this sleep so pause() can
				// confirm the thread has stopped reading before returning.
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
				// CSI sequences starting with 0x1B (usually
				// 0x1B '[' <rest>). We only treat the lone-byte case
				// as cancel so a twitchy arrow-key press during
				// streaming doesn't kill the turn. If 0x1B appears
				// mid-sequence it's almost certainly a CSI prefix.
				if (n == 1 && buf[0] == '\x1b') {
					g_interrupted = 1;
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
	// tui::SelectOption) has exclusive access to it.
	// Blocks until the background thread has acknowledged the pause —
	// i.e. is confirmed to be in its sleep loop and not mid-read.
	// Without this synchronisation the thread can race to consume
	// arrow-key CSI bytes (ESC [ A/B) just after pause() sets the flag
	// but before SelectOption() calls read(), causing ↑/↓ to do nothing.
	void pause() {
		fPaused.store(true);
		// Spin until the thread has set fPausedAck, confirming it has
		// exited any in-progress poll/read and is in the idle sleep loop.
		// Upper bound: the poll() timeout is 100 ms, so we wait at most
		// ~110 ms in the worst case (poll just started when we set the flag).
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
	std::atomic<bool> fRunning    { false };
	std::atomic<bool> fPaused     { false };
	std::atomic<bool> fPausedAck { false }; // set by thread when it enters pause sleep
	std::thread       fThread;
	termios           fSaved {};
	bool              fSavedValid = false;
};

// Pointer to the currently active EscInterruptGuard (if any).
// Set in SendWithTools so that PromptPermission can pause/resume
// it around tui::SelectOption() calls.
static EscInterruptGuard* g_active_esc_guard = nullptr;

struct Config {
	std::string model;
	int         max_tokens = kMaxTokens;
	std::string system;  // flawfinder: ignore
	bool        show_usage              = false;
	bool        logging_enabled         = false;
	bool        fAllowDestructiveTools = false;
	bool        notify_enabled          = true;
	double      notify_min_duration_sec = 60.0;
	// Auto-compact fires `/compact` when a turn's input token
	// count crosses `compact_auto_threshold` * context_window.
	// 0.0 disables. context_window=0 auto-detects from the
	// model name (1M for "[1m]" variants, 200k otherwise).
	double      compact_auto_threshold  = 0.8;
	int         compact_context_window  = 0;
	json        prices;
	json        hooks;
	json        mcp_servers;
	json        telegram;
};

Config load_config() {
	Config cfg;
	cfg.model = kDefaultModel;

	std::ifstream f(paths::ConfigPath());
	if (!f.is_open()) return cfg;

	try {
		const json j = json::parse(f);
		if (j.contains("model"))      cfg.model      = j["model"].get<std::string>();
		if (j.contains("max_tokens")) cfg.max_tokens = j["max_tokens"].get<int>();
		if (j.contains("system"))     cfg.system     = j["system"].get<std::string>();
		if (j.contains("show_usage")) cfg.show_usage = j["show_usage"].get<bool>();
		if (j.contains("prices"))       cfg.prices      = j["prices"];
		if (j.contains("hooks"))        cfg.hooks       = j["hooks"];
		if (j.contains("mcp_servers"))  cfg.mcp_servers = j["mcp_servers"];
		if (j.contains("telegram"))     cfg.telegram    = j["telegram"];
		// Accept both the current key and the legacy lowercase-t spelling
		// so existing config.json files continue to work after the rename.
		if (j.contains("fAllowDestructiveTools"))
			cfg.fAllowDestructiveTools = j["fAllowDestructiveTools"].get<bool>();
		else if (j.contains("fAllowDestructivetools"))
			cfg.fAllowDestructiveTools = j["fAllowDestructivetools"].get<bool>();
		if (j.contains("logging") && j["logging"].is_object()) {
			cfg.logging_enabled = j["logging"].value("enabled", false);
		}
		if (j.contains("notify") && j["notify"].is_object()) {
			cfg.notify_enabled          = j["notify"].value("enabled", true);
			cfg.notify_min_duration_sec = j["notify"].value("min_duration_seconds", 60.0);
		}
		if (j.contains("compact") && j["compact"].is_object()) {
			cfg.compact_auto_threshold = j["compact"].value("auto_threshold", 0.8);
			cfg.compact_context_window = j["compact"].value("context_window", 0);
		}
	} catch (const json::exception& e) {
		std::cerr << "warning: failed to parse " << paths::ConfigPath() << ": " << e.what() << "\n";
	}
	return cfg;
}

enum class AuthKind { None, OAuth, ApiKey };

struct Auth {
	AuthKind    kind = AuthKind::None;
	std::string credential;
};

struct StreamState {
	std::string          sse_buffer;
	std::string          raw_buffer;
	std::string          text;
	// Atomics so the spinner thread can read them concurrently
	// with the main thread's SSE parser. The spinner reads
	// input_tokens via a non-owning pointer passed to
	// SetLiveInputTokens() so the "(44s · ↑ 652 tokens)" tail
	// picks up the prompt size the moment `message_start` arrives.
	std::atomic<int>     input_tokens        { 0 };
	std::atomic<int>     output_tokens       { 0 };
	// Prompt-cache telemetry from the Messages API usage block.
	// cache_creation = tokens written to cache (1.25× cost, first
	// hit); cache_read = tokens served from cache (~0.1× cost,
	// subsequent hits). When caching works, cache_read dominates.
	std::atomic<int>     cache_creation_input_tokens { 0 };
	std::atomic<int>     cache_read_input_tokens     { 0 };
	bool                 saw_text            = false;
	bool                 stream_error        = false;
	std::string          stream_error_message;
	tui::Spinner*        spinner             = nullptr;
	tui::MarkdownRenderer renderer;

	// Structured content accumulation for tool-use support.
	std::vector<json>    content_blocks;           // finalized text + tool_use blocks
	std::string          current_type;             // "text" / "tool_use" while streaming a block
	std::string          current_text;
	std::string          current_tool_id;
	std::string          current_tool_name;
	std::string          current_tool_input_raw;   // partial JSON being accumulated
	std::string          stop_reason;              // set via message_delta
};

struct SendResult {
	int                exit_code = 0;
	std::string        assistant_text;
	int                input_tokens  = 0;
	int                output_tokens = 0;
	std::vector<json>  content_blocks;
	std::string        stop_reason;
	// Anthropic prompt-cache usage for this call. cache_read > 0
	// on repeat turns means the system+tools prefix is reused from
	// Anthropic's server-side cache — faster TTFT, 10% of normal
	// input-token cost on that portion. Kept at the end so existing
	// positional {...} constructions continue to compile with these
	// as zero-defaulted trailing fields.
	int                cache_creation_input_tokens = 0;
	int                cache_read_input_tokens     = 0;
};

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
			// Intentionally leave the spinner running here — we
			// want the "(elapsed · ↑ N tokens)" tail to render
			// during the window between prompt ingestion and the
			// first text_delta. MarkdownRenderer::Write() still
			// stops the spinner on first real output.
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
			if (j.contains("error") && j["error"].contains("message")) {
				state->stream_error_message = j["error"]["message"].get<std::string>();
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

void PrintUsage(const char* prog, const std::string& default_model, int default_max_tokens) {
	std::cerr << "Usage: " << prog << " [OPTIONS] [MESSAGE...]\n"
			  << "\n"
			  << "Sends a one-shot message to the Claude API and streams the reply.\n"
			  << "If stdin is not a terminal (piped input), its contents are appended\n"
			  << "to the message so `cat file.txt | " << prog << " \"summarize\"` works.\n"
			  << "\n"
			  << "Commands:\n"
			  << "  login                Authenticate via Claude.ai (OAuth + PKCE).\n"
			  << "  logout               Delete stored credentials.\n"
			  << "\n"
			  << "Options:\n"
			  << "  -i, --interactive    Start a multi-turn REPL session.\n"
			  << "  -m, --model MODEL    Model to use (default: " << default_model << ").\n"
			  << "  -t, --max-tokens N   Max tokens in response (default: " << default_max_tokens << ").\n"
			  << "  -s, --system TEXT    Custom system prompt (appended after the\n"
			  << "                       required Claude Code prefix when OAuth is used).\n"
			  << "  -u, --usage          After the response, print input/output token\n"
			  << "                       usage to stderr.\n"
			  << "  -r, --resume [NAME]  Start the REPL pre-loaded with the last saved\n"
			  << "                       session (implies -i). Without NAME loads the\n"
			  << "                       default session (history.json). With NAME loads\n"
			  << "                       (or creates) a named session stored as\n"
			  << "                       history-<NAME>.json alongside it.\n"
			  << "  -y, --yes            Auto-approve destructive tools (Bash/Write/Edit)\n"
			  << "                       for this run. Needed for one-shot invocations\n"
			  << "                       without a TTY to answer the y/a/n prompt.\n"
			  << "  -a, --attach PATH    Attach a file path to this session. Repeatable.\n"
			  << "                       Announced to Claude on the next user turn so\n"
			  << "                       tools like Read can pull them in. In interactive\n"
			  << "                       mode you can also drag files from Tracker onto\n"
			  << "                       the Terminal window — the REPL auto-detects paths.\n"
			  << "      --plain          Disable ANSI color output.\n"
			  << "      --color          Force ANSI color output, even when piped.\n"
			  << "  -V, --version        Print version and exit.\n"
			  << "  -h, --help           Show this help and exit.\n"
			  << "\n"
			  << "Config file: " << paths::ConfigPath() << "\n"
			  << "  Optional JSON with keys: model, max_tokens, system, show_usage,\n"
			  << "  fAllowDestructiveTools, prices. CLI flags override config values.\n"
			  << "\n"
			  << "Memory files (prepended to the system prompt, user before project):\n"
			  << "  " << paths::UserMemoryPath() << "\n"
			  << "  ./CLAUDE.md (per-project, loaded from the current working directory)\n"
			  << "\n"
			  << "Authentication (in priority order):\n"
			  << "  1. OAuth tokens from 'claude login' (uses Pro/Max quota).\n"
			  << "  2. ANTHROPIC_API_KEY environment variable (billed per token).\n";
}

Auth ResolveAuth() {
	if (auto stored = LoadTokens(); stored) {
		if (stored->IsExpired()) {
			if (auto refreshed = RefreshTokens(*stored); refreshed) {
				SaveTokens(*refreshed);
				return {AuthKind::OAuth, refreshed->access_token};
			}
			std::cerr << "warning: OAuth refresh failed, falling back to API key\n";
		} else {
			return {AuthKind::OAuth, stored->access_token};
		}
	}

	if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {  // flawfinder: ignore
		return {AuthKind::ApiKey, k};
	}
	return {};
}

// Session-scoped curl handle. Reused across all SendConversation
// calls so DNS cache, TLS session, and TCP connections persist
// between turns instead of being rebuilt on every request.
// Created lazily on first use; cleaned up via atexit.
namespace {
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
} // namespace

// Fetch the list of available models from GET /v1/models.
// Returns a vector of (id, display_name) pairs sorted by `created_at`
// descending (newest first), matching the default API ordering.
// On any error the returned vector is empty.
//
// Uses a private short-lived handle rather than the session handle
// returned by get_curl(), because model listing is a one-shot request
// (called from the /model picker) and must not disturb the persistent
// streaming state of the main session handle.
struct ModelEntry { std::string id; std::string display_name; };

std::vector<ModelEntry> fetch_models(const Auth& auth) {
	CURL* curl = curl_easy_init();
	if (!curl) return {};

	// Accumulate the response body.
	std::string body;
	auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
		auto* s = static_cast<std::string*>(userdata);
		s->append(ptr, size * nmemb);
		return size * nmemb;
	};

	curl_slist* headers = nullptr;
	if (auth.kind == AuthKind::OAuth) {
		headers = curl_slist_append(headers, ("authorization: Bearer " + auth.credential).c_str());
		headers = curl_slist_append(headers, (std::string("anthropic-beta: ") + kOAuthBeta).c_str());
	} else {
		headers = curl_slist_append(headers, ("x-api-key: " + auth.credential).c_str());
	}
	headers = curl_slist_append(headers, (std::string("anthropic-version: ") + kApiVersion).c_str());

	curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/models?limit=100");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(write_cb));
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

	const CURLcode res = curl_easy_perform(curl);
	long http_status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || http_status != 200) return {};

	std::vector<ModelEntry> out;
	try {
		const auto j = json::parse(body);
		if (!j.contains("data") || !j["data"].is_array()) return {};
		for (const auto& m : j["data"]) {
			ModelEntry e;
			e.id           = m.value("id", "");
			e.display_name = m.value("display_name", e.id);
			if (!e.id.empty()) out.push_back(std::move(e));
		}
	} catch (...) {}
	return out;
}

SendResult SendConversation(const Auth& auth, const std::string& model, int max_tokens,
							 const json& messages, const std::string& custom_system,
							 bool include_tools) {
	constexpr int kMaxRetries = 3;
	constexpr int kBaseDelay  = 1000; // ms; doubles on each retry

	CURL* curl = get_curl();
	if (!curl) {
		std::cerr << "error: curl_easy_init failed\n";
		return {1, {}, 0, 0, {}, {}};
	}

	// Sanitize the system prompt once before entering the retry loop.
	// CLAUDE.md files and the BFS snapshot are read as raw bytes and may
	// contain non-UTF-8 sequences (e.g. a file with a Latin-1 em-dash, a
	// BFS attribute written by an older tool, etc.). nlohmann::json::dump()
	// throws type_error.316 on any invalid byte, terminating the process if
	// uncaught. SanitizeUtf8 replaces bad bytes with U+FFFD so the string
	// is always safe to serialize.
	const std::string safe_system = SanitizeUtf8(custom_system);

	for (int attempt = 1; /* break/return inside */; ++attempt) {
	// Reset per-request state on the reused handle so stale
	// headers / callbacks from the previous call don't leak.
	curl_easy_reset(curl);

	// Mutable copy of messages so we can stamp cache_control on
	// the most recent user turn without disturbing the caller's
	// conversation history.
	json cached_messages = messages;
	if (!cached_messages.empty()) {
		auto& last = cached_messages.back();
		if (last.contains("content")) {
			auto& content = last["content"];
			if (content.is_string()) {
				// Upgrade string content into an array with a single
				// text block so we have somewhere to attach
				// cache_control.
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
	// system field's first entry is the Claude Code preamble. Earlier
	// versions concatenated extra content into a single string which
	// worked for small flag-only system prompts, but started failing
	// once CLAUDE.md memory files were appended. Sending `system` as
	// an array with the preamble as element 0 and any extra content
	// as element 1 passes the check.
	//
	// Prompt caching: mark the LAST system block with
	// cache_control: ephemeral. Render order is tools → system →
	// messages, so one marker here caches both the tools array and
	// the system prompt together. Subsequent turns in the same
	// session (stable tools + stable system) hit the cache and
	// process input ~5-10× faster at ~10% of the normal input
	// token cost.
	if (auth.kind == AuthKind::OAuth) {
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
	if (auth.kind == AuthKind::OAuth) {
		headers = curl_slist_append(headers, ("authorization: Bearer " + auth.credential).c_str());
		headers = curl_slist_append(headers, (std::string("anthropic-beta: ") + kOAuthBeta).c_str());
	} else {
		headers = curl_slist_append(headers, ("x-api-key: " + auth.credential).c_str());
	}
	headers = curl_slist_append(headers, (std::string("anthropic-version: ") + kApiVersion).c_str());
	headers = curl_slist_append(headers, "content-type: application/json");
	headers = curl_slist_append(headers, "accept: text/event-stream");

	StreamState state;
	// The Spinner now picks a gerund verb ("Forming", "Pondering",
	// ...) internally and renders a Claude Code-style
	// "(elapsed · ↑ N tokens · esc:cancel)" tail. We hand it a
	// pointer to the live input_tokens atomic so the prompt size
	// appears as soon as `message_start` arrives over SSE, a few
	// hundred ms after curl_easy_perform begins.
	tui::Spinner spinner("thinking");
	spinner.SetLiveInputTokens(&state.input_tokens);
	state.spinner = &spinner;
	state.renderer.SetSpinner(&spinner);

	curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
	const std::string ua = std::string("haiku-claude-cli/") + kVersion;
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
	// creating a second concurrent reader on the same stdin fd causes a
	// race where both threads see POLLIN, one reads the ESC byte, and
	// the other silently discards unrelated bytes. Skip the inner guard
	// entirely when an outer one is live. When called standalone (e.g.
	// direct one-shot), no outer guard exists so we create one here.
	CURLcode res;
	{
		// Conditionally-constructed guard: only active when there is no
		// outer EscInterruptGuard (standalone / one-shot path).
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
			LogLine("retry attempt=" + std::to_string(attempt)
					 + " curl=" + std::to_string(res));
			// Interruptible sleep: poll in 100 ms ticks so Esc/Ctrl+C
			// is noticed promptly rather than waiting out the full delay.
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue; // retry
		}
		std::cerr << "\nerror: request failed: " << curl_easy_strerror(res) << "\n";
		return {1, {}, 0, 0, {}, {}};
	}

	if (http_status < 200 || http_status >= 300) {
		// Parse Anthropic's error envelope for a user-friendly message,
		// then map the HTTP code to a plain-language explanation.
		std::string api_msg;
		try {
			const json err = json::parse(state.raw_buffer);
			if (err.contains("error") && err["error"].is_object()
				&& err["error"].contains("message")
				&& err["error"]["message"].is_string()) {
				api_msg = err["error"]["message"].get<std::string>();
			}
		} catch (const json::exception&) {}

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
			LogLine("retry attempt=" + std::to_string(attempt)
					 + " http=" + std::to_string(http_status));
			for (int slept = 0; slept < delay && !g_interrupted; slept += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(100, delay - slept)));
			continue; // retry
		}

		std::cerr << "\n" << tui::ErrorLabel() << " ";
		switch (http_status) {
			case 401:
				std::cerr << "unauthorized (HTTP 401) — your OAuth token may be "
							 "expired. Run `claude logout` and then `claude login`.";
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
		LogLine("error http=" + std::to_string(http_status)
				 + (api_msg.empty() ? "" : " msg=" + api_msg));
		return {1, {}, 0, 0, {}, {}};
	}

	if (state.stream_error) {
		std::cerr << "\nerror: stream error: " << state.stream_error_message << "\n";
		return {1, state.text, state.input_tokens.load(), state.output_tokens.load(),
				state.content_blocks, state.stop_reason,
				state.cache_creation_input_tokens.load(),
				state.cache_read_input_tokens.load()};
	}

	if (state.content_blocks.empty()) {
		// Empty is fine if the turn ended cleanly — but attribute
		// the cause. The most common reason in practice is a
		// max_tokens cap so tight that the first tool_use block
		// never got a content_block_stop event, leaving it stuck
		// in the accumulator. Surface stop_reason so the caller
		// (and the user) can react, rather than emitting a generic
		// "no content" error that hides the real cause.
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

std::string ShortInputSummary(const json& input) {
	const std::string dumped = input.dump();
	if (dumped.size() <= 80) return dumped;
	return dumped.substr(0, 77) + "...";
}

enum class Permission { Allow, Deny };

// Session-scoped allowlist of tool Names the user has explicitly
// approved with "(a)lways".
std::unordered_set<std::string>& always_allowed() {
	static std::unordered_set<std::string> s;
	return s;
}

// Non-interactive mode is set by the Telegram bridge: there's no
// stdin to prompt on, so destructive tools are either blanket-allowed
// or blanket-denied based on config.
bool g_non_interactive_tools             = false;
bool g_non_interactive_allow_destructive = false;

// Telegram bridge mute toggle. When true, every outbound call to the
// bot API (sendMessage / editMessageText / sendChatAction / the
// message_id-returning variant) is suppressed by the tg_* wrapper
// lambdas in the bridge's tg_* wrappers. Incoming messages still get
// processed locally — tools run, the operator's terminal still shows
// everything — but nothing leaves the machine until /unmute. The
// /mute and /unmute acks themselves bypass the wrapper (they go out
// via client.SendMessage directly) so the state transition is
// always visible on the Telegram side.
std::atomic<bool> g_telegram_muted { false };

// Pauses the Telegram thinking-updater threads while a permission
// prompt is in flight so the bot doesn't spam EditMessageText and
// hit Telegram rate limits that would delay (or drop) the inline-
// keyboard "allow Bash?" message the user needs to tap.
std::atomic<bool> g_telegram_updater_paused { false };

// Set at startup from Config::fAllowDestructiveTools or the -y/--yes
// flag. Grants destructive-tool permission without prompting whenever
// stdin isn't usable for a y/a/n dialog (piped stdin, closed stdin,
// etc.). Interactive TTY sessions still prompt normally.
bool g_allow_destructive_tools = false;

// Ludicrous mode: session-scoped toggle that auto-approves all
// destructive tool calls and skips permission prompts entirely.
// Toggled on/off via the /ludicrous slash command.
std::atomic<bool> g_ludicrous_mode { false };

// Telegram permission hook. When the bridge is handling a turn it
// installs a callback here so that PromptPermission can send an
// inline-keyboard message and block until the user taps a button,
// instead of falling through to the blanket allow/deny logic.
// Signature: (tool_name, preview_text, local_answered) → Permission.
//   local_answered: non-null when PromptPermission is also showing a
//   local SelectOption menu in a racing thread. The hook must check
//   *local_answered and return early (Deny) when it becomes true so
//   the local answer wins without a dangling wait. Pure-Telegram
//   hooks (bridge, non-interactive) may ignore it ((void)local_answered).
// Cleared to nullptr after each turn.
std::function<Permission(const std::string&, const std::string&, std::atomic<bool>*)> g_telegram_permission_hook;

Permission PromptPermission(const std::string& tool_name, const json& input,
							 std::string* denial_reason = nullptr) {
	if (always_allowed().count(tool_name)) return Permission::Allow;
	if (!tools::RequiresPermission(tool_name)) return Permission::Allow;

	// Ludicrous mode: all permissions auto-approved, no prompts.
	if (g_ludicrous_mode.load()) {
		std::cout << tui::Dim("  \xE2\x9A\xA1 ludicrous: auto-approved " + tool_name) << "\n";
		return Permission::Allow;
	}

	// Telegram remote-control hook: fires for ALL turns (local or
	// Telegram-origin) when /remote-control is active.  Checked
	// before the g_non_interactive_tools gate so a local turn that
	// triggers Bash still asks the Telegram user for approval.
	//
	// When a real TTY is attached we race the Telegram inline-keyboard
	// against the local SelectOption menu: whichever answers first wins.
	// This lets the operator at the keyboard approve/deny instantly
	// without waiting for a Telegram response, and vice-versa.
	if (g_telegram_permission_hook) {
		const std::string extra = tools::Preview(tool_name, input);
		const std::string preview = extra.empty()
			? (tool_name + " " + ShortInputSummary(input))
			: extra;
		if (!extra.empty()) std::cout << tui::Dim(extra) << "\n";
		else std::cout << tui::Meta("  -> " + tool_name + " " + ShortInputSummary(input)) << "\n";

		const bool has_tty = isatty(fileno(stdin)) && isatty(fileno(stdout));
		// For Telegram-origin turns (g_non_interactive_tools == true) we
		// must NOT call tui::SelectOption — the main-thread REPL is also
		// reading stdin, so calling SelectOption from the worker thread
		// races on stdin and causes a crash.  Route straight through the
		// Telegram hook regardless of whether a TTY is attached.
		if (!has_tty || g_non_interactive_tools) {
			// Non-interactive or Telegram-origin: only Telegram can answer.
			std::cout << tui::Bold("allow " + tool_name + "? ")
					  << tui::Dim("[awaiting Telegram response]") << "\n" << std::flush;
			const Permission p = g_telegram_permission_hook(tool_name, preview, nullptr);
			const char* lbl = (p == Permission::Allow) ? "yes" : "no";
			std::cout << tui::Dim(std::string("  -> ") + lbl) << "\n";
			return p;
		}

		// Race: show the local menu in a thread while the Telegram hook
		// waits for a button tap.  A shared atomic lets the winner
		// signal the loser to stop blocking.
		// NOTE: this branch is only reached for local REPL turns that
		// also have /remote-control active (g_non_interactive_tools==false
		// but g_telegram_permission_hook is set).  In that case the main
		// thread owns stdin so it is safe to call SelectOption here.
		std::atomic<bool> local_answered { false }; // set when local user picks
		std::atomic<bool> tg_answered    { false }; // set when Telegram answers
		Permission         result         { Permission::Deny };
		std::mutex         race_mu;
		std::condition_variable race_cv;

		// Telegram side runs in a thread so SelectOption can own this
		// thread's stdin in raw mode without a re-entrant conflict.
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

		// Local side: show the menu.
		tui::PositionCursorForChat();
		const std::vector<std::string> choices = {
			"Yes, allow once",
			"Always allow this session",
			"No, deny",
		};
		std::cout << tui::Dim("[also awaiting Telegram — or answer locally]") << "\n" << std::flush;
		if (g_active_esc_guard) g_active_esc_guard->pause();
		// Count preview lines printed above the menu so SelectOption can
		// erase them together with the menu block on selection.
		// extra_pre: the tool preview/summary line(s) + the Telegram hint line.
		const int race_pre_lines =
			(extra.empty()
				? 1
				: static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1)
			+ 1; // the "[also awaiting Telegram]" line
		// Pass &tg_answered so SelectOption polls it every ~100 ms and
		// returns -1 immediately when the Telegram side wins the race.
		const int picked = tui::SelectOption(choices, "allow " + tool_name + "?",
											 &tg_answered, race_pre_lines);
		if (g_active_esc_guard) g_active_esc_guard->resume();
		tui::PositionCursorForChat();

		{
			std::unique_lock<std::mutex> lk(race_mu);
			if (!tg_answered.load()) {
				// Local user answered first (picked >= 0): record result
				// and signal the Telegram thread to stop waiting.
				if (picked == 1) {
					always_allowed().insert(tool_name);
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
			// else (picked == -1): Telegram already answered; result is
			// already set by the tg_thread lambda above.
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
	// cannot show a y/a/n dialog. Either auto-approve from config / -y,
	// or fail loudly with a clear reason so the model sees why the call
	// was denied instead of silently narrating fake success.
	if (!isatty(fileno(stdin))) {
		if (g_allow_destructive_tools) {
			always_allowed().insert(tool_name);
			return Permission::Allow;
		}
		const std::string msg =
			"cannot prompt for " + tool_name + " permission: stdin is not a "
			"terminal. Re-run with -y/--yes to auto-approve destructive tools "
			"for this invocation, set \"fAllowDestructiveTools\": true in "
			+ paths::ConfigPath() + ", or use -i/--interactive from a real terminal.";
		std::cerr << tui::Meta("[tool: " + tool_name + " -> denied: no TTY to prompt]") << "\n"
				  << tui::Dim("  " + msg) << "\n";
		if (denial_reason) *denial_reason = msg;
		return Permission::Deny;
	}

	// Print the Preview + question text into the scroll region so
	// the history shows what was asked. Each piece ends with a
	// newline so the cursor advances cleanly.
	const std::string extra = tools::Preview(tool_name, input);
	int pre_lines = 0; // lines printed above the menu (to be erased with it)
	if (!extra.empty()) {
		std::cout << tui::Dim(extra) << "\n";
		// Count the lines occupied by the preview block: one per '\n' in
		// the string, plus the trailing '\n' we just emitted above.
		pre_lines = static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1;
	} else {
		std::cout << tui::Meta("  -> " + tool_name + " " + ShortInputSummary(input)) << "\n";
		pre_lines = 1;
	}

	// Render the three choices as a vertical arrow-key menu.
	// tui::SelectOption puts stdin into raw mode, draws the list,
	// and returns a 0-based index.  Passing the heading lets it own
	// the full block and replace it with a compact summary on selection.
	// pre_lines tells it to erase the preview above the menu too so the
	// entire question block is replaced by the single summary line.
	tui::PositionCursorForChat();
	const std::vector<std::string> choices = {
		"Yes, allow once",
		"Always allow this session",
		"No, deny",
	};
	// Pause the EscInterruptGuard background thread while SelectOption()
	// has exclusive ownership of stdin in raw mode. Without this, the
	// guard's thread races to read ESC bytes and arrow-key sequences
	// (ESC [ A/B) are either consumed before SelectOption sees them or
	// split across both readers — causing ↑/↓ to misbehave or register
	// as a "no" answer.
	if (g_active_esc_guard) g_active_esc_guard->pause();
	const int picked = tui::SelectOption(choices, "allow " + tool_name + "?",
										 nullptr, pre_lines);
	if (g_active_esc_guard) g_active_esc_guard->resume();
	tui::PositionCursorForChat();

	if (g_interrupted && picked >= static_cast<int>(choices.size()) - 1) {
		if (denial_reason) *denial_reason = "interrupted — permission denied for " + tool_name;
		return Permission::Deny;
	}
	if (picked == 1) {
		always_allowed().insert(tool_name);
		return Permission::Allow;
	}
	if (picked == 0) return Permission::Allow;
	if (denial_reason) {
		*denial_reason = "user declined permission for " + tool_name;
	}
	return Permission::Deny;
}

// Forward decl — definition lives further down with the other
// BFS helpers so it can use shell_single_quote.
static void AutoWriteSummaryIfMissing(const std::string& path,
										  const std::string& content);

SendResult SendWithTools(const Auth& auth, const std::string& model, int max_tokens,
						   json& messages, const std::string& custom_system) {
	SendResult aggregate;
	aggregate.exit_code = 0;

	// Clear any stale interrupt and flush the tty input queue BEFORE
	// starting the EscInterruptGuard thread. This eliminates the race
	// where:
	//   (a) A stale ESC byte left in the kernel tty buffer (e.g. the
	//       user pressed ESC twice in the previous turn) is read by the
	//       new guard thread and sets g_interrupted=1 AFTER the main
	//       thread's g_interrupted=0 clear.
	//   (b) g_interrupted still equals 1 from the previous turn when
	//       the guard thread starts and the thread somehow re-triggers
	//       before the clear.
	// Clearing + flushing first, then constructing the guard, means the
	// thread starts with a clean state: no stale bytes in the buffer and
	// g_interrupted already 0.
	g_interrupted = 0;
	if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);

	// Keep stdin in cbreak mode for the entire multi-turn tool loop so
	// Esc is detected not just during HTTP streaming but also while tools
	// are executing (Bash, WebFetch, etc.). EscInterruptGuard already
	// handles the non-TTY / isatty check internally and is a no-op there.
	EscInterruptGuard esc_guard;
	g_active_esc_guard = &esc_guard;
	// Clear the global pointer when SendWithTools returns so nobody
	// holds a dangling reference to esc_guard after it is destroyed.
	struct EscGuardScope {
		~EscGuardScope() { g_active_esc_guard = nullptr; }
	} esc_guard_scope;

	while (true) {
		// Honour any interrupt that arrived between turns (e.g. Esc pressed
		// while a tool was running but before the next API call started).
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
		// We keep plain text blocks since they're still valid.
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
			// exiting silently. "end_turn" is the expected happy
			// path; everything else (max_tokens, refusal, pause_turn,
			// stop_sequence without an explicit one configured) is
			// something the user should know about.
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
				LogLine("truncated stop_reason=max_tokens output=" + std::to_string(used));
			} else if (result.stop_reason == "refusal") {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " the model declined to answer (stop_reason=refusal).\n";
				LogLine("stop_reason=refusal");
			} else if (result.stop_reason == "pause_turn") {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " the model paused its turn (stop_reason=pause_turn);"
							 " re-send to continue.\n";
				LogLine("stop_reason=pause_turn");
			} else if (result.stop_reason != "end_turn"
					   && result.stop_reason != "stop_sequence"
					   && !result.stop_reason.empty()) {
				std::cerr << "\n" << tui::ErrorLabel()
						  << " unexpected stop_reason=" << result.stop_reason << "\n";
				LogLine("stop_reason=" + result.stop_reason);
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
			LogLine("tool " + tname + " input=" + ShortInputSummary(tinput));
			// Notify Telegram: tool about to run. Include the rich
			// preview (diff, file header, etc.) when available so the
			// remote user sees what they are about to approve, just like
			// the local preview printed above PromptPermission.
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
				// the terminal like a normal turn so the user can
				// follow along. The final text becomes this tool's
				// result.
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
				// tool run so the user sees continuous feedback
				// instead of a frozen cursor during multi-step
				// turns. Especially matters for Bash commands that
				// can take many seconds. Also update tool_phase so
				// the Telegram updater thread shows the tool name
				// instead of "thinking…" while it runs.
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

			// Auto-seed BFS cache: if Claude just Read a file
			// without a claude:summary attribute, write a
			// heuristic summary derived from the content we
			// already have in memory. Zero extra API calls —
			// next session's ReadAttr gets a free starting
			// point, which Claude can later overwrite with a
			// richer summary via WriteAttr.
			if (tname == "Read" && !tres.is_error) {
				AutoWriteSummaryIfMissing(
					tinput.value("path", std::string{}),
					tres.content);
			}

			// For BFS-native tools, measure actual bytes saved by
			// stat-ing the target file(s) and subtracting the tool's
			// own output size. ReadAttr compares against one file;
			// Query sums every path returned. The result feeds
			// stats::RecordTool so /stats can show the "BFS saved N
			// tokens" block.
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
			// Binary tool output (e.g. `cat` on a driver blob, /dev node,
			// or any file with non-UTF-8 bytes) will throw
			// json::type_error.316 if passed raw. Replace invalid bytes
			// with U+FFFD so the model still sees the content shape.
			tres.content = SanitizeUtf8(tres.content);

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

// Print a one-line token-usage summary to stderr. Called at the end of a
// one-shot invocation when -u / --usage is set, mirroring the per-turn
// [turn N in X/Y out X/Y] line that the interactive REPL always shows.
void PrintUsageLine(const SendResult& result) {
	std::cerr << "[usage] input: " << result.input_tokens
			  << " tokens  output: " << result.output_tokens << " tokens\n";
}

enum class SlashAction { Continue, Quit, Passthrough };

struct LoopCtx {
	const Auth&               auth;
	int                       max_tokens;
	const std::string&        custom_system;
	const json&               prices;
	std::string&              model;
	int&                      turn_count;
	int&                      session_input;
	int&                      session_output;
	json&                     messages;
	std::vector<std::string>& session_urls;
	bool&                     notify_enabled;
	double&                   notify_min_duration;
	// Invoked by slash commands that mutate state which shows up
	// in the fixed-bottom status frame (model name, turn counter,
	// session totals). Default is a no-op so non-REPL callers (the
	// Telegram bridge) don't need to wire anything up.
	std::function<void()>     redraw_status;
	// When true, /compact was triggered automatically by the
	// auto-compact threshold and must skip the confirmation
	// prompt. Cleared after each DispatchSlash call.
	bool                      auto_compact = false;
	// Session name forwarded to SaveHistory after auto-compact so
	// the right history file is updated. Empty = default session.
	std::string               resume_name  = {};
	// Thresholds forwarded from Config so the Telegram bridge can
	// trigger auto-compact after each turn, mirroring the REPL.
	double                    compact_auto_threshold  = 0.0;
	int                       compact_window_override = 0;
};


// Safely wrap a string in single quotes for a shell command. Any
// embedded `'` is split out as `'\''`. Used by /open so a URL with
// a `?q=foo'bar` query string can't break the command line.
static std::string shell_single_quote(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += '\'';
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else           out += c;
	}
	out += '\'';
	return out;
}

#ifdef __HAIKU__
// "What is this file about" distilled from content Claude
// just Read. No API call — first non-blank, non-comment
// line plus total-line-count, truncated to 80 chars. Good
// enough to let the next session tell "oh, this file is
// about X" via ReadAttr before deciding to Read the whole
// thing. Claude can later overwrite with a richer summary
// via WriteAttr; this is the floor, not the ceiling.
static std::string DeriveHeuristicSummary(const std::string& content) {
	size_t pos = 0;
	int    total_lines = 0;
	std::string first_meaningful;
	while (pos < content.size()) {
		const size_t nl  = content.find('\n', pos);
		const size_t len = (nl == std::string::npos ? content.size() : nl) - pos;
		++total_lines;
		if (first_meaningful.empty() && total_lines <= 50) {
			const std::string line = content.substr(pos, len);
			const size_t ws = line.find_first_not_of(" \t");
			if (ws != std::string::npos) {
				const std::string trimmed = line.substr(ws);
				if (trimmed.size() >= 2 &&
					trimmed.substr(0, 2) != "//" &&
					trimmed.substr(0, 2) != "/*" &&
					trimmed[0] != '#' && trimmed[0] != '*') {
					first_meaningful = trimmed.substr(0, 80);
				}
			}
		}
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	std::string out = std::to_string(total_lines) + "L";
	if (!first_meaningful.empty()) out += " \xE2\x80\x94 " + first_meaningful;
	return out;
}

// Probe a file for an existing non-empty claude:summary
// attribute. Best-effort — silent failures return false,
// which is safe (we'll attempt to write, addattr will just
// overwrite with ours or fail quietly).
static bool HasClaudeSummary(const std::string& path) {
	const std::string cmd =
		"catattr -d claude:summary " + shell_single_quote(path) +
		" 2>/dev/null";
	FILE* p = popen(cmd.c_str(), "r");  // flawfinder: ignore
	if (!p) return false;
	char buf[256];
	size_t total = 0;
	while (std::fgets(buf, sizeof(buf), p)) total += std::strlen(buf);
	pclose(p);
	return total > 1;  // >1 accounts for a trailing newline
}

// Auto-seed the BFS cache: if Claude just Read a file and
// it has no claude:summary yet, write a heuristic one
// derived from the content already in memory. Fork+exec
// addattr; the child fully detaches and redirects stdio so
// it can't interfere with the REPL's TUI state.
static void AutoWriteSummaryIfMissing(const std::string& path,
										  const std::string& content) {
	if (path.empty()) return;
	if (HasClaudeSummary(path)) return;
	const std::string summary = DeriveHeuristicSummary(content);
	if (summary.empty()) return;

	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		setsid();
		int devnull = ::open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		const char* argv[] = {
			"addattr", "-t", "string",
			"claude:summary", summary.c_str(), path.c_str(),
			nullptr
		};
		execvp("addattr", const_cast<char* const*>(argv));  // flawfinder: ignore
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
}
#else
static void AutoWriteSummaryIfMissing(const std::string&, const std::string&) {}
#endif

struct PriceEntry {
	double input;
	double output;
};

// Pick a reasonable context-window size for the given model.
// Config-override wins; otherwise the "[1m]" suffix signals the
// 1M-token Anthropic beta, and everything else gets the standard
// 200k window.
int DetectContextWindow(const std::string& model, int override_val) {
	if (override_val > 0) return override_val;
	if (model.find("[1m]") != std::string::npos) return 1'000'000;
	return 200'000;
}

PriceEntry get_price(const std::string& model, const json& config_prices) {
	if (config_prices.is_object() && config_prices.contains(model)) {
		const auto& p = config_prices[model];
		return { p.value("input", 0.0), p.value("output", 0.0) };
	}
	// Per-million-token fallbacks based on publicly listed Claude pricing.
	// Config file overrides these by adding an entry under "prices".
	if (model.find("opus")   != std::string::npos) return { 15.0, 75.0 };
	if (model.find("haiku")  != std::string::npos) return { 0.8,   4.0 };
	if (model.find("sonnet") != std::string::npos) return { 3.0,  15.0 };
	return { 3.0, 15.0 };
}

SlashAction DispatchSlash(const std::string& line, LoopCtx& ctx,
						   std::string& passthrough_out) {
	std::string cmd = line;
	std::string args;
	if (const auto sp = line.find(' '); sp != std::string::npos) {
		cmd  = line.substr(0, sp);
		args = line.substr(sp + 1);
		while (!args.empty() && args.front() == ' ') args.erase(args.begin());
	}

	if (cmd == "/help" || cmd == "/?") {
		std::cout << tui::Meta(
			"multi-line input:\n"
			"  \\ + Enter          new line (works everywhere, including SSH)\n"
			"  Ctrl+J             new line (local terminal)\n"
			"  Alt+Enter          new line (local terminal)\n"
			"\n"
			"slash commands:\n"
			"  /help              this list\n"
			"  /clear             reset the running conversation\n"
			"  /model [name]      list all available models, or swap to <name>\n"
			"  /compact           summarize and replace the running history\n"
			"  /usage             session tokens, cost estimate, subscription windows\n"
			"  /todos             show the current in-session todo list\n"
			"  /memory [user]     open CLAUDE.md in $EDITOR (project by default)\n"
			"  /stats             lifetime token usage and tool stats\n"
			"  /open [N|URL]      list URLs from this session, open #N, or open URL\n"
			"  /notify [on|off|S] desktop notification on slow turns (default 60s)\n"
			"  /remote-control    toggle Telegram remote control on/off\n"
			"  /ludicrous         toggle ludicrous mode (auto-approve all tool permissions)\n"
			"  /exit, /quit       leave the REPL (Ctrl+D also works)\n")
				  << "\n";
		const auto custom = commands::Names();
		if (!custom.empty()) {
			std::string body = "custom commands from .claude/commands/ and user dir:\n";
			for (const auto& c : custom) body += "  /" + c + "\n";
			std::cout << tui::Meta(body) << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/todos") {
		const auto result = tools::Run("TodoRead", json::object());
		std::cout << tui::Meta("current todos:") << "\n"
				  << result.content << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/stats") {
		std::cout << tui::Meta(stats::FormatDisplay()) << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/memory") {
		// With an explicit "user" arg go straight there; otherwise
		// show a picker so the user can choose project or user scope.
		std::string target;
		if (args == "user") {
			target = paths::UserMemoryPath();
			const auto slash = target.rfind('/');
			if (slash != std::string::npos) paths::MkdirP(target.substr(0, slash));
		} else if (!args.empty()) {
			// Any other explicit arg is treated as a direct path (future-proofing).
			target = paths::ProjectMemoryPath();
		} else {
			// Interactive picker: project vs user.
			const std::string proj = paths::ProjectMemoryPath();
			const std::string user = paths::UserMemoryPath();
			const std::vector<std::string> options = {
				"Project  (" + proj + ")",
				"User     (" + user + ")",
			};
			const int picked = tui::SelectOption(options, "open memory file:");
			if (picked == 1) {
				target = user;
				const auto slash = target.rfind('/');
				if (slash != std::string::npos) paths::MkdirP(target.substr(0, slash));
			} else {
				target = proj;
			}
		}
		const char* editor_env = std::getenv("EDITOR");  // flawfinder: ignore
		const std::string editor = editor_env && *editor_env ? editor_env : "nano";
		const std::string cmdline = editor + " '" + target + "'";
		std::cout << tui::Meta("[opening " + target + " with " + editor + "]") << "\n";
		const int rc = std::system(cmdline.c_str());  // flawfinder: ignore
		if (rc != 0) {
			std::cout << tui::Meta("[editor exited " + std::to_string(rc) + "]") << "\n";
		} else {
			std::cout << tui::Meta("[memory will be reloaded on the next turn]") << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/notify") {
		auto state_line = [&]() {
			char buf[128];
			std::snprintf(buf, sizeof(buf),
				"[notify: %s, threshold %.0fs]",
				ctx.notify_enabled ? "on" : "off",
				ctx.notify_min_duration);
			return std::string(buf);
		};
		if (args.empty()) {
			std::cout << tui::Meta(state_line()) << "\n";
			std::cout << tui::Dim("  /notify on | off | <seconds>") << "\n";
			return SlashAction::Continue;
		}
		if (args == "on")  { ctx.notify_enabled = true;  std::cout << tui::Meta(state_line()) << "\n"; return SlashAction::Continue; }
		if (args == "off") { ctx.notify_enabled = false; std::cout << tui::Meta(state_line()) << "\n"; return SlashAction::Continue; }

		// Numeric → new threshold. Accepts int or float seconds.
		// Negative or zero values disable without changing enabled.
		char* end = nullptr;
		const double v = std::strtod(args.c_str(), &end);
		if (end == args.c_str() || *end != '\0') {
			std::cout << tui::Meta("[/notify: expected 'on', 'off', or a number of seconds]") << "\n";
			return SlashAction::Continue;
		}
		if (v < 0.0) {
			std::cout << tui::Meta("[/notify: threshold must be >= 0]") << "\n";
			return SlashAction::Continue;
		}
		ctx.notify_min_duration = v;
		std::cout << tui::Meta(state_line()) << "\n";
		return SlashAction::Continue;
	}
	if (cmd == "/open") {
		std::string target;
		if (args.empty()) {
			if (ctx.session_urls.empty()) {
				std::cout << tui::Meta("[no URLs seen in this session yet]") << "\n";
				return SlashAction::Continue;
			}
			for (size_t i = 0; i < ctx.session_urls.size(); ++i) {
				const std::string idx = "  " + std::to_string(i + 1) + ". ";
				std::cout << tui::Meta(idx + ctx.session_urls[i]) << "\n";
			}
			std::cout << tui::Dim("  /open N to launch, or /open <url>") << "\n";
			return SlashAction::Continue;
		}

		bool is_num = !args.empty();
		for (char c : args) {
			if (!std::isdigit(static_cast<unsigned char>(c))) { is_num = false; break; }
		}
		if (is_num) {
			const size_t idx = static_cast<size_t>(std::atoi(args.c_str()));
			if (idx == 0 || idx > ctx.session_urls.size()) {
				std::cout << tui::Meta("[no URL #" + args + " — /open with no args to list]") << "\n";
				return SlashAction::Continue;
			}
			target = ctx.session_urls[idx - 1];
		} else {
			target = args;
		}

		// Fire-and-forget via `open` (Haiku's native URL launcher,
		// also present on macOS for the dev workflow). Background
		// the child so the REPL keeps its status frame; redirect
		// stdout/stderr so the launcher can't stomp on our TUI.
		const std::string cmdline =
			"open " + shell_single_quote(target) + " >/dev/null 2>&1 &";
		std::cout << tui::Meta("[opening " + target + "]") << "\n";
		const int rc = std::system(cmdline.c_str());  // flawfinder: ignore
		if (rc != 0) {
			std::cout << tui::Meta("[open exited " + std::to_string(rc) + "]") << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/exit" || cmd == "/quit") {
		return SlashAction::Quit;
	}
	if (cmd == "/ludicrous") {
		const bool now = !g_ludicrous_mode.load();
		g_ludicrous_mode.store(now);
		if (now) {
			std::cout << tui::Yellow("\xE2\x9A\xA1 LUDICROUS MODE ENGAGED")
					  << tui::Dim(" \xe2\x80\x94 all tool permissions auto-approved") << "\n";
		} else {
			std::cout << tui::Dim("\xE2\x9A\xA1 Ludicrous mode off \xe2\x80\x94 permission prompts restored") << "\n";
		}
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	if (cmd == "/clear") {
		ctx.messages        = json::array();
		ctx.turn_count      = 0;
		ctx.session_input   = 0;
		ctx.session_output  = 0;
		std::cout << tui::Meta("[conversation cleared]") << "\n";
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	if (cmd == "/model") {
		if (args.empty()) {
			// Fetch the model list and show an interactive picker.
			const auto models = fetch_models(ctx.auth);
			if (models.empty()) {
				std::cout << tui::Meta("[current model: " + ctx.model + "]") << "\n";
				std::cout << tui::Meta("[could not fetch model list — check connection/key]") << "\n";
				return SlashAction::Continue;
			}
			// Build option strings. Include display_name if it differs from id.
			std::vector<std::string> options;
			options.reserve(models.size());
			int preselect = 0;
			for (int i = 0; i < static_cast<int>(models.size()); ++i) {
				const auto& m = models[i];
				std::string label = m.id;
				if (m.display_name != m.id)
					label += "  (" + m.display_name + ")";
				options.push_back(std::move(label));
				if (m.id == ctx.model) preselect = i;
			}
			// Prime the selector on the currently active model.
			// SelectOption always starts at index 0; swap the active
			// model to the top so it's highlighted by default, then
			// map back to the real index after the user picks.
			// Simpler: just note the preselect index and pass it via
			// a wrapper that renders with the cursor starting there.
			// tui::SelectOption starts at 0, so we pass a reordered
			// list with the active model first, then remap.
			std::vector<std::string> ordered = options;
			std::vector<int>         index_map(models.size());
			std::iota(index_map.begin(), index_map.end(), 0);
			if (preselect > 0) {
				// Rotate so active model is first.
				std::rotate(ordered.begin(),
							ordered.begin() + preselect,
							ordered.end());
				std::rotate(index_map.begin(),
							index_map.begin() + preselect,
							index_map.end());
			}
			const int picked = tui::SelectOption(ordered, "select model:");
			const int real_idx = index_map[picked];
			const std::string chosen = models[real_idx].id;
			if (chosen != ctx.model) {
				ctx.model = chosen;
				std::cout << tui::Meta("[model set to " + ctx.model + "]") << "\n";
				if (ctx.redraw_status) ctx.redraw_status();
			} else {
				std::cout << tui::Meta("[model unchanged: " + ctx.model + "]") << "\n";
			}
		} else {
			ctx.model = args;
			std::cout << tui::Meta("[model set to " + ctx.model + "]") << "\n";
			if (ctx.redraw_status) ctx.redraw_status();
		}
		return SlashAction::Continue;
	}
	if (cmd == "/usage") {
		auto header = [](const std::string& key) -> std::string {
			const auto it = g_last_rate_headers.find(key);
			return it == g_last_rate_headers.end() ? std::string() : it->second;
		};

		auto render_bar = [](double pct) {
			constexpr int kBarWidth = 50;
			if (pct < 0.0)   pct = 0.0;
			if (pct > 100.0) pct = 100.0;
			const int filled = static_cast<int>(pct * kBarWidth / 100.0 + 0.5);
			std::string out;
			for (int i = 0; i < filled;                ++i) out += "\u2588";
			for (int i = 0; i < kBarWidth - filled;    ++i) out += ' ';
			return out;
		};

		auto format_reset = [](const std::string& ts_str) {
			if (ts_str.empty()) return std::string();
			const time_t ts = static_cast<time_t>(std::atoll(ts_str.c_str()));
			std::tm tm {};
			localtime_r(&ts, &tm);
			char out[64];
			std::strftime(out, sizeof(out), "%a %b %d at %H:%M (%Z)", &tm);
			return std::string(out);
		};

		auto print_window = [&](const std::string& label,
								const std::string& util_key,
								const std::string& reset_key) {
			const std::string util_s  = header(util_key);
			const std::string reset_s = header(reset_key);
			if (util_s.empty()) return;
			const double util = std::atof(util_s.c_str());
			const double pct  = util * 100.0;
			char pct_str[16];
			std::snprintf(pct_str, sizeof(pct_str), "%3.0f%% used", pct);
			std::cout << "  " << tui::Bold(label) << "\n"
					  << "  " << render_bar(pct) << " " << pct_str << "\n"
					  << "  " << tui::Dim("Resets " + format_reset(reset_s)) << "\n"
					  << "\n";
		};

		// Session summary (our own state).
		const PriceEntry price = get_price(ctx.model, ctx.prices);
		const double in_cost   = (ctx.session_input  / 1'000'000.0) * price.input;
		const double out_cost  = (ctx.session_output / 1'000'000.0) * price.output;
		char session_buf[512];
		std::snprintf(session_buf, sizeof(session_buf),
			"  model %s  turns %d  in %d  out %d  est $%.4f",
			ctx.model.c_str(),
			ctx.turn_count,
			ctx.session_input,
			ctx.session_output,
			in_cost + out_cost);
		std::cout << tui::Dim(session_buf) << "\n\n";

		if (header("anthropic-ratelimit-unified-5h-utilization").empty()) {
			std::cout << tui::Dim("(no rate-limit data yet — make a request first)")
					  << "\n";
			return SlashAction::Continue;
		}

		print_window("Current session",
					 "anthropic-ratelimit-unified-5h-utilization",
					 "anthropic-ratelimit-unified-5h-reset");
		print_window("Current week (all models)",
					 "anthropic-ratelimit-unified-7d-utilization",
					 "anthropic-ratelimit-unified-7d-reset");
		print_window("Current week (Sonnet only)",
					 "anthropic-ratelimit-unified-7d_sonnet-utilization",
					 "anthropic-ratelimit-unified-7d_sonnet-reset");

		const std::string claim = header("anthropic-ratelimit-unified-representative-claim");
		if (!claim.empty()) {
			std::cout << tui::Dim("  binding window: " + claim) << "\n";
		}
		return SlashAction::Continue;
	}
	if (cmd == "/compact") {
		if (ctx.messages.empty()) {
			std::cout << tui::Meta("[nothing to compact]") << "\n";
			return SlashAction::Continue;
		}
		// Confirmation prompt — skipped when auto-compact triggered
		// this call automatically (ctx.auto_compact == true).
		if (!ctx.auto_compact) {
			const std::vector<std::string> options = {
				"Yes, summarize and replace history",
				"No, keep history as-is",
			};
			const int picked = tui::SelectOption(options, "compact conversation history?");
			if (picked != 0) {
				std::cout << tui::Meta("[compact cancelled]") << "\n";
				return SlashAction::Continue;
			}
		}
		json request_messages = ctx.messages;
		request_messages.push_back({
			{"role",    "user"},
			{"content",
				"Two tasks, in order:\n"
				"\n"
				"1. For each source file you've gained real understanding "
				"of during this conversation, call WriteAttr to persist a "
				"concise one-line claude:summary capturing what the file "
				"is for. Only write for files you could confidently "
				"describe — skip files only mentioned in passing. "
				"WriteAttr is auto-approved and restricted to the "
				"claude:* namespace, so these writes are cheap and safe. "
				"This lets future sessions start with accurate summaries "
				"instead of the mechanical auto-seed placeholders.\n"
				"\n"
				"2. Then summarize the preceding conversation in 2-3 "
				"short paragraphs, preserving important context, "
				"decisions, code, and open questions. Reply with only "
				"the summary after the WriteAttr calls."},
		});
		std::cout << "\n" << tui::ClaudePrompt();
		const std::string compact_system = ComposeSystem(ctx.custom_system);
		// SendWithTools (not SendConversation) so the WriteAttr
		// calls inside step 1 actually Fire. The tool-use loop
		// terminates when Claude emits the final summary text block.
		const auto result = SendWithTools(
			ctx.auth, ctx.model, ctx.max_tokens,
			request_messages, compact_system);
		std::cout << "\n";
		if (result.exit_code != 0) {
			std::cout << tui::Meta("[compact failed]") << "\n";
			return SlashAction::Continue;
		}
		ctx.session_input  += result.input_tokens;
		ctx.session_output += result.output_tokens;
		ctx.messages = json::array({
			{{"role", "user"},      {"content", "[previous conversation context follows]"}},
			{{"role", "assistant"}, {"content", result.assistant_text}},
		});
		char note[96];
		std::snprintf(note, sizeof(note),
			"[compacted: %d in / %d out tokens]",
			result.input_tokens, result.output_tokens);
		std::cout << tui::Meta(note) << "\n";
		// Persist the compacted history so the next --resume or
		// crash-recovery loads the pruned state instead of the
		// original oversized session.
		SaveHistory(ctx.messages, ctx.model, ctx.resume_name);
		if (ctx.redraw_status) ctx.redraw_status();
		return SlashAction::Continue;
	}
	// Fall back to user-defined commands loaded from
	// .claude/commands/*.md (or the global dir). If a match exists we
	// substitute {{args}} and hand the expanded text back to the REPL
	// loop to send as a normal user message.
	const std::string cmd_name = cmd.substr(1); // drop leading '/'
	if (auto expanded = commands::Expand(cmd_name, args); expanded) {
		passthrough_out = std::move(*expanded);
		return SlashAction::Passthrough;
	}

	std::cout << tui::Meta("[unknown command: " + cmd + " — try /help]") << "\n";
	return SlashAction::Continue;
}

// Compact a raw token count into a k/M suffix so it fits on the
// bottom status row without dominating the line. Examples:
//   142   -> "142"
//   1832  -> "1.8k"
//   24110 -> "24k"
std::string CompactTokens(int n) {
	if (n < 1000) return std::to_string(n);
	if (n < 10000) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
		return buf;
	}
	if (n < 1000000) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%dk", n / 1000);
		return buf;
	}
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0);
	return buf;
}

// Compose the fixed-bottom status row for a regular REPL session.
// Format:
//   <short_model> · turn N · ↑ 1.2k · ↓ 420 · max 8192
// Padded with a leading space for visual breathing room. Caller
// is responsible for passing the numbers; this helper just
// formats. Truncates to TerminalWidth() minus 1 so the row never
// wraps to the next line (which would destroy the fixed frame).
std::string FormatStatusRow(const std::string& model,
							  int turn_count,
							  int session_input,
							  int session_output,
							  int max_tokens,
							  const std::string& right_label) {
	auto short_model = [](const std::string& m) {
		const std::string prefix = "claude-";
		if (m.rfind(prefix, 0) == 0) return m.substr(prefix.size());
		return m;
	};

	std::string left;
	left.reserve(96);
	left += " ";
	left += tui::Bold(short_model(model));
	left += tui::Muted(" \xC2\xB7 turn ") + tui::Muted(std::to_string(turn_count));
	left += tui::Muted(" \xC2\xB7 \xE2\x86\x91 ") + tui::Muted(CompactTokens(session_input));
	left += tui::Muted(" \xC2\xB7 \xE2\x86\x93 ") + tui::Muted(CompactTokens(session_output));
	left += tui::Muted(" \xC2\xB7 max ") + tui::Muted(std::to_string(max_tokens));

	if (right_label.empty()) return left;

	const int width = tui::TerminalWidth();
	if (width <= 0) return left + "  " + right_label;

	const int left_cols  = tui::DisplayWidth(left);
	const int right_cols = tui::DisplayWidth(right_label);
	const int gap        = (width - 1) - left_cols - right_cols;
	if (gap < 2) return left + "  " + right_label;
	return left + std::string(gap, ' ') + right_label + " ";
}

// Scan `text` for a numbered list at line start (`1. foo`, `2. bar`,
// ...). If at least two consecutive options are found, return them
// so the bridge can render inline-keyboard buttons. Each pair is
// {number_string, item_label}; the label is trimmed to ~28 chars
// so the buttons stay readable in Telegram.
std::vector<std::pair<std::string, std::string>>
extract_numbered_options(const std::string& text) {
	std::vector<std::pair<std::string, std::string>> out;
	std::istringstream iss(text);
	std::string        line;
	while (std::getline(iss, line)) {
		size_t i = 0;
		while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
		if (i == 0 || i > 2)                            continue; // need 1-2 digits
		if (i >= line.size() || line[i] != '.')         continue;
		if (i + 1 >= line.size() || line[i + 1] != ' ') continue;
		std::string number = line.substr(0, i);
		std::string label  = line.substr(i + 2);
		if (label.size() > 28) {
			label.resize(27);
			label += "\xE2\x80\xA6"; // …
		}
		out.emplace_back(std::move(number), std::move(label));
	}
	if (out.size() < 2) return {};
	return out;
}

// Self-contained background Telegram poller spawned on demand by
// the /remote-control slash command inside InteractiveLoop. Runs
// in its own thread, polls Telegram for incoming messages from
// allowed users, and hands each one to SendWithTools on the
// local machine. Each Telegram user gets an independent rolling
// history in fUserMessages (not shared with the REPL's own
// `messages`, but local turns are mirrored to the primary chat).
//
// Fully bidirectional: streaming edits, typing indicator,
// inline permission buttons, numbered-option buttons, and local
// input mirroring — identical experience to the full Telegram bridge.
//
// Thread-safe: the poller thread grabs fReplMutex for the
// duration of each SendWithTools call so it serializes cleanly
// against whatever the REPL's own main thread is doing. The REPL
// loop provides the mutex; RemoteControl only reads the reference.
class RemoteControl {
public:
	static bool config_is_valid(const Config& cfg, std::string* reason) {
		if (!cfg.telegram.is_object()) {
			if (reason) *reason = "config.telegram is missing from config.json";
			return false;
		}
		if (cfg.telegram.value("bot_token", std::string{}).empty()) {
			if (reason) *reason = "config.telegram.bot_token is not set";
			return false;
		}
		if (!cfg.telegram.contains("allowed_user_ids")
			|| !cfg.telegram["allowed_user_ids"].is_array()
			|| cfg.telegram["allowed_user_ids"].empty()) {
			if (reason) *reason = "config.telegram.allowed_user_ids must list at least one Telegram user ID";
			return false;
		}
		return true;
	}

	RemoteControl(const Config& cfg, const Auth& auth,
				  const std::string& custom_system)
		: fClient(cfg.telegram.value("bot_token", std::string{})),
		  fAuth(auth),
		  fCustomSystem(custom_system),
		  fCfgModel(cfg.model),
		  fCfgMaxTokens(cfg.max_tokens) {
		for (const auto& v : cfg.telegram["allowed_user_ids"]) {
			if (v.is_number_integer()) fAllowed.insert(v.get<int64_t>());
		}
		fAllowDestructive = cfg.telegram.value("fAllowDestructiveTools",
			cfg.telegram.value("fAllowDestructivetools", false));
	}

	~RemoteControl() { Stop(); }
	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;

	bool start() {
		if (fRunning.load()) return false;
		// Determine the primary user for local mirroring —
		// smallest fAlloweduser_id, same logic as InteractiveLoop's bridge.
		fPrimaryUserId = 0;
		for (const auto& id : fAllowed) {
			if (fPrimaryUserId == 0 || id < fPrimaryUserId) fPrimaryUserId = id;
		}
		// Default the active perm chat to the primary user so that
		// a locally-initiated turn can send permission prompts to
		// Telegram before any Telegram message has arrived.
		fActiveChatId = fPrimaryUserId;
		// Clear the always-allowed tool set so "Always allow this session"
		// approvals from a previous session don't silently carry over and
		// skip permission prompts in this new one.
		always_allowed().clear();
		fRunning.store(true);
		fWorkerRunning.store(true);
		fWorker = std::thread(&RemoteControl::work_loop, this);
		fPoller = std::thread(&RemoteControl::poll_loop, this);

		// Install global hooks once for the lifetime of this
		// RemoteControl session.  Both local REPL turns and
		// Telegram-origin turns will share these hooks; the active
		// chat ID (fActiveChatId) is updated by process_update()
		// before each Telegram-origin SendWithTools call, and
		// reset to fPrimaryUserId for local turns so permission
		// prompts always reach the right chat.
		g_telegram_permission_hook = [this](const std::string& tool_name,
											const std::string& preview,
											std::atomic<bool>* local_answered) -> Permission {
			const int64_t chat = fActiveChatId.load();
			if (chat == 0) return Permission::Deny;
			const std::string question =
				"\xF0\x9F\x94\x90 allow " + tool_name + "?\n\n" + preview;
			const std::vector<std::vector<telegram::Button>> kb = {
				{{ "1. Yes, allow once",          "perm:yes"    }},
				{{ "2. Always allow this session", "perm:always" }},
				{{ "3. No, deny",                 "perm:no"     }},
			};
			g_telegram_updater_paused.store(true);
			const int64_t perm_msg_id = fClient.SendMessageWithId(chat, question, kb);
			while (!g_interrupted) {
				// Bail early if the local user already answered.
				if (local_answered && local_answered->load()) {
					g_telegram_updater_paused.store(false);
					return Permission::Deny; // result is ignored by the race winner path
				}
				std::unique_lock<std::mutex> lk(fPermMu);
				fPermCv.wait_for(lk, std::chrono::seconds(2),
					[&]{ return !fPermQueue.empty() || g_interrupted
						|| (local_answered && local_answered->load()); });
				// Check local_answered again after wakeup.
				if (local_answered && local_answered->load()) {
					g_telegram_updater_paused.store(false);
					return Permission::Deny;
				}
				while (!fPermQueue.empty()) {
					const telegram::Update upd = fPermQueue.front();
					fPermQueue.pop_front();
					lk.unlock();
					g_telegram_updater_paused.store(false);
					fClient.AnswerCallback(upd.callback_query_id);
					// Replace the button message with a plain summary so
					// the answered permission prompt doesn't linger above
					// the final response with stale interactive buttons.
					if (upd.text == "perm:always") {
						always_allowed().insert(tool_name);
						fClient.EditMessageText(chat, perm_msg_id,
							"\xF0\x9F\x94\x90 allow " + tool_name
							+ "? \xE2\x86\x92 \xE2\x9C\x85 always allowed this session", {});
						return Permission::Allow;
					}
					if (upd.text == "perm:yes") {
						fClient.EditMessageText(chat, perm_msg_id,
							"\xF0\x9F\x94\x90 allow " + tool_name
							+ "? \xE2\x86\x92 \xE2\x9C\x85 allowed once", {});
						return Permission::Allow;
					}
					if (upd.text == "perm:no") {
						fClient.EditMessageText(chat, perm_msg_id,
							"\xF0\x9F\x94\x90 allow " + tool_name
							+ "? \xE2\x86\x92 \xF0\x9F\x9A\xAB denied", {});
						return Permission::Deny;
					}
					lk.lock();
				}
			}
			g_telegram_updater_paused.store(false);
			return Permission::Deny;
		};

		g_tool_status_hook = [this](const std::string& notice) {
			const int64_t chat = fActiveChatId.load();
			if (chat != 0 && !g_telegram_muted.load())
				fClient.SendMessage(chat, notice);
		};

		return true;
	}

	void Stop() {
		if (!fRunning.exchange(false)) return;
		g_telegram_permission_hook = nullptr;
		g_tool_status_hook         = nullptr;
		// Wake any thread blocked in AcquireTurn() so it sees
		// fRunning == false and unblocks cleanly.
		fTurnCv.notify_all();
		if (fPoller.joinable()) fPoller.join();
		// Stop the worker thread so it doesn't dangle after the
		// poller exits. Wake it so it unblocks from the empty
		// work_queue wait and sees fWorkerRunning == false.
		fWorkerRunning.store(false);
		fWorkCv.notify_one();
		if (fWorker.joinable()) fWorker.join();
	}

	bool running() const { return fRunning.load(); }

	// Serialise turns: both the local REPL and the Telegram worker
	// call AcquireTurn() before starting SendWithTools and
	// ReleaseTurn() when done.  This replaces the old scheme where
	// work_loop held fReplMutex across the entire SendWithTools
	// call, which blocked ReadMessage() and froze the local prompt
	// for the full duration of a Telegram-origin turn.
	//
	// AcquireTurn() blocks until no other turn is in progress, then
	// sets the token.  ReleaseTurn() clears it and wakes waiters.
	// Neither call holds a mutex across SendWithTools itself.
	void AcquireTurn() {
		std::unique_lock<std::mutex> lk(fTurnMu);
		fTurnCv.wait(lk, [this]{ return !fTurnInProgress || !fRunning.load(); });
		fTurnInProgress = true;
	}

	void ReleaseTurn() {
		{
			std::lock_guard<std::mutex> lk(fTurnMu);
			fTurnInProgress = false;
		}
		fTurnCv.notify_all();
	}

	// Mirror a local REPL turn to the primary Telegram chat so
	// the phone-side user sees what was typed locally. Called from
	// the main thread after each InteractiveLoop turn completes.
	// Send the user's local prompt to the primary Telegram chat
	// immediately — before Claude starts working — so the phone
	// side sees what is being asked in real time. Follows up with
	// a "⏳ thinking…" placeholder whose message_id is stored so
	// mirror_to_primary() can edit it in-place with the real reply.
	void mirror_prompt(const std::string& user_text) {
		if (!fRunning.load()) return;
		if (fPrimaryUserId == 0) return;
		if (g_telegram_muted.load()) return;
		fClient.SendMessage(fPrimaryUserId, "> " + user_text);
		// Post a "thinking" placeholder; store the id so the reply
		// can edit it rather than sending a separate message.
		fPrimaryThinkingMsgId = fClient.SendMessageWithId(
			fPrimaryUserId,
			"\xE2\x8F\xB3 thinking\xE2\x80\xA6"); // ⏳ thinking…
		// Animate the placeholder in the background until
		// mirror_to_primary() or mirror_cancel() is called.
		StartThinkingUpdater();
	}

	// Send Claude's reply to the primary Telegram chat after the
	// turn completes.  The prompt was already forwarded by
	// mirror_prompt(), so we only send the assistant text here.
	// If a "thinking" placeholder was posted it is edited in-place;
	// otherwise a fresh message is sent.
	void mirror_to_primary(const std::string& assistant_text) {
		StopThinkingUpdater();
		if (!fRunning.load()) return;
		if (fPrimaryUserId == 0) return;
		if (g_telegram_muted.load()) {
			fPrimaryThinkingMsgId = 0;
			return;
		}
		const int64_t ph = fPrimaryThinkingMsgId;
		fPrimaryThinkingMsgId = 0;
		if (!assistant_text.empty()) {
			if (ph != 0) {
				if (!fClient.EditMessageText(fPrimaryUserId, ph, assistant_text))
					fClient.SendMessage(fPrimaryUserId, assistant_text);
			} else {
				fClient.SendMessage(fPrimaryUserId, assistant_text);
			}
		} else if (ph != 0) {
			// Empty reply — remove the placeholder with a note.
			fClient.EditMessageText(fPrimaryUserId, ph, "(no response)");
		}
	}

	// Called when the turn aborts (auth error, etc.) so the
	// "thinking" placeholder is cleaned up instead of left dangling.
	void mirror_cancel() {
		StopThinkingUpdater();
		if (fPrimaryUserId == 0 || fPrimaryThinkingMsgId == 0) return;
		fClient.EditMessageText(fPrimaryUserId, fPrimaryThinkingMsgId,
								"\xE2\x9D\x8C error \xE2\x80\x94 turn aborted"); // ❌ error — turn aborted
		fPrimaryThinkingMsgId = 0;
	}

	// Start a background thread that animates the "thinking"
	// placeholder while SendWithTools is running. The thread edits
	// the message every second: it shows a rotating "⏳ thinking…"
	// dot animation until streaming starts, then shows the
	// accumulated streamed text + a block cursor (▌) so the phone
	// sees tokens arrive in near-real-time.
	// Call StopThinkingUpdater() (or mirror_to_primary()) to stop it.
	void StartThinkingUpdater() {
		if (fPrimaryThinkingMsgId == 0) return;
		fUpdaterRunning.store(true);
		fUpdaterThread = std::thread([this]() {
			int  dot_phase   = 0;
			int  last_version = 0;
			static const char* kDots[] = {
				"\xE2\x8F\xB3 thinking\xE2\x80\xA6",          // ⏳ thinking…
				"\xE2\x8F\xB3 thinking\xE2\x80\xA4",          // ⏳ thinking.
				"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4",       // ⏳ thinking..
				"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4\xE2\x80\xA4", // ⏳ thinking...
			};
			while (fUpdaterRunning.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1200));
				if (!fUpdaterRunning.load()) break;
				// Paused while a permission prompt is in flight.
				if (g_telegram_updater_paused.load()) continue;
				const int64_t ph = fPrimaryThinkingMsgId;
				if (ph == 0) break;
				if (g_stream_progress) {
					const int v = g_stream_progress->version.load(
						std::memory_order_relaxed);
					if (v != last_version) {
						last_version = v;
						std::string snap;
						{
							std::lock_guard<std::mutex> lk(g_stream_progress->mu);
							snap = g_stream_progress->text;
						}
						if (!snap.empty()) {
							fClient.EditMessageText(fPrimaryUserId, ph,
								snap + " \xE2\x96\x8C"); // ▌ streaming cursor
							continue;
						}
					}
				}
				// No streaming text yet — animate the dots.
				fClient.EditMessageText(fPrimaryUserId, ph,
					kDots[dot_phase % 4]);
				++dot_phase;
			}
		});
	}

	void StopThinkingUpdater() {
		fUpdaterRunning.store(false);
		if (fUpdaterThread.joinable()) fUpdaterThread.join();
	}

private:
	void poll_loop() {
		while (fRunning.load() && !g_interrupted) {
			// Pass &fRunning so the curl progress callback can
			// abort the in-flight long-poll when Stop() flips the
			// flag — otherwise /remote-control off would wait up
			// to ~20s for Telegram's long-poll to return.
			const auto updates = fClient.poll(10, &fRunning);
			if (!fRunning.load() || g_interrupted) break;
			for (const auto& u : updates) {
				if (!fRunning.load() || g_interrupted) break;
				if (!fAllowed.count(u.user_id)) {
					LogLine("remote-control reject user=" + std::to_string(u.user_id));
					continue;
				}
				// Route perm:* button taps to the permission queue so
				// the hook's wait loop sees them instead of the outer
				// poll consuming them first.
				if (u.is_callback && u.text.rfind("perm:", 0) == 0) {
					{
						std::lock_guard<std::mutex> lk(fPermMu);
						fPermQueue.push_back(u);
					}
					fPermCv.notify_one();
					continue;
				}
				// Push to the work queue so the worker thread processes
				// the turn. This keeps the poller free to long-poll
				// getUpdates and deliver perm:* callbacks while a turn
				// is in flight — fixing the deadlock where the
				// permission hook blocked waiting for a callback that
				// the poller couldn't receive.
				{
					std::lock_guard<std::mutex> lk(fWorkMu);
					fWorkQueue.push_back(u);
				}
				fWorkCv.notify_one();
			}
		}
	}

	void work_loop() {
		while (fWorkerRunning.load()) {
			telegram::Update job;
			{
				std::unique_lock<std::mutex> lk(fWorkMu);
				fWorkCv.wait(lk, [&]{
					return !fWorkQueue.empty() || !fWorkerRunning.load();
				});
				if (!fWorkerRunning.load()) break;
				job = fWorkQueue.front();
				fWorkQueue.pop_front();
			}
			// Serialise against local REPL turns via the turn-token
			// rather than holding fReplMutex across SendWithTools.
			// This keeps the local prompt responsive even when a
			// Telegram turn is running: ReadMessage() never blocks,
			// and only the moment the user hits Enter do they wait
			// (briefly) in AcquireTurn() for the current turn to end.
			AcquireTurn();
			process_update(job);
			ReleaseTurn();
		}
	}

	// Gate an outgoing client call on the shared mute flag so
	// /mute from either surface silences the remote too.
	void tg_send(int64_t chat, const std::string& text) {
		if (g_telegram_muted.load()) return;
		fClient.SendMessage(chat, text);
	}

	void process_update(const telegram::Update& u) {
		if (u.is_callback) fClient.AnswerCallback(u.callback_query_id);

		const std::string who = u.username.empty()
			? std::to_string(u.user_id) : u.username;

		// The poller thread runs while libedit has the cursor
		// parked on the fixed input row. Route all our stdout
		// writes into the scroll region via save/restore so we
		// don't clobber the prompt and libedit can redraw on the
		// next keystroke without damage.
		std::cout << "\x1b""7";          // save cursor
		tui::PositionCursorForChat();
		std::cout << tui::Meta("[remote " + who + "] " + u.text) << "\n";
		LogLine("remote-control rx user=" + std::to_string(u.user_id)
				 + " text=" + u.text);

		if (u.text == "/mute") {
			if (!g_telegram_muted.exchange(true)) {
				fClient.SendMessage(u.chat_id,
					"Remote muted. No replies until /unmute.");
			}
			std::cout << "\x1b""8" << std::flush;
			return;
		}
		if (u.text == "/unmute") {
			if (g_telegram_muted.exchange(false)) {
				fClient.SendMessage(u.chat_id,
					"Remote unmuted. Replies will be sent again.");
			}
			std::cout << "\x1b""8" << std::flush;
			return;
		}
		// /new is a Telegram-friendly alias for /clear.
		if (u.text == "/new") {
			fUserMessages.erase(u.user_id);
			tg_send(u.chat_id, "(history cleared)");
			std::cout << "\x1b""8" << std::flush;
			return;
		}

		// All other slash commands go through DispatchSlash.
		// /exit, /quit, and /remote-control are not meaningful here.
		if (!u.text.empty() && u.text.front() == '/') {
			const std::string cmd_word = u.text.substr(0, u.text.find(' '));
			if (cmd_word == "/exit" || cmd_word == "/quit"
					|| cmd_word == "/remote-control") {
				tg_send(u.chat_id, "(" + cmd_word + " is not available from Telegram)");
				std::cout << "\x1b""8" << std::flush;
				return;
			}
			const std::string dispatched = (u.text == "/start") ? "/help" : u.text;

			std::ostringstream capture;
			std::streambuf* old_buf = std::cout.rdbuf(capture.rdbuf());

			json& messages_ref = fUserMessages[u.user_id];
			if (!messages_ref.is_array()) messages_ref = json::array();
			// RemoteControl doesn't track session totals or URLs per
			// user; use local throwaway vars for the LoopCtx fields
			// that DispatchSlash may update (/model, /compact, etc.).
			int    rc_turn   = 0;
			int    rc_in     = 0;
			int    rc_out    = 0;
			bool   rc_notify = false;
			double rc_thresh = 60.0;
			std::vector<std::string> rc_urls;
			json   rc_prices = json::object();
			LoopCtx ctx{fAuth, fCfgMaxTokens, fCustomSystem, rc_prices,
						fCfgModel, rc_turn, rc_in, rc_out,
						messages_ref, rc_urls, rc_notify, rc_thresh,
						{}};
			std::string passthrough;
			const SlashAction action = DispatchSlash(dispatched, ctx, passthrough);

			std::cout.rdbuf(old_buf);
			const std::string output = capture.str();

			// Strip ANSI escape sequences before sending to Telegram.
			std::string plain;
			plain.reserve(output.size());
			for (size_t i = 0; i < output.size(); ) {
				if (output[i] == '\x1b' && i + 1 < output.size() && output[i+1] == '[') {
					i += 2;
					while (i < output.size()
							&& output[i] != 'm' && output[i] != 'K'
							&& output[i] != 'H' && output[i] != 'A'
							&& output[i] != 'B' && output[i] != 'J') ++i;
					if (i < output.size()) ++i;
				} else {
					plain += output[i++];
				}
			}
			while (!plain.empty() && (plain.front() == '\n' || plain.front() == ' '))
				plain.erase(plain.begin());
			while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' '))
				plain.pop_back();

			if (action == SlashAction::Passthrough) {
				const_cast<telegram::Update&>(u).text = passthrough;
				// Fall through to normal prompt handling below.
			} else {
				if (!plain.empty()) tg_send(u.chat_id, plain);
				std::cout << "\x1b""8" << std::flush;
				return;
			}
		}

		json& msgs = fUserMessages[u.user_id];
		if (!msgs.is_array()) msgs = json::array();

		// Point the persistent permission and status hooks at this
		// Telegram user's chat for the duration of this turn.
		fActiveChatId.store(u.chat_id);

		// Post a placeholder and start a streaming-edit updater
		// thread — identical to the full bridge.
		const int64_t placeholder_id = [&]() -> int64_t {
			if (g_telegram_muted.load()) return 0;
			return fClient.SendMessageWithId(u.chat_id, "\xE2\x80\xA6"); // …
		}();

		StreamProgress    progress;
		g_stream_progress = &progress;
		std::atomic<bool> updater_running { true };
		int               last_version = 0;
		int               dot_phase    = 0;
		std::thread updater([&]() {
			while (updater_running.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				if (!updater_running.load()) break;
				// Paused while a permission prompt is in flight.
				if (g_telegram_updater_paused.load()) continue;
				if (placeholder_id == 0) continue;

				// While a tool is actively running, show its name.
				std::string tool_ph;
				{
					std::lock_guard<std::mutex> lk(progress.mu);
					tool_ph = progress.tool_phase;
				}
				if (!tool_ph.empty()) {
					fClient.EditMessageText(u.chat_id, placeholder_id, tool_ph);
					continue;
				}

				const int v = progress.version.load(std::memory_order_relaxed);
				if (v == last_version) {
					static const char* kDots[] = {
						"\xE2\x8F\xB3 thinking\xE2\x80\xA6",
						"\xE2\x8F\xB3 thinking\xE2\x80\xA4",
						"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4",
						"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4\xE2\x80\xA4",
					};
					fClient.EditMessageText(u.chat_id, placeholder_id,
											kDots[dot_phase % 4]);
					++dot_phase;
				} else {
					last_version = v;
					std::string snap;
					{ std::lock_guard<std::mutex> lk(progress.mu); snap = progress.text; }
					if (!snap.empty())
						fClient.EditMessageText(u.chat_id, placeholder_id,
												snap + " \xE2\x96\x8C"); // ▌
				}
			}
		});

		const json snapshot = msgs;
		msgs.push_back({{"role", "user"}, {"content", u.text}});

		g_non_interactive_tools             = true;
		g_non_interactive_allow_destructive = fAllowDestructive;

		std::cout << tui::ClaudePrompt();
		const std::string effective_system = ComposeSystem(fCustomSystem);
		const auto result = SendWithTools(fAuth, fCfgModel, fCfgMaxTokens,
											msgs, effective_system);
		std::cout << "\n";
		g_non_interactive_tools = false;
		// Restore active chat to primary so any local-turn permission
		// prompts (after this Telegram turn) go to the right place.
		fActiveChatId.store(fPrimaryUserId);

		updater_running.store(false);
		if (updater.joinable()) updater.join();
		g_stream_progress = nullptr;

		if (result.exit_code != 0 || result.assistant_text.empty()) {
			msgs = snapshot;
			const std::string err = "(error: Claude did not return a response)";
			if (placeholder_id)
				fClient.EditMessageText(u.chat_id, placeholder_id, err);
			else if (!g_telegram_muted.load())
				fClient.SendMessage(u.chat_id, err);
			LogLine("remote-control tx user=" + std::to_string(u.user_id) + " -> error");
			std::cout << "\x1b""8" << std::flush;
			return;
		}

		// Numbered options → inline keyboard buttons.
		std::vector<std::vector<telegram::Button>> keyboard;
		const auto options = extract_numbered_options(result.assistant_text);
		for (const auto& opt : options) {
			telegram::Button b;
			b.text          = opt.first + ". " + opt.second;
			b.callback_data = opt.first;
			keyboard.push_back({ std::move(b) });
		}

		if (!g_telegram_muted.load()) {
			if (placeholder_id) {
				if (!fClient.EditMessageText(u.chat_id, placeholder_id,
												result.assistant_text, keyboard)) {
					fClient.SendMessage(u.chat_id, result.assistant_text, keyboard);
				}
			} else {
				fClient.SendMessage(u.chat_id, result.assistant_text, keyboard);
			}
		}
		LogLine("remote-control tx user=" + std::to_string(u.user_id)
				 + " out=" + std::to_string(result.output_tokens));
		std::cout << "\x1b""8" << std::flush;
	}

	telegram::Client             fClient;
	std::unordered_set<int64_t>  fAllowed;
	int64_t                      fPrimaryUserId = 0;
	int64_t                      fPrimaryThinkingMsgId = 0;
	// Chat ID that the persistent permission/status hooks target.
	// Set to the sender's chat_id at the start of each Telegram-origin
	// turn; reset to fPrimaryUserId when that turn ends so that local
	// REPL turns still route their permission prompts to Telegram.
	std::atomic<int64_t>         fActiveChatId { 0 };
	std::atomic<bool>            fUpdaterRunning { false };
	std::thread                  fUpdaterThread;
	bool                         fAllowDestructive = false;
	Auth                         fAuth;
	std::string                  fCustomSystem;
	std::string                  fCfgModel;
	int                          fCfgMaxTokens;
	std::map<int64_t, json>      fUserMessages;
	std::atomic<bool>            fRunning { false };
	std::thread                  fPoller;
	// Turn-token: serialises local and Telegram-origin turns without
	// holding a mutex across SendWithTools.  AcquireTurn() blocks
	// until fTurnInProgress is false, then sets it; ReleaseTurn()
	// clears it and wakes all waiters.
	std::mutex                   fTurnMu;
	std::condition_variable      fTurnCv;
	bool                         fTurnInProgress = false;
	// Worker thread for Telegram-origin turns. The poller pushes
	// non-perm updates to fWorkQueue; the worker pulls and processes
	// them one at a time, keeping the poller free to deliver perm:*
	// callbacks.
	std::deque<telegram::Update> fWorkQueue;
	std::mutex                   fWorkMu;
	std::condition_variable      fWorkCv;
	std::atomic<bool>            fWorkerRunning { false };
	std::thread                  fWorker;
	// Shared queue for perm:* callback taps. The poll_loop feeds it;
	// the permission hook drains it via fPermCv so they don't race
	// on fClient.poll() advancing the update offset.
	std::deque<telegram::Update> fPermQueue;
	std::mutex                   fPermMu;
	std::condition_variable      fPermCv;
};

// Break a libedit line into shell-style tokens so a drop of one
// or more paths from Tracker is correctly split. Single and double
// quotes preserve internal spaces; a literal `\` escapes the next
// char. Stops short of full shell expansion — no $VAR, no ~/, no
// globbing. Tracker drops arrive as absolute POSIX paths so the
// simple cases cover the real workflow.
static std::vector<std::string> shell_tokenize(const std::string& s) {
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
// Used to suppress the bracketed-paste multi-line input hint that is
// unreliable over SSH — Ctrl+J / Alt+Enter often don't reach the app
// when forwarded through an SSH multiplexer or basic client.
// The SSH daemon always exports at least one of these variables.
static bool IsSshSession() {
	return std::getenv("SSH_CLIENT")     != nullptr  // flawfinder: ignore
		|| std::getenv("SSH_TTY")        != nullptr  // flawfinder: ignore
		|| std::getenv("SSH_CONNECTION") != nullptr;  // flawfinder: ignore
}

// True if `line` is a drag-and-drop from Tracker: one or more tokens,
// every one an absolute path that stat-resolves. Bare filenames without
// a leading slash are NOT treated as drops so the user can still type
// "main.cpp" as a literal question without the REPL swallowing it.
//
// __attribute__((noinline)): GCC 13 LTO loses track of the vector
// element pointer across inlining into InteractiveLoop and emits a
// spurious -Wfree-nonheap-object. Keeping this function out-of-line
// gives the optimiser a clean boundary and silences the false positive.
static __attribute__((noinline))
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

// Compose the "Files attached:" preamble block the same way the
// one-shot path does, so drops and --attach produce the same shape
// of content for Claude to reason about.
static std::string ComposeAttachmentPreamble(const std::vector<std::string>& paths) {
	if (paths.empty()) return {};
	std::string s = "Files attached to this session:\n";
	for (const auto& p : paths) { s += "- "; s += p; s += '\n'; }
	s += '\n';
	return s;
}

// Dim `[attached: a, b, +N more]` line shown after a successful
// --attach or drop. Keeps the list short on narrow terminals.
static std::string FormatAttachedLine(const std::vector<std::string>& paths) {
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

int InteractiveLoop(const Auth& initial_auth, const Config& cfg,
					 const std::string& initial_model, int max_tokens,
					 const std::string& custom_system, const json& prices, bool resume,
					 const std::string& resume_name,
					 const std::string& initial_message,
					 std::vector<std::string> initial_attachments) {
	InterruptGuard interrupt_guard;
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
	Auth auth = initial_auth;

	hooks::Fire(hooks::Event::SessionStart, json::object());
	stats::RecordSession();
	LogLine("session start (model=" + model + ")");

	int turn_count         = 0;
	int session_input      = 0;
	int session_output     = 0;

	// Notification state — start from config, toggleable at runtime
	// via `/notify on|off|<seconds>`. Local copies so the user can
	// experiment without rewriting config.json.
	bool   notify_enabled       = cfg.notify_enabled;
	double notify_min_duration  = cfg.notify_min_duration_sec;

	// Auto-compact thresholds. context_window resolves to the
	// model-specific cap on first use (handled inside the loop so
	// a `/model` swap mid-session picks up the new window).
	const double compact_auto_threshold = cfg.compact_auto_threshold;
	const int    compact_window_override = cfg.compact_context_window;

	if (resume) {
		if (auto loaded = LoadHistory(resume_name); loaded && loaded->is_array()) {
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

	std::unique_ptr<RemoteControl> remote;

	// Remote-control is off by default even when telegram config is present.
	// Use /remote-control (or /remote-control on) to start it explicitly.

	// Compose the status row including a green "Remote Control
	// active" right-label whenever the remote poller is running.
	// Mute state gets an appended "· muted" marker in yellow.
	auto compose_status = [&]() {
		std::string right;
		if (g_ludicrous_mode.load()) {
			right = tui::Yellow("\xE2\x9A\xA1 LUDICROUS");
		}
		if (remote && remote->running()) {
			if (!right.empty()) right += tui::Dim("  \xC2\xB7  ");
			right += tui::Green("Remote Control active");
			if (g_telegram_muted.load()) {
				right += tui::Yellow(" \xC2\xB7 muted");
			}
		}
		return FormatStatusRow(model, turn_count, session_input,
								 session_output, max_tokens, right);
	};

	// Install the fixed-bottom status frame after the welcome text
	// so the scrolling welcome lines live in the chat history
	// region. RAII teardown happens in the StatusFrame guard below.
	tui::InstallStatusBar(compose_status());
	struct StatusFrameGuard {
		~StatusFrameGuard() { tui::TeardownStatusBar(); }
	} status_frame_guard;

	std::string pending = initial_message;

	// Paths announced to Claude on the next outgoing user turn.
	// Starts with any --attach values and grows each time the user
	// drops a file from Tracker onto the Terminal window. Drained
	// exactly once per turn, then refills from subsequent drops.
	std::vector<std::string> pending_paths = std::move(initial_attachments);

	// URLs seen in assistant replies so far this session. Populated
	// after each turn by ExtractUrls; consumed by `/open` for
	// numbered launches.
	std::vector<std::string> session_urls;

	while (true) {
		// Resize events rebuild the scroll region and redraw the
		// fixed rows so the frame stays correct after the user
		// drags the terminal window. Also unconditionally show
		// the cursor in case a previous interrupt left it hidden
		// — cheap, runs once per prompt.
		if (tui::ConsumeResizePending()) tui::RedrawStatusBar();
		tui::ShowCursor();
		tui::PositionCursorForInput();

		std::string line;
		if (!pending.empty()) {
			line    = std::move(pending);
			pending.clear();
			tui::PositionCursorForChat();
			std::cout << tui::UserPrompt() << line << "\n";
		} else {
			if (!repl::ReadMessage(tui::UserPrompt(),
									tui::ContinuationPrompt(),
									line)) {
				std::cout << "\n";
				break;
			}
			tui::ClearInputRow();
			// libedit drew the prompt on the fixed input row and
			// echoed a newline on enter. Push the cursor back
			// into the scroll region and replay the submitted
			// line so the chat history preserves a Record of
			// the user's message.
			tui::PositionCursorForChat();
			if (!line.empty()) {
				std::cout << tui::UserPrompt() << line << "\n";
			}
		}

		while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
			line.pop_back();
		}
		if (line.empty()) continue;

		// Drag-and-drop from Tracker: Haiku's Terminal inserts the
		// dropped file's POSIX path into the active input line (quoted
		// if it contains spaces). If libedit hands back a line that's
		// purely one-or-more absolute paths that all exist on disk,
		// treat it as an attachment event rather than a prompt: stash
		// the paths, acknowledge with a dim meta line, and wait for
		// the user's actual question. The leading-slash requirement
		// avoids swallowing bare filenames like "main.cpp" that a user
		// might type as a literal question.
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
			// poller on and off. DispatchSlash doesn't know about
			// it because the REPL's own telegram mode is a separate
			// subcommand with its own entry point; here we own the
			// lifecycle directly.
			//
			// Parse the head of the line as `/remote-control`
			// followed by an optional ` on` / ` off` / ` status`
			// suffix so users can type any of them without being
			// too picky about exact whitespace.
			auto starts_with = [](const std::string& s, const char* prefix) {
				const size_t n = std::strlen(prefix);
				return s.size() >= n && s.compare(0, n, prefix) == 0;
			};
			if (starts_with(line, "/remote-control")) {
				std::string arg;
				if (line.size() > std::strlen("/remote-control")) {
					arg = line.substr(std::strlen("/remote-control"));
					// Trim leading / trailing whitespace.
					size_t a = 0, b = arg.size();
					while (a < b && (arg[a] == ' ' || arg[a] == '\t')) ++a;
					while (b > a && (arg[b - 1] == ' ' || arg[b - 1] == '\t')) --b;
					arg = arg.substr(a, b - a);
				}
				LogLine("slash remote-control arg='" + arg + "'");

				// /remote-control with no arg toggles. Explicit
				// "on" / "off" force the state.
				const bool currently_running = remote && remote->running();
				const bool want_off = (arg == "off")
					|| (arg.empty() && currently_running);
				const bool want_on  = (arg == "on")
					|| (arg.empty() && !currently_running);

				if (want_off) {
					if (currently_running) {
						remote->Stop();
						std::cout << tui::Meta("[remote control: telegram poller stopped]") << "\n";
						LogLine("remote control stopped from /remote-control" +
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
						if (!RemoteControl::config_is_valid(cfg, &why)) {
							std::cout << tui::Meta("[remote control: " + why + "]") << "\n"
									  << tui::Dim("  Add a 'telegram' block to config.json with\n"
												  "  bot_token and allowed_user_ids.\n"
												  "  See the Telegram setup section in README.md.") << "\n";
							LogLine("remote control config invalid: " + why);
						} else {
							try {
								if (!remote) {
									remote = std::make_unique<RemoteControl>(
										cfg, auth, custom_system);
								}
								if (remote->start()) {
									std::cout << tui::Meta("[remote control: telegram poller started]") << "\n";
									LogLine("remote control started from /remote-control");
									tui::SetStatusBar(compose_status());
								} else {
									std::cout << tui::Meta("[remote control: start() returned false — already running?]") << "\n";
								}
							} catch (const std::exception& e) {
								std::cout << tui::Meta(std::string("[remote control error: ") + e.what() + "]") << "\n";
								LogLine(std::string("remote control construction failed: ") + e.what());
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

			LoopCtx ctx{auth, max_tokens, custom_system, prices, model,
						turn_count, session_input, session_output, messages,
						session_urls, notify_enabled, notify_min_duration,
						[&]() { tui::SetStatusBar(compose_status()); }};
			ctx.resume_name            = resume_name;
			ctx.compact_auto_threshold  = compact_auto_threshold;
			ctx.compact_window_override = compact_window_override;
			std::string expanded;
			const SlashAction action = DispatchSlash(line, ctx, expanded);
			repl::Record(line);
			recordedBySlashCmd = true;
			if (action == SlashAction::Quit) break;
			if (action == SlashAction::Continue) continue;
			if (action == SlashAction::Passthrough) {
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

		// Prepend the accumulated attachment preamble (from --attach
		// at launch or any drops during the session) silently to the
		// outgoing API content. The replay and hooks payload keep
		// only the user's actual typed text.
		std::string api_content = line;
		if (!pending_paths.empty()) {
			api_content = ComposeAttachmentPreamble(pending_paths) + api_content;
			pending_paths.clear();
		}
		const json snapshot = messages;
		messages.push_back({{"role", "user"}, {"content", api_content}});

		std::cout << "\n" << tui::ClaudePrompt();
		const auto turn_start = std::chrono::steady_clock::now();
		const std::string system_for_turn = ComposeSystem(custom_system);
		// Refresh the OAuth token if it's about to expire so
		// long-running REPL sessions don't fail mid-conversation.
		// ResolveAuth is cheap — reads credentials.json and
		// checks the clock; refresh is only attempted when the
		// current token is within 60 s of expiry.
		auth = ResolveAuth();
		if (auth.kind == AuthKind::None) {
			std::cout << "\n" << tui::Meta("[error: authentication expired — run /exit and `claude login`]") << "\n";
			messages = snapshot;
			continue;
		}
		// Echo the user's local prompt to Telegram immediately so
		// the phone side sees what is being asked before Claude
		// starts working on it.
		if (remote && remote->running()) {
			remote->mirror_prompt(line);
		}

		// Set up stream progress so the remote-control thinking
		// updater can show live streaming text on the phone side.
		StreamProgress stream_progress;
		if (remote && remote->running())
			g_stream_progress = &stream_progress;

		// Serialise against any concurrent Telegram-origin turn
		// via the turn-token.  AcquireTurn() blocks only until the
		// current Telegram turn finishes (if one is running), then
		// marks us as the active turn.  Unlike the old lock_guard
		// approach, ReadMessage() above was never blocked — only
		// this post-Enter path waits, so the terminal stays live.
		//
		// Re-check running() after AcquireTurn() in case Stop()
		// woke us via fTurnCv but /remote-control was toggled off
		// while we were waiting — in that case just run without
		// the turn-token bookkeeping.
		SendResult result;
		if (remote && remote->running()) {
			remote->AcquireTurn();
			if (remote->running()) {
				result = SendWithTools(auth, model, max_tokens, messages, system_for_turn);
				remote->ReleaseTurn();
			} else {
				remote->ReleaseTurn();
				result = SendWithTools(auth, model, max_tokens, messages, system_for_turn);
			}
		} else {
			result = SendWithTools(auth, model, max_tokens, messages, system_for_turn);
		}

		g_stream_progress = nullptr;
		const double elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - turn_start).count();
		std::cout << "\n";

		if (result.exit_code != 0) {
			messages = snapshot;
			if (remote && remote->running())
				remote->mirror_cancel();
			continue;
		}

		++turn_count;
		session_input  += result.input_tokens;
		session_output += result.output_tokens;
		stats::RecordTurn(result.input_tokens, result.output_tokens);

		// Harvest any URLs Claude mentioned so `/open N` can launch
		// them later. Dedup in insertion order so the index stays
		// stable across turns.
		for (auto& url : notify::ExtractUrls(result.assistant_text)) {
			if (std::find(session_urls.begin(), session_urls.end(), url)
				== session_urls.end()) {
				session_urls.push_back(std::move(url));
			}
		}

		// Desktop notification for slow turns. The threshold default
		// (60 s) targets the "I walked away from the laptop" case —
		// fast replies on a user sitting at the keyboard don't buzz.
		// Body is the first sentence of the reply so the user can
		// glance at the notification and know whether to come back.
		if (notify_enabled && elapsed >= notify_min_duration) {
			notify::Send(
				notify::PickPlayfulTitle(elapsed),
				notify::FirstSentence(result.assistant_text, 120));
		}

		// Auto-compact: if this turn's input-token count crosses
		// the threshold share of the model's context window, fire
		// /compact immediately (no confirmation prompt) and save the
		// pruned history so the next --resume or crash-recovery
		// loads the compacted state.
		if (compact_auto_threshold > 0.0) {
			const int window = DetectContextWindow(model, compact_window_override);
			const int trigger = static_cast<int>(window * compact_auto_threshold);
			if (result.input_tokens >= trigger) {
				char note[160];
				std::snprintf(note, sizeof(note),
					"[auto-compact: context at %d%% (%d / %d tokens) — compacting now]",
					(result.input_tokens * 100) / window,
					result.input_tokens, window);
				std::cout << tui::Meta(note) << "\n";
				// Build a LoopCtx with auto_compact=true so the
				// /compact handler skips the SelectOption dialog.
				// Pass resume_name so SaveHistory writes the right
				// file after compacting.
				LoopCtx ac_ctx{auth, max_tokens, custom_system, prices, model,
							   turn_count, session_input, session_output, messages,
							   session_urls, notify_enabled, notify_min_duration,
							   [&]() { tui::SetStatusBar(compose_status()); }};
				ac_ctx.auto_compact          = true;
				ac_ctx.resume_name           = resume_name;
				ac_ctx.compact_auto_threshold  = compact_auto_threshold;
				ac_ctx.compact_window_override = compact_window_override;
				std::string dummy;
				DispatchSlash("/compact", ac_ctx, dummy);
			}
		}

		char status[256];
		// Append cache info when either creation or read is non-zero
		// so the user can see caching working without digging into
		// /stats. " · cache R:N W:N" tail — R = read (near-instant
		// reuse), W = write (new content, cached for next turn).
		char cache_tail[64] = {0};
		const int c_read  = result.cache_read_input_tokens;
		const int c_write = result.cache_creation_input_tokens;
		if (c_read > 0 || c_write > 0) {
			std::snprintf(cache_tail, sizeof(cache_tail),
				"  \xC2\xB7 cache R:%d W:%d", c_read, c_write);
		}
		std::snprintf(status, sizeof(status),
			"[turn %d  %.1fs  in %d/%d  out %d/%d%s]",
			turn_count, elapsed,
			result.input_tokens, session_input,
			result.output_tokens, session_output,
			cache_tail);
		std::cout << tui::Meta(status) << "\n";
		LogLine("turn " + std::to_string(turn_count)
				 + " model=" + model
				 + " in=" + std::to_string(result.input_tokens)
				 + " out=" + std::to_string(result.output_tokens)
				 + " cache_r=" + std::to_string(c_read)
				 + " cache_w=" + std::to_string(c_write));

		// Push the updated session counters into the fixed-bottom
		// status row so the user sees the running totals without
		// having to pull up /usage. Uses compose_status so the
		// "Remote Control active" label reflects the current
		// toggle state.
		tui::SetStatusBar(compose_status());

		// Mirror the local turn to the primary Telegram chat so
		// the phone-side user sees the conversation in real time
		// when /remote-control is active.
		if (remote && remote->running()) {
			remote->mirror_to_primary(result.assistant_text);
		}

		SaveHistory(messages, model, resume_name);

		hooks::Fire(hooks::Event::Stop, json{{"assistant_text", result.assistant_text}});
	}
	return 0;
}

} // namespace

int main(int argc, char* argv[]) {
	tui::Init();
	tui::InstallSigwinchHandler();

	// Crash-safe teardown. std::atexit fires on normal return,
	// exit(), and unhandled exceptions (via terminate). Signal
	// handlers for SIGINT / SIGTERM below call it explicitly
	// before re-raising so Ctrl+C out of a REPL also restores
	// the scroll region. Without this, a crashed bridge leaves
	// the user's terminal with a truncated scroll region stuck
	// above the now-missing status row.
	std::atexit([]() {
		repl::Deinit();           // disable bracketed paste mode
		tui::TeardownStatusBar();
	});
	{
		struct sigaction sa {};
		sa.sa_handler = [](int sig) {
			repl::Deinit();           // disable bracketed paste mode
			tui::TeardownStatusBar();
			// Re-raise with the default handler so the exit
			// status reflects the signal, not a clean return.
			struct sigaction dfl {};
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			sigaction(sig, &dfl, nullptr);
			raise(sig);
		};
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGTERM, &sa, nullptr);
		// Note: we deliberately do NOT install this for SIGINT
		// because InterruptGuard already has a SIGINT handler
		// that sets g_interrupted, and replacing it here would
		// break Ctrl+C cancellation of in-flight requests.
		// TeardownStatusBar will still Fire via std::atexit
		// when the REPL loop exits cleanly after the interrupt.
	}

	if (argc >= 2) {
		const std::string cmd = argv[1];
		if (cmd == "login")  return DoLogin();
		if (cmd == "logout") return DoLogout();
	}

	const Config cfg = load_config();
	InitLogging(cfg.logging_enabled);
	hooks::Load(cfg.hooks);
	mcp::Init(cfg.mcp_servers);

#ifdef __HAIKU__
	// Ensure the claude:summary BFS index exists on this volume so
	// Query("\"claude:summary\" == \"*\"") runs in O(1) rather than
	// walking every file. mkindex is idempotent: it exits non-zero
	// with "File or Directory already exists" when the index is
	// already present, which we intentionally ignore. Fork+exec keeps
	// the failure mode fully silent — no output, no effect on the
	// running process if mkindex is absent or the volume is read-only.
	{
		pid_t pid = fork();
		if (pid == 0) {
			// Child: redirect stdio to /dev/null, exec mkindex.
			int devnull = ::open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > 2) close(devnull);
			}
			const char* argv_mk[] = {
				"mkindex", "-t", "string", "claude:summary", nullptr
			};
			execvp("mkindex", const_cast<char* const*>(argv_mk));  // flawfinder: ignore
			_exit(127);
		}
		if (pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0); // reap; ignore exit code
		}
	}
#endif

	std::string              model         = cfg.model;
	int                      max_tokens    = cfg.max_tokens;
	bool                     interactive   = false;
	bool                     show_usage    = cfg.show_usage;
	bool                     resume        = false;
	std::string              resume_name;   // empty = default history.json
	std::string              custom_system = cfg.system;  // flawfinder: ignore
	std::vector<std::string> parts;
	std::vector<std::string> attachments;

	// Seed the destructive-tool flag from config. -y/--yes below can
	// still flip it on for ad-hoc runs; there's no reason to flip it
	// off mid-invocation so we don't expose a --no-yes counterpart.
	if (cfg.fAllowDestructiveTools) g_allow_destructive_tools = true;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0], model, max_tokens);
			return 0;
		}
		if (arg == "-V" || arg == "--version") {
			std::cout << "haiku-claude-cli " << kVersion << "\n";
			return 0;
		}
		if (arg == "-i" || arg == "--interactive") {
			interactive = true;
			continue;
		}
		if (arg == "-u" || arg == "--usage") {
			show_usage = true;
			continue;
		}
		if (arg == "-r" || arg == "--resume") {
			resume      = true;
			interactive = true;
			// Optional next argument is the session name. Consume it
			// only if it doesn't look like a flag (i.e. doesn't start
			// with '-') so `claude -r -i` still works.
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				resume_name = argv[++i];
			}
			continue;
		}
		if (arg == "-y" || arg == "--yes") {
			g_allow_destructive_tools = true;
			continue;
		}
		if (arg == "-a" || arg == "--attach") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a path\n";
				return 1;
			}
			attachments.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--plain") {
			tui::SetColorEnabled(false);
			continue;
		}
		if (arg == "--color") {
			tui::SetColorEnabled(true);
			continue;
		}
		if (arg == "-m" || arg == "--model") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			model = argv[++i];
			continue;
		}
		if (arg == "-t" || arg == "--max-tokens") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			max_tokens = std::atoi(argv[++i]);
			if (max_tokens <= 0) {
				std::cerr << "error: --max-tokens must be a positive integer\n";
				return 1;
			}
			continue;
		}
		if (arg == "-s" || arg == "--system") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			custom_system = argv[++i];
			continue;
		}
		parts.push_back(arg);
	}

	std::string message;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i > 0) message += ' ';
		message += parts[i];
	}

	if (!interactive && !isatty(fileno(stdin))) {
		// Only slurp stdin if data is actually ready. Without this
		// check, fread blocks forever when stdin is an open-but-empty
		// pipe (e.g. `ssh host 'claude hi'` without -t, or CI jobs
		// that inherit a runner's idle stdin). 100ms is imperceptible
		// for real pipelines like `cat file | claude "summarize"` but
		// saves the invocation from hanging in non-interactive shells.
		struct pollfd pfd;
		pfd.fd      = STDIN_FILENO;
		pfd.events  = POLLIN;
		pfd.revents = 0;
		const bool has_input =
			poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP));
		if (has_input) {
			std::string stdin_data;
			char        buf[4096];
			size_t      n;
			while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
				stdin_data.append(buf, n);
			}
			while (!stdin_data.empty() && (stdin_data.back() == '\n' || stdin_data.back() == '\r')) {
				stdin_data.pop_back();
			}
			if (!stdin_data.empty()) {
				if (message.empty()) {
					message = std::move(stdin_data);
				} else {
					message += "\n\n";
					message += stdin_data;
				}
			}
		}
	}

	if (!interactive && message.empty()) {
		// With no message and no -i flag, default to interactive mode
		// when stdin is a real terminal. Pipe/redirected cases still
		// get the usage error so scripts fail loudly on empty input.
		if (isatty(fileno(stdin))) {
			interactive = true;
		} else {
			PrintUsage(argv[0], model, max_tokens);
			return 1;
		}
	}

	// Resolve --attach arguments to absolute paths so Claude's Read
	// tool doesn't depend on whatever cwd the REPL inherited. Missing
	// paths fail loudly — a shell user wants to catch a typo at the
	// door instead of silently announcing a non-existent file.
	std::vector<std::string> resolved_attachments;
	if (!attachments.empty()) {
		resolved_attachments.reserve(attachments.size());
		for (const auto& p : attachments) {
			struct stat st;
			if (stat(p.c_str(), &st) != 0) {
				std::cerr << "error: --attach path not found: " << p << "\n";
				return 1;
			}
			char abs[PATH_MAX];
			const char* use = realpath(p.c_str(), abs) ? abs : p.c_str();  // flawfinder: ignore
			resolved_attachments.emplace_back(use);
		}
		std::cout << tui::Meta(FormatAttachedLine(resolved_attachments)) << "\n";
	}

	const Auth auth = ResolveAuth();
	if (auth.kind == AuthKind::None) {
		std::cerr << "error: no authentication configured.\n"
				  << "Run '" << argv[0] << " login' to authenticate with your Claude account,\n"
				  << "or set ANTHROPIC_API_KEY.\n";
		return 1;
	}

	if (interactive) {
		return InteractiveLoop(auth, cfg, model, max_tokens, custom_system, cfg.prices, resume, resume_name, message, std::move(resolved_attachments));
	}

	InterruptGuard interrupt_guard;
	// One-shot: bake the attachment preamble into the single user
	// turn. Interactive mode defers this to the first REPL turn and
	// also accepts drops mid-session.
	const std::string one_shot_content = ComposeAttachmentPreamble(resolved_attachments) + message;
	json messages = json::array({{{"role", "user"}, {"content", one_shot_content}}});
	const std::string effective_system = ComposeSystem(custom_system);
	const auto result = SendWithTools(auth, model, max_tokens, messages, effective_system);
	if (show_usage) {
		PrintUsageLine(result);
	}
	return result.exit_code;
}
