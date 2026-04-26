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

// Anthropic Messages API client and tool-use loop. SendConversation
// does one streamed POST to /v1/messages; SendWithTools wraps that
// in a loop that dispatches each tool_use block back through the
// tools:: registry, re-feeding tool_results until the model stops
// requesting tools.
namespace api {

using json = nlohmann::json;

enum class Permission { Allow, Deny };

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

// Cross-thread progress handle used by the Telegram bridge to watch a
// streaming response and push incremental edits to the chat. Written
// by the SSE parser's text_delta branch; read by the bridge's
// updater thread. Nulled out when no remote consumer is attached.
struct StreamProgress {
	std::mutex        mu;
	std::string       text;
	std::atomic<int>  version {0};
	// Non-empty while a tool is actively running. The updater thread
	// shows this string instead of the "thinking…" animation.
	std::string       tool_phase;
};

// Globals that module boundaries cross: these used to live in
// main.cpp's anonymous namespace. Extern-declared here so the
// Telegram bridge, REPL session, and permission prompt can all read
// and write them without plumbing references through every call.

// Set by the bridge so ProcessSseEvent can push token deltas.
extern StreamProgress* g_stream_progress;

// Set by a bridge context to forward tool lifecycle notices (start,
// result, denial) as separate Telegram messages. Cleared when no
// remote consumer is attached.
extern std::function<void(const std::string&)> g_tool_status_hook;

// Set by the bridge to route permission prompts through Telegram.
// Signature: (tool_name, preview_text, local_answered) → Permission.
// `local_answered` is non-null when the local TTY is racing the
// same prompt; the hook must poll it and return Deny once the local
// side wins.
extern std::function<Permission(const std::string&, const std::string&,
                                 std::atomic<bool>*)> g_telegram_permission_hook;

// Non-interactive mode is set by the Telegram bridge: there's no
// stdin to prompt on, so destructive tools are either blanket-allowed
// or blanket-denied based on config.
extern bool g_non_interactive_tools;
extern bool g_non_interactive_allow_destructive;

// Set at startup from Config::fAllowDestructiveTools or -y/--yes.
// Grants destructive-tool permission without prompting whenever
// stdin isn't usable for a y/a/n dialog.
extern bool g_allow_destructive_tools;

// Ludicrous mode: session-scoped toggle that auto-approves all
// destructive tool calls and skips permission prompts entirely.
extern std::atomic<bool> g_ludicrous_mode;

// Pauses the Telegram thinking-updater threads while a permission
// prompt is in flight so the bot doesn't spam EditMessageText and
// hit rate limits that would delay the inline-keyboard permission
// message.
extern std::atomic<bool> g_telegram_updater_paused;

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
// caller's copy.
SendResult SendConversation(config::Auth auth, const std::string& model,
                            int max_tokens, const json& messages,
                            const std::string& custom_system, bool include_tools);

// Multi-round tool-use loop: SendConversation → dispatch tool_use
// blocks → append tool_results → repeat until stop_reason leaves
// "tool_use". Mutates `messages` in place so the caller can persist
// the full conversation.
SendResult SendWithTools(const config::Auth& auth, const std::string& model,
                         int max_tokens, json& messages,
                         const std::string& custom_system);

// Drain and return the list of file paths whose claude:summary BFS
// attribute was written by WriteAttr tool calls during the most recent
// SendWithTools invocation on this thread. The internal list is cleared
// on each call so subsequent calls return only new additions.
// No-op (returns empty vector) on non-Haiku platforms.
std::vector<std::string> DrainWrittenSummaryPaths();

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
