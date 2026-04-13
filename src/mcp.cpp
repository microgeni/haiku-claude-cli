#include "mcp.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace mcp {

namespace {

constexpr const char* kProtocolVersion = "2024-11-05";
constexpr const char* kNamePrefix      = "mcp__";

struct McpTool {
    std::string server_name;
    std::string tool_name;
    std::string namespaced; // "mcp__<server>__<tool>" for Claude
    json        input_schema;
    std::string description;
};

struct Client {
    std::string           name;
    pid_t                 pid       = -1;
    int                   in_fd     = -1;
    int                   out_fd    = -1;
    int                   next_id   = 1;
    std::string           read_buf;
    std::vector<McpTool>  tools;
    bool                  alive     = false;
};

std::vector<Client> g_clients;

bool write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string read_line(Client& c) {
    for (;;) {
        const auto nl = c.read_buf.find('\n');
        if (nl != std::string::npos) {
            std::string line = c.read_buf.substr(0, nl);
            c.read_buf.erase(0, nl + 1);
            return line;
        }
        char buf[4096];
        const ssize_t n = read(c.out_fd, buf, sizeof(buf));
        if (n <= 0) {
            c.alive = false;
            return {};
        }
        c.read_buf.append(buf, static_cast<size_t>(n));
    }
}

bool send_json(Client& c, const json& msg) {
    const std::string line = msg.dump() + "\n";
    if (!write_all(c.in_fd, line)) {
        c.alive = false;
        return false;
    }
    return true;
}

json send_request(Client& c, const std::string& method, const json& params) {
    const int id = c.next_id++;
    const json req = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  method},
        {"params",  params.is_null() ? json::object() : params},
    };
    if (!send_json(c, req)) return {};

    // Read until we find the matching id. Drop notifications and
    // other responses we didn't ask for — a full implementation
    // would queue them, but for synchronous tool calls we don't
    // care.
    for (int guard = 0; guard < 1000; ++guard) {
        const std::string line = read_line(c);
        if (line.empty()) return {};
        json msg;
        try {
            msg = json::parse(line);
        } catch (const json::exception&) {
            continue;
        }
        if (msg.contains("id") && msg["id"].is_number() && msg["id"].get<int>() == id) {
            return msg;
        }
    }
    return {};
}

void send_notification(Client& c, const std::string& method, const json& params) {
    const json note = {
        {"jsonrpc", "2.0"},
        {"method",  method},
        {"params",  params.is_null() ? json::object() : params},
    };
    (void)send_json(c, note);
}

bool spawn_server(Client& c, const std::string& command,
                  const std::vector<std::string>& args,
                  const std::map<std::string, std::string>& env_overrides) {
    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0)  return false;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return false; }

    const pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child: wire up pipes, then exec.
        close(in_pipe[1]);
        close(out_pipe[0]);
        if (dup2(in_pipe[0],  STDIN_FILENO)  < 0) _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        close(in_pipe[0]);
        close(out_pipe[1]);
        // Leave stderr inherited so the server's error messages
        // surface in the user's terminal.

        for (const auto& kv : env_overrides) {
            setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(command.c_str(), argv.data());
        _exit(127);
    }

    // Parent.
    close(in_pipe[0]);
    close(out_pipe[1]);
    c.pid    = pid;
    c.in_fd  = in_pipe[1];
    c.out_fd = out_pipe[0];
    c.alive  = true;
    return true;
}

bool initialize_client(Client& c) {
    const json init_params = {
        {"protocolVersion", kProtocolVersion},
        {"capabilities",    { {"tools", json::object()} }},
        {"clientInfo",      {
            {"name",    "haiku-claude-cli"},
            {"version", "0.9.0"},
        }},
    };
    const json resp = send_request(c, "initialize", init_params);
    if (resp.is_null() || !resp.contains("result")) {
        std::cerr << "mcp(" << c.name << "): initialize failed\n";
        return false;
    }
    send_notification(c, "notifications/initialized", json::object());
    return true;
}

void discover_tools(Client& c) {
    const json resp = send_request(c, "tools/list", json::object());
    if (resp.is_null() || !resp.contains("result")) {
        std::cerr << "mcp(" << c.name << "): tools/list failed\n";
        return;
    }
    const auto& result = resp["result"];
    if (!result.contains("tools") || !result["tools"].is_array()) return;

    for (const auto& t : result["tools"]) {
        McpTool tool;
        tool.server_name  = c.name;
        tool.tool_name    = t.value("name", std::string{});
        if (tool.tool_name.empty()) continue;
        tool.description  = t.value("description", std::string{});
        tool.input_schema = t.contains("inputSchema") ? t["inputSchema"] : json::object();
        tool.namespaced   = std::string(kNamePrefix) + c.name + "__" + tool.tool_name;
        c.tools.push_back(std::move(tool));
    }
}

Client* find_by_tool_name(const std::string& namespaced) {
    for (auto& c : g_clients) {
        if (!c.alive) continue;
        for (const auto& t : c.tools) {
            if (t.namespaced == namespaced) return &c;
        }
    }
    return nullptr;
}

McpTool* find_tool(Client& c, const std::string& namespaced) {
    for (auto& t : c.tools) {
        if (t.namespaced == namespaced) return &t;
    }
    return nullptr;
}

bool g_atexit_registered = false;

} // namespace

void init(const json& config_mcp_servers) {
    shutdown();

    if (!config_mcp_servers.is_object()) return;

    if (!g_atexit_registered) {
        std::atexit(shutdown);
        g_atexit_registered = true;
    }

    for (auto it = config_mcp_servers.begin(); it != config_mcp_servers.end(); ++it) {
        const std::string server_name = it.key();
        const auto& entry = it.value();
        if (!entry.is_object()) continue;

        const std::string command = entry.value("command", std::string{});
        if (command.empty()) {
            std::cerr << "mcp(" << server_name << "): missing command\n";
            continue;
        }
        std::vector<std::string> args;
        if (entry.contains("args") && entry["args"].is_array()) {
            for (const auto& a : entry["args"]) {
                if (a.is_string()) args.push_back(a.get<std::string>());
            }
        }
        std::map<std::string, std::string> env_overrides;
        if (entry.contains("env") && entry["env"].is_object()) {
            for (auto eit = entry["env"].begin(); eit != entry["env"].end(); ++eit) {
                if (eit.value().is_string()) {
                    env_overrides[eit.key()] = eit.value().get<std::string>();
                }
            }
        }

        Client c;
        c.name = server_name;
        if (!spawn_server(c, command, args, env_overrides)) {
            std::cerr << "mcp(" << server_name << "): spawn failed\n";
            continue;
        }
        if (!initialize_client(c)) {
            close(c.in_fd);
            close(c.out_fd);
            waitpid(c.pid, nullptr, 0);
            continue;
        }
        discover_tools(c);
        std::cerr << "mcp(" << c.name << "): loaded " << c.tools.size() << " tool(s)\n";
        g_clients.push_back(std::move(c));
    }
}

void shutdown() {
    for (auto& c : g_clients) {
        if (c.in_fd  >= 0) close(c.in_fd);
        if (c.out_fd >= 0) close(c.out_fd);
        if (c.pid > 0) {
            int status = 0;
            // Give the child a moment to exit after its stdin closes.
            waitpid(c.pid, &status, 0);
        }
    }
    g_clients.clear();
}

json tool_definitions() {
    json out = json::array();
    for (const auto& c : g_clients) {
        if (!c.alive) continue;
        for (const auto& t : c.tools) {
            out.push_back({
                {"name",         t.namespaced},
                {"description",  t.description.empty()
                                     ? "MCP tool provided by " + c.name
                                     : t.description},
                {"input_schema", t.input_schema.is_null() ? json::object() : t.input_schema},
            });
        }
    }
    return out;
}

bool is_mcp_tool(const std::string& name) {
    return name.size() > std::strlen(kNamePrefix)
        && name.compare(0, std::strlen(kNamePrefix), kNamePrefix) == 0;
}

std::optional<tools::ToolResult> run(const std::string& name, const json& input) {
    if (!is_mcp_tool(name)) return std::nullopt;
    Client* c = find_by_tool_name(name);
    if (!c) return tools::ToolResult{"error: no MCP server owns " + name, true};
    const McpTool* tool = find_tool(*c, name);
    if (!tool) return tools::ToolResult{"error: tool not found in server " + c->name, true};

    const json params = {
        {"name",      tool->tool_name},
        {"arguments", input},
    };
    const json resp = send_request(*c, "tools/call", params);
    if (resp.is_null()) {
        return tools::ToolResult{"error: MCP server " + c->name + " returned no response", true};
    }
    if (resp.contains("error")) {
        const auto& err = resp["error"];
        return tools::ToolResult{"error: " + err.value("message", std::string{"unknown"}), true};
    }
    if (!resp.contains("result")) {
        return tools::ToolResult{"error: malformed MCP response", true};
    }

    const auto& result = resp["result"];
    // Concatenate all text blocks in the content array for display.
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& block : result["content"]) {
            if (block.value("type", "") == "text") {
                if (!text.empty()) text += "\n";
                text += block.value("text", "");
            }
        }
    }
    const bool is_error = result.value("isError", false);
    if (text.empty()) text = is_error ? "error: MCP call failed" : "(empty result)";
    return tools::ToolResult{text, is_error};
}

} // namespace mcp
