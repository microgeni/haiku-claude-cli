#ifndef HAIKU_CLAUDE_CLI_CONFIG_H
#define HAIKU_CLAUDE_CLI_CONFIG_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

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
	std::string system;
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
	json        prices;
	json        hooks;
	json        mcp_servers;
	json        telegram;
};

Config Load();

enum class AuthKind { None, OAuth, ApiKey };

struct Auth {
	AuthKind    kind = AuthKind::None;
	std::string credential;
};

// Resolve authentication in priority order: OAuth tokens (auto-
// refreshed when expired), then ANTHROPIC_API_KEY env var.
Auth ResolveAuth();

// Compose the effective system prompt: user CLAUDE.md, project
// CLAUDE.md, BFS summary snapshot (Haiku only), then `flag_system`.
// Called per-turn so edits to the CLAUDE.md files take effect
// immediately.
std::string ComposeSystem(const std::string& flag_system);

// Replace invalid UTF-8 bytes (including truncated sequences) with
// U+FFFD so nlohmann::json::dump() never throws type_error.316.
std::string SanitizeUtf8(const std::string& s);

// Load/save the rolling history file. Optional `name` names the
// session (history-<name>.json); empty means the default file.
std::optional<json> LoadHistory(const std::string& name = "");
bool SaveHistory(const json& messages, const std::string& model,
                 const std::string& name = "");

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

} // namespace config

#endif
