#ifndef HAIKU_CLAUDE_CLI_CONFIG_H
#define HAIKU_CLAUDE_CLI_CONFIG_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "history_util.h"  // config::CapHistoryMessages (pure).

// Configuration, credentials, system-prompt composition, history
// persistence, and logging. Everything that turns on-disk state
// (config.json, CLAUDE.md, credentials.json, history.json, log files,
// BFS claude:summary attributes) into the values the rest of the
// program consumes.
namespace config {

using json = nlohmann::json;

// Build-wide constants. Shared so modules that emit version strings,
// set defaults, or attach HTTP headers can reach them without taking
// a dependency on main.cpp.
extern const char* const kVersion;
extern const char* const kDefaultModel;
extern const char* const kApiVersion;
extern const char* const kOAuthBeta;
extern const int         kMaxTokens;

struct Config {
	std::string model;
	int         max_tokens = 0;
	std::string system;  // flawfinder: ignore (member name, not the system() call)
	bool        show_usage              = false;
	bool        logging_enabled         = false;
	bool        fAllowDestructiveTools = false;
	bool        notify_enabled          = true;
	double      notify_min_duration_sec = 60.0;
	// Auto-compact fires `/compact` when a turn's input token count
	// crosses `compact_auto_threshold` * context_window. 0.0 disables.
	// context_window = 0 auto-detects from the model name.
	double      compact_auto_threshold  = 0.8;
	int         compact_context_window  = 0;
	// Maximum number of messages retained in a saved history file. The
	// oldest are dropped so history.json can't grow without bound on a
	// long single session. 200 ≈ 100 turns. Applied on both save and
	// load. Configurable via the "history_max_messages" config key.
	int         history_max_messages    = 200;
	// Extended-thinking token budget. 0 disables; > 0 enables a visible
	// reasoning block before each reply (see api::g_thinking_budget).
	int         thinking_budget         = 0;
	json        prices;
	json        hooks;
	json        mcp_servers;
	json        telegram;
	// "workflow" object — the Markov workflow-memory settings
	// (enabled / surprise_threshold / order / nudges). Passed
	// verbatim to workflow::Configure(). Disabled when absent.
	json        workflow;
};

Config Load();

enum class AuthKind { None, OAuth, ApiKey };

struct Auth {
	AuthKind    kind = AuthKind::None;
	std::string credential;
};

// Resolve authentication in priority order: OAuth tokens (auto-
// refreshed when expired), then ANTHROPIC_API_KEY env var, then a
// stored API key written via SaveApiKey() (GUI login).
Auth ResolveAuth();

// Persist / read / clear an Anthropic API key in a 0600 file at
// <ConfigDir>/api_key. Used by the GUI auth dialog so a key entered
// once is remembered across launches (the env var still wins). Returns
// true on success; LoadApiKey returns "" when none is stored.
bool        SaveApiKey(const std::string& key);
std::string LoadApiKey();
bool        ClearApiKey();

// Compose the effective system prompt: user CLAUDE.md, project
// CLAUDE.md, BFS summary snapshot (Haiku only), then `flag_system`.
// Called per-turn so edits to the CLAUDE.md files take effect
// immediately.
// `working_dir` overrides the working-directory line appended to the
// prompt; pass an empty string to fall back to getcwd().
std::string ComposeSystem(const std::string& flag_system,
                           const std::string& working_dir = {});

// The system prompt split into two cache tiers, ordered stable-first.
//
// Anthropic's prompt cache matches on an exact prefix: everything up
// to a cache_control breakpoint must be byte-identical to the previous
// request or the cache misses. Splitting lets the large, rarely-changing
// part earn a long-lived cache entry even when the tail changes.
//
//   stable   — user/project CLAUDE.md, the BFS tool guidance + summary
//              snapshot, behaviour guidance, and the skills/agents
//              index. Byte-identical across turns of a session and
//              (mostly) across sessions in the same project, so it
//              survives as a warm prefix. The CLAUDE.md files are
//              re-read every turn and belong here despite being
//              user-editable: they change rarely, so an edit costs one
//              cache miss and then re-warms.
//   volatile — the plan-mode directive (toggled mid-session by /plan
//              and /execute), the -s/--system flag text, and the
//              working-directory line.
//
// Concatenating stable + "\n\n" + volatile reproduces exactly what
// ComposeSystem() returns, so callers that don't care about caching
// can keep using ComposeSystem().
struct SystemTiers {
	std::string stable;
	std::string volatileTier;
};

// Build the tiered system prompt. Same inputs and same joined output
// as ComposeSystem(); only the split point is additional information.
SystemTiers ComposeSystemTiers(const std::string& flag_system,
                                const std::string& working_dir = {});

// The stable tier on its own, for the API client's cache-breakpoint
// placement. api.cpp receives an already-composed system string, so it
// verifies this really is a prefix of that string before splitting it
// into two cacheable blocks — a mismatch (sub-agent prompt, custom
// caller) just falls back to a single block.
std::string StableSystemPrefix();

// Replace invalid UTF-8 bytes (including truncated sequences) with
// U+FFFD so nlohmann::json::dump() never throws type_error.316.
std::string SanitizeUtf8(const std::string& s);

// Load/save the rolling history file. Optional `name` names the
// session (history-<name>.json); empty means the default file.
std::optional<json> LoadHistory(const std::string& name = "");
bool SaveHistory(const json& messages, const std::string& model,
                 const std::string& name = "");

// Set the cap (in messages) applied when saving and loading history.
// Call once at startup from the loaded Config. Values < 1 are ignored
// so a mis-set config key can't silently disable persistence.
void SetHistoryMessageCap(int cap);

// CapHistoryMessages (pure history-array cap) is declared in
// history_util.h, included at the top of this header.

// Logging. InitLogging is a no-op when `enabled` is false; LogLine
// silently drops when the log file isn't open.
void InitLogging(bool enabled);
void LogLine(const std::string& msg);

// Safely single-quote a string for a /bin/sh command line. Any
// embedded `'` is split out as `'\''`.
std::string ShellSingleQuote(const std::string& s);

// Auto-seed a `claude:summary` BFS attribute for a file that has
// none yet. Called after a successful Read tool call so the next
// session can skip the full read. No-op on non-Haiku platforms.
void AutoWriteSummaryIfMissing(const std::string& path,
                                const std::string& content);

// Force a full re-scan of the project's claude:summary attributes,
// replacing the in-process snapshot. Call after /compact so
// summaries written during the session appear in the next turn's
// system prompt. No-op on non-Haiku platforms.
void ReloadBfsSummaries();

// Update the in-process snapshot for a specific set of paths whose
// claude:summary attribute was written or changed during this session.
// O(changed) — avoids a full filesystem walk. No-op on non-Haiku.
void RefreshSummarySnapshot(const std::vector<std::string>& paths);

} // namespace config

#endif
