#ifndef HAIKU_CLAUDE_CLI_COMMANDS_H
#define HAIKU_CLAUDE_CLI_COMMANDS_H

#include <optional>
#include <string>
#include <vector>

// User-defined slash commands loaded from .claude/commands/*.md
// (project-local) and a user-global directory. Each .md file is a
// prompt template: its file contents become the user message sent to
// Claude, with `{{args}}` substituted by whatever text followed the
// command name in the REPL input.
//
// Project commands override user commands when both define the same
// name. Command name is the filename without the .md extension.
namespace commands {

void load(const std::string& user_dir);

std::vector<std::string> names();

// Return the substituted body when `name` resolves to a known
// command, or std::nullopt otherwise. An empty file body is also
// treated as "not found" so the dispatcher falls through to the
// built-in unknown-command error path.
std::optional<std::string> expand(const std::string& name,
                                  const std::string& args);

} // namespace commands

#endif
