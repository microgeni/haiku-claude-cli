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

// JSON array of tool Definitions to send as the request's `tools`
// parameter. Safe to call any number of times.
json Definitions();

// Dispatch a tool_use block by name with its parsed input. Unknown
// tool Names return an is_error result.
ToolResult Run(const std::string& name, const json& input);

// True when a tool must prompt the user for permission before its
// first execution in a session. Currently Bash, Write, and Edit.
//
// `input` is optional and lets a tool decide per-call: the Skill tool
// only prompts when the named skill's body contains a !`cmd` marker,
// since expanding it would execute shell that the user never typed.
// Callers that pass nothing get the conservative name-only answer.
bool RequiresPermission(const std::string& name,
                        const json& input = json::object());

// True when a tool cannot mutate the filesystem, shell, or any external
// state — safe to expose in Plan Mode. Read/Glob/Grep/WebFetch/WebSearch/
// Task/TodoRead/TodoWrite plus the Haiku read-only BFS tools (Query,
// ReadAttr). Bash, Write, Edit, WriteAttr, IndexAttr, and every MCP tool
// are treated as potentially mutating and excluded.
bool IsReadOnly(const std::string& name);

// Optional multi-line description of what the tool would do if the
// caller approves it. Shown between the `[tool: Name ...]` notice and
// the permission prompt. Empty for tools where the input dump is
// already self-explanatory (Bash, Read, Glob, Grep).
std::string Preview(const std::string& name, const json& input);

// A compact, single-line summary of a tool's most salient argument
// (the command for Bash, the path for Read/Write/Edit, the pattern for
// Grep/Glob, etc.). Newlines are collapsed to spaces. When maxLen > 0 the
// result is clamped to that many characters with a trailing ellipsis so it
// fits on one status row; pass maxLen == 0 to return the full, untruncated
// argument (used by the GUI tool log, which can show the whole command).
// Used by the CLI status line, the GUI tool log, and the Telegram bridge.
std::string ArgSummary(const std::string& name, const json& input,
                       size_t maxLen = 80);

// Structured diff for GUI display. Returns a header line followed by
// diff lines, each prefixed with '+' (addition), '-' (removal), or ' '
// (context). Lines are newline-terminated. Returns an empty string when
// the tool produces no diff (e.g. Bash) or when the diff cannot be built
// (file not found). Currently supports Edit and Write.
std::string GuiDiff(const std::string& name, const json& input);

// The 1-based line number an editor should jump to after a Write or Edit,
// computed from the same input the tool ran with. For Edit it is the line of
// the first replaced occurrence of old_string; for Write it is line 1 (top of
// the new/overwritten file). Returns 0 for tools with no meaningful position.
// Used to drive cursor placement when reopening edited files in the IDE.
int EditedLine(const std::string& name, const json& input);

} // namespace tools

#endif
