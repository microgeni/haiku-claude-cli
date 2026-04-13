#ifndef HAIKU_CLAUDE_CLI_TOOLS_H
#define HAIKU_CLAUDE_CLI_TOOLS_H

#include <string>

#include <nlohmann/json.hpp>

// Tool implementations exposed to Claude via the Messages API `tools`
// parameter. Each tool has a name, JSON schema for its input, and a
// handler that receives parsed input and returns a ToolResult.
namespace tools {

using json = nlohmann::json;

struct ToolResult {
    std::string content;
    bool        is_error = false;
};

// JSON array of tool definitions to send as the request's `tools`
// parameter. Safe to call any number of times.
json definitions();

// Dispatch a tool_use block by name with its parsed input. Unknown
// tool names return an is_error result.
ToolResult run(const std::string& name, const json& input);

// True when a tool must prompt the user for permission before its
// first execution in a session. Currently Bash and Write.
bool requires_permission(const std::string& name);

// Optional multi-line description of what the tool would do if the
// caller approves it. Shown between the `[tool: Name ...]` notice and
// the permission prompt. Empty for tools where the input dump is
// already self-explanatory (Bash, Read, Glob, Grep).
std::string preview(const std::string& name, const json& input);

} // namespace tools

#endif
