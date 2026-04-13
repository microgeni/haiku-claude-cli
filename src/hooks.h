#ifndef HAIKU_CLAUDE_CLI_HOOKS_H
#define HAIKU_CLAUDE_CLI_HOOKS_H

#include <string>

#include <nlohmann/json.hpp>

// Shell-command hooks wired at lifecycle points. Users register hooks
// in config.json under a "hooks" key:
//
//   {
//     "hooks": {
//       "PreToolUse":       [{ "matcher": "Bash", "command": "check.sh" }],
//       "UserPromptSubmit": [{                     "command": "log.sh"  }]
//     }
//   }
//
// Each hook is a shell command line run via `sh -c`. Event payload
// JSON is written to the hook's stdin. Stderr is forwarded verbatim
// so hooks can talk back to the user. A non-zero exit from any
// matching hook means `Block` — the caller then aborts whatever was
// about to happen (drop the turn / deny the tool call).
namespace hooks {

using json = nlohmann::json;

enum class Event {
    SessionStart,
    UserPromptSubmit,
    PreToolUse,
    PostToolUse,
    Stop,
};

enum class Outcome {
    Proceed,
    Block,
};

void load(const json& config_hooks);

// Fire every matching hook for this event with `payload` enriched by
// an "event" field (and "tool_name" when non-empty). `tool_name` is
// used as the matcher filter for PreToolUse / PostToolUse entries.
Outcome fire(Event event, const json& payload, const std::string& tool_name = {});

} // namespace hooks

#endif
