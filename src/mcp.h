#ifndef HAIKU_CLAUDE_CLI_MCP_H
#define HAIKU_CLAUDE_CLI_MCP_H

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools.h"

// Minimal MCP (Model Context Protocol) stdio-transport client.
// Spawns configured MCP servers as subprocesses, speaks newline-
// delimited JSON-RPC 2.0 over their stdin/stdout, runs the
// `initialize` handshake, discovers their tools via `tools/list`,
// and routes tool calls via `tools/call`.
//
// Server-provided tools are namespaced as `mcp__<server>__<tool>`
// when advertised to Claude, so they can't collide with built-ins.
//
// MVP scope: stdio only, synchronous request/response, process
// teardown on cli Shutdown. No resources/, prompts/, or HTTP/SSE
// transport yet.
namespace mcp {

using json = nlohmann::json;

// Parse the "mcp_servers" key from config.json, spawn each server,
// run the initialize handshake, and query tools/list. Servers that
// fail to spawn or initialize are logged to stderr and skipped.
// Safe to call more than once (clears state).
void Init(const json& config_mcp_servers);

// Tear down every live MCP client: close pipes, wait for the child
// process. Called from an atexit hook installed by init().
void Shutdown();

// Tool Definitions to merge into the Messages API `tools` array,
// one entry per namespaced MCP tool.
json ToolDefinitions();

// True when `name` is an MCP tool (prefix `mcp__`).
bool IsMcpTool(const std::string& name);

// Introspection for the GUI diagnostics panel: one entry per initialized
// MCP server with its name, live tool count, and whether the subprocess is
// currently alive. Empty when no servers are configured.
struct ServerInfo {
	std::string name;
	int         tool_count = 0;
	bool        alive      = false;
};
std::vector<ServerInfo> ActiveServers();

// Dispatch `name` (mcp__server__tool) to the owning client. Returns
// std::nullopt if the name isn't an MCP tool or the server isn't
// live; otherwise returns the tool's ToolResult.
std::optional<tools::ToolResult> Run(const std::string& name, const json& input);

} // namespace mcp

#endif
