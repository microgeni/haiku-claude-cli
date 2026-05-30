#ifndef CCH_TOOLS_H
#define CCH_TOOLS_H

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "cch/CommandTarget.h"
#include "cch/StreamSink.h"

namespace cch {

// One tool invocation as parsed from a model tool_use block.
struct ToolCall {
    std::string id;     // tool_use id, echoed back in the tool_result
    std::string name;   // "bash", "search", "ssh_exec", ...
    std::string input;  // raw JSON arguments
};

// The result handed back to the model as a tool_result block.
struct ToolResult {
    std::string toolUseId;
    std::string content;    // accumulated stdout/stderr (buffered, see note)
    bool        isError{false};

    // Convenience constructors so tool handlers can return
    // {content, isError} without spelling out field names.
    ToolResult() = default;
    ToolResult(std::string c, bool err)
        : content(std::move(c)), isError(err) {}
};

// A handler runs a ToolCall and returns its result. It is given a StreamSink so
// it can ALSO stream live output to the UI while it runs -- the two sinks
// pattern: stream to display, buffer for the API round-trip. The buffered text
// becomes ToolResult.content; the live stream is cosmetic.
using ToolHandler =
    std::function<ToolResult(const ToolCall& call, const StreamSink& liveSink)>;

// Registry the agent loop dispatches through. The GUI/CLI register the command
// tools by binding them to CommandTargets (local, ssh, routeros) and a native
// FileSearch where available.
class ToolRegistry {
public:
    void Register(const std::string& name, ToolHandler handler);

    // Returns true and fills `out` if a handler exists; false otherwise.
    bool Dispatch(const ToolCall& call, const StreamSink& liveSink,
                  ToolResult& out) const;

    // The tool schema (JSON) advertised to the model in the API request.
    std::string SchemaJson() const;

private:
    std::map<std::string, ToolHandler> fHandlers;
};

// Optional native file search (BFS BQuery on Haiku, injected by GUI). When set,
// LocalTarget routes file-search tool calls here instead of exec("find").
struct FileSearch {
    // Returns matching paths for an attribute/name query, or empty on miss.
    std::function<std::vector<std::string>(const std::string& query)> query;
};

// Populate a registry with all built-in tool handlers (Read, Write, Edit,
// Glob, Grep, Bash, WebFetch, WebSearch, TodoWrite, TodoRead, and the
// Haiku BFS tools on __HAIKU__ builds). Called once at startup.
void RegisterBuiltins(ToolRegistry& reg);

} // namespace cch

#endif
