#ifndef HAIKU_CLAUDE_CLI_API_H
#define HAIKU_CLAUDE_CLI_API_H

#include <atomic>
#include <csignal>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "output_sink.h"

// Anthropic Messages API client and tool-use loop. SendConversation
// does one streamed POST to /v1/messages; SendWithTools wraps that
// in a loop that dispatches each tool_use block back through the
// tools:: registry, re-feeding tool_results until the model stops
// requesting tools.

// RAII handle so terminal_sink.cpp can pause/resume the ESC-interrupt
// background thread around SelectOption calls without knowing the
// concrete EscInterruptGuard type (which is private to api.cpp).
struct EscInterruptGuardHandle {
	virtual void pause()  = 0;
	virtual void resume() = 0;
	virtual ~EscInterruptGuardHandle() = default;
};

namespace api {

using json = nlohmann::json;

// api::Permission is defined in output_sink.h; it is in the api::
// namespace there so all existing api::Permission references compile.

struct SendResult {
	int                exit_code = 0;
	std::string        assistant_text;
	int                input_tokens  = 0;
	int                output_tokens = 0;
	std::vector<json>  content_blocks;
	std::string        stop_reason;
	// Anthropic prompt-cache usage for this call. cache_read > 0 on
	// repeat turns means the system+tools prefix is reused from
	// Anthropic's server-side cache — faster TTFT, 10% of normal
	// input-token cost on that portion.
	int                cache_creation_input_tokens = 0;
	int                cache_read_input_tokens     = 0;
};

// Used by session.cpp's LocalWorker to animate the Telegram mirror
// placeholder for locally-initiated turns (StartThinkingUpdater).
// Separate from the TelegramSink streaming path for Telegram-origin
// turns, which no longer uses this struct.
struct StreamProgress {
	std::mutex        mu;
	std::string       text;
	std::atomic<int>  version {0};
	std::string       tool_phase; // unused after Step 3; kept for compat
};

// Set by session.cpp LocalWorker when mirroring a local turn to Telegram.
extern StreamProgress* g_stream_progress;

// Set at startup from Config::fAllowDestructiveTools or -y/--yes.
// Grants destructive-tool permission without prompting whenever
// stdin isn't usable for a y/a/n dialog.
extern bool g_allow_destructive_tools;

// Ludicrous mode: session-scoped toggle that auto-approves all
// destructive tool calls and skips permission prompts entirely.
extern std::atomic<bool> g_ludicrous_mode;

// Extended-thinking budget in tokens for the current session. 0 disables
// extended thinking; > 0 adds `thinking: {type: "enabled", budget_tokens}`
// to the request so the model reasons before answering. Set by the /think
// slash command and the `thinking_budget` config key.
extern std::atomic<int> g_thinking_budget;

// Pointer to the currently active EscInterruptGuard, exposed so
// TerminalSink::AskPermission can pause/resume it around SelectOption
// calls. Null when no turn is in flight.
extern EscInterruptGuardHandle* g_active_esc_guard;

// Response-header cache populated by the SSE client. Keys are the
// lowercased `anthropic-*` header names. Consumed by /usage.
extern std::map<std::string, std::string> g_last_rate_headers;

// Session-scoped allowlist of tool names the user has explicitly
// approved with "(a)lways". Exposed so the bridge can clear it on
// /remote-control start (to prevent carry-over between sessions).
std::unordered_set<std::string>& AlwaysAllowed();

// Install a SIGINT handler that sets g_interrupted. RAII-scoped so
// the previous handler is restored on destruction.
class InterruptGuard {
public:
	InterruptGuard();
	~InterruptGuard();
	InterruptGuard(const InterruptGuard&) = delete;
	InterruptGuard& operator=(const InterruptGuard&) = delete;
private:
	struct sigaction fPrev {};
};

// Single streamed POST to /v1/messages. `include_tools` decides
// whether the tools array is sent — set false for sub-agents that
// must not call tools. `auth` is taken by value so the 401-refresh
// path can swap in a renewed token and retry without touching the
// caller's copy. `sink_in` is the OutputSink to use; if null a
// fresh TerminalSink is created for this call.
SendResult SendConversation(config::Auth auth, const std::string& model,
                            int max_tokens, const json& messages,
                            const std::string& custom_system, bool include_tools,
                            OutputSink* sink_in = nullptr);

// Multi-round tool-use loop: SendConversation → dispatch tool_use
// blocks → append tool_results → repeat until stop_reason leaves
// "tool_use". Mutates `messages` in place so the caller can persist
// the full conversation.
// When `sink_in` is non-null, uses that sink instead of creating a
// fresh TerminalSink. Used by the Telegram bridge to pass TelegramSink.
SendResult SendWithTools(const config::Auth& auth, const std::string& model,
                         int max_tokens, json& messages,
                         const std::string& custom_system,
                         OutputSink* sink_in = nullptr);

// Drain and return the list of file paths whose claude:summary BFS
// attribute was written by WriteAttr tool calls during the most recent
// SendWithTools invocation on this thread. The internal list is cleared
// on each call so subsequent calls return only new additions.
// No-op (returns empty vector) on non-Haiku platforms.
std::vector<std::string> DrainWrittenSummaryPaths();

// Initialise libcurl's global state once, before any worker thread can
// call curl_easy_init(). libcurl's implicit global init (triggered by the
// first curl_easy_init) is NOT thread-safe; the GUI runs SendWithTools on
// a worker thread, so this must be called once from main() at startup.
// Safe to call multiple times; only the first call has effect.
void GlobalInit();

} // namespace api

// Shared cancellation flag — set by the SIGINT handler and the Esc
// interrupt thread, read by the curl progress callback and by the
// tool runner in tools.cpp. Must be at global scope (external
// linkage) for that cross-TU access.
extern volatile sig_atomic_t g_interrupted;

// Set by the Ctrl+X branch of EscInterruptGuard to distinguish a
// cancel-and-retype interrupt from a plain ESC/Ctrl+C cancel.
// Cleared by InteractiveLoop after it restores the input buffer.
// Same external-linkage requirement as g_interrupted.
extern volatile sig_atomic_t g_cancel_retype;

#endif
