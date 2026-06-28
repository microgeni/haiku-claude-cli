#ifndef HAIKU_CLAUDE_CLI_AGENTS_H
#define HAIKU_CLAUDE_CLI_AGENTS_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Subagents — Claude Code parity.
//
// A subagent is a Markdown file with YAML frontmatter followed by a
// system prompt. When the Task tool is invoked with a `subagent_type`
// matching one of these definitions, the agent loop runs the sub-turn
// with that definition's system prompt, model, and tool allow list
// instead of the generic defaults.
//
// Frontmatter fields:
//   name         Identifier used as subagent_type (defaults to filename).
//   description  When to delegate to this subagent — surfaced to the
//                model so it can choose the right one.
//   tools        Optional comma/space-separated allow list. When set,
//                only these tools are exposed to the subagent. Empty =
//                inherit all tools. (A subagent named with read-only
//                intent should list only read-only tools.)
//   model        Optional model override (e.g. "haiku", "sonnet", or a
//                full model id). Empty = inherit the parent's model.
//   color        Optional UI hint, accepted and ignored for parity.
//
// Definitions load from the user dir (<ConfigDir>/agents) and the
// project dir (./.claude/agents). Project definitions override user
// ones of the same name.
namespace agents {

using json = nlohmann::json;

struct Agent {
	std::string name;          // subagent_type
	std::string description;   // delegation hint
	std::string prompt;        // markdown body → system prompt
	std::string tools;         // raw allow-list string ("" = all)
	std::string model;         // model override ("" = inherit)
	std::string color;         // UI hint (unused by the CLI)
};

// (Re)scan the user and project agent directories. Replaces the
// in-memory registry on each call.
void Load(const std::string& userDir, const std::string& projectDir);

// All loaded subagents, sorted by name.
const std::vector<Agent>& All();

// Sorted names (for listings / tab completion).
std::vector<std::string> Names();

// Look up by name; nullptr when absent.
const Agent* Find(const std::string& name);

// Resolve a frontmatter model alias ("haiku"/"sonnet"/"opus" or a full
// id) against the parent model. Returns `parentModel` unchanged when
// the agent has no override.
std::string ResolveModel(const Agent& a, const std::string& parentModel);

// Parse the `tools` allow list into a set of tool names. Empty result
// means "inherit all" (no filtering).
std::vector<std::string> ToolAllowList(const Agent& a);

// A block describing available subagents for the system prompt so the
// model can pass the right subagent_type to the Task tool. Empty when
// no subagents are loaded.
std::string SystemBlock();

} // namespace agents

#endif
