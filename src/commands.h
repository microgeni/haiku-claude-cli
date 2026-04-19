#ifndef HAIKU_CLAUDE_CLI_COMMANDS_H
#define HAIKU_CLAUDE_CLI_COMMANDS_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"

// Slash-command dispatch for the REPL: built-ins like /help, /model,
// /compact, /memory, /open, /usage, plus user-defined commands
// loaded from `.claude/commands/*.md` (project-local) and a user-
// global directory. Each .md file is a prompt template: its file
// contents become the user message sent to Claude, with `{{args}}`
// substituted by whatever text followed the command name.
//
// Project commands override user commands when both define the same
// name. Command name is the filename without the .md extension.
namespace commands {

void Load(const std::string& user_dir);

std::vector<std::string> Names();

// Return the substituted body when `name` resolves to a known
// command, or std::nullopt otherwise. An empty file body is also
// treated as "not found" so the dispatcher falls through to the
// built-in unknown-command error path.
std::optional<std::string> Expand(const std::string& name,
								  const std::string& args);

// Outcome of Dispatch().
//   Continue    — command handled; REPL loop should read the next line.
//   Quit        — command requested the REPL to exit.
//   Passthrough — command expanded to a user message; REPL should
//                 send `passthrough_out` as a normal prompt.
enum class SlashAction { Continue, Quit, Passthrough };

// Mutable context passed into Dispatch so slash handlers can read
// session totals and update state that persists across turns.
struct LoopCtx {
	const config::Auth&        auth;
	int                        max_tokens;
	const std::string&         custom_system;
	const nlohmann::json&      prices;
	std::string&               model;
	int&                       turn_count;
	int&                       session_input;
	int&                       session_output;
	nlohmann::json&            messages;
	std::vector<std::string>&  session_urls;
	bool&                      notify_enabled;
	double&                    notify_min_duration;
	// Invoked by slash commands that mutate state visible in the
	// fixed-bottom status frame (model name, turn counter, totals).
	// Default is a no-op so non-REPL callers don't need to wire it.
	std::function<void()>      redraw_status;
	// When true, /compact was triggered automatically by the auto-
	// compact threshold and must skip the confirmation prompt.
	bool                       auto_compact = false;
	// Session name forwarded to SaveHistory after auto-compact so
	// the right history file is updated. Empty = default session.
	std::string                resume_name  = {};
	// Thresholds forwarded from Config so external callers can
	// trigger auto-compact after each turn.
	double                     compact_auto_threshold  = 0.0;
	int                        compact_window_override = 0;
};

// Dispatch a slash command line. Sets `passthrough_out` when the
// return value is Passthrough (the caller should send it as a user
// message). `line` must start with `/`.
SlashAction Dispatch(const std::string& line, LoopCtx& ctx,
					 std::string& passthrough_out);

} // namespace commands

#endif
