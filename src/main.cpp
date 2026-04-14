#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "commands.h"
#include "hooks.h"
#include "mcp.h"
#include "oauth.h"
#include "repl.h"
#include "telegram.h"
#include "tools.h"
#include "tui.h"

using json = nlohmann::json;

namespace {

constexpr const char* kVersion      = "1.0.1";
constexpr const char* kDefaultModel = "claude-sonnet-4-6";
constexpr const char* kApiUrl       = "https://api.anthropic.com/v1/messages";
constexpr const char* kApiVersion   = "2023-06-01";
constexpr const char* kOAuthBeta    = "oauth-2025-04-20";
constexpr const char* kOAuthSystem  = "You are Claude Code, Anthropic's official CLI for Claude.";
constexpr int         kMaxTokens    = 1024;

std::string config_dir() {
#ifdef __HAIKU__
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/boot/home") + "/config/settings/claude-cli";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/claude-cli";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/claude-cli";
#endif
}

std::string history_path() {
    return config_dir() + "/history.json";
}

std::string repl_history_path() {
    return config_dir() + "/repl_history";
}

std::string config_path() {
    return config_dir() + "/config.json";
}

std::string log_dir() {
    return config_dir() + "/logs";
}

std::ofstream g_log;

void init_logging(bool enabled) {
    if (!enabled) return;
    const std::string dir = log_dir();
    {
        // Tiny inline mkdir_p so logging has no forward-decl hassle.
        std::string accum;
        for (size_t i = 0; i < dir.size(); ++i) {
            accum += dir[i];
            const bool boundary = (dir[i] == '/') || (i + 1 == dir.size());
            if (!boundary) continue;
            if (accum.empty() || accum == "/") continue;
            if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return;
        }
    }

    const std::time_t t = std::time(nullptr);
    std::tm            tm {};
    localtime_r(&t, &tm);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);

    g_log.open(dir + "/claude-" + date + ".log", std::ios::app);
}

void log_line(const std::string& msg) {
    if (!g_log.is_open()) return;
    const std::time_t t = std::time(nullptr);
    std::tm            tm {};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
    g_log << "[" << ts << "] " << msg << "\n";
    g_log.flush();
}

std::string user_memory_path() {
    return config_dir() + "/CLAUDE.md";
}

std::string project_memory_path() {
    return "CLAUDE.md";
}

std::string load_optional_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Compose the effective system prompt from (in order):
//   1. User-level memory at ~/config/settings/claude-cli/CLAUDE.md
//   2. Project memory at <cwd>/CLAUDE.md
//   3. The --system flag value (or config's "system")
// The required Claude Code preamble for OAuth is prepended inside
// send_conversation, so we don't repeat it here. Called per-turn so
// edits to the CLAUDE.md files take effect immediately.
std::string compose_system(const std::string& flag_system) {
    std::string out;
    auto append = [&](const std::string& chunk) {
        if (chunk.empty()) return;
        if (!out.empty()) out += "\n\n";
        out += chunk;
    };
    append(load_optional_file(user_memory_path()));
    append(load_optional_file(project_memory_path()));
    append(flag_system);
    return out;
}

bool mkdir_p(const std::string& path) {
    std::string accum;
    for (size_t i = 0; i < path.size(); ++i) {
        accum += path[i];
        const bool boundary = (path[i] == '/') || (i + 1 == path.size());
        if (!boundary) continue;
        if (accum.empty() || accum == "/") continue;
        if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

std::optional<json> load_history() {
    std::ifstream f(history_path());
    if (!f.is_open()) return std::nullopt;
    try {
        json j = json::parse(f);
        if (j.contains("messages") && j["messages"].is_array()) {
            return j["messages"];
        }
    } catch (...) {
        // fall through
    }
    return std::nullopt;
}

bool save_history(const json& messages, const std::string& model) {
    const std::string path = history_path();
    const auto        slash = path.rfind('/');
    if (slash == std::string::npos) return false;
    if (!mkdir_p(path.substr(0, slash))) return false;

    const json j = {
        {"messages", messages},
        {"model",    model},
        {"saved_at", static_cast<long>(std::time(nullptr))},
    };
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << j.dump(2) << "\n";
    chmod(path.c_str(), 0600);
    return true;
}

// Cross-thread progress handle used by the Telegram bridge to watch a
// streaming response and push incremental edits to the chat. Written
// by process_sse_event's text_delta branch; read by the bridge's
// updater thread. Nulled out when no remote consumer is attached.
struct StreamProgress {
    std::mutex        mu;
    std::string       text;
    std::atomic<int>  version {0};
};
StreamProgress* g_stream_progress = nullptr;

std::map<std::string, std::string> g_last_rate_headers;

size_t header_callback(char* buffer, size_t size, size_t nitems, void* /*userp*/) {
    const size_t total = size * nitems;
    std::string  line(buffer, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) return total;

    std::string name = line.substr(0, colon);
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name.compare(0, 10, "anthropic-") != 0) return total;

    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(0, 1);
    }
    g_last_rate_headers[name] = value;
    return total;
}

volatile sig_atomic_t g_interrupted = 0;

extern "C" void handle_sigint(int) {
    g_interrupted = 1;
}

struct InterruptGuard {
    InterruptGuard() {
        g_interrupted = 0;
        struct sigaction sa {};
        sa.sa_handler = handle_sigint;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, &prev_);
    }
    ~InterruptGuard() {
        sigaction(SIGINT, &prev_, nullptr);
    }
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
  private:
    struct sigaction prev_ {};
};

int xfer_callback(void* /*clientp*/,
                  curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    return g_interrupted ? 1 : 0;
}

struct Config {
    std::string model;
    int         max_tokens = kMaxTokens;
    std::string system;
    bool        show_usage      = false;
    bool        logging_enabled = false;
    json        prices;
    json        hooks;
    json        mcp_servers;
    json        telegram;
};

Config load_config() {
    Config cfg;
    cfg.model = kDefaultModel;

    std::ifstream f(config_path());
    if (!f.is_open()) return cfg;

    try {
        const json j = json::parse(f);
        if (j.contains("model"))      cfg.model      = j["model"].get<std::string>();
        if (j.contains("max_tokens")) cfg.max_tokens = j["max_tokens"].get<int>();
        if (j.contains("system"))     cfg.system     = j["system"].get<std::string>();
        if (j.contains("show_usage")) cfg.show_usage = j["show_usage"].get<bool>();
        if (j.contains("prices"))       cfg.prices      = j["prices"];
        if (j.contains("hooks"))        cfg.hooks       = j["hooks"];
        if (j.contains("mcp_servers"))  cfg.mcp_servers = j["mcp_servers"];
        if (j.contains("telegram"))     cfg.telegram    = j["telegram"];
        if (j.contains("logging") && j["logging"].is_object()) {
            cfg.logging_enabled = j["logging"].value("enabled", false);
        }
    } catch (const json::exception& e) {
        std::cerr << "warning: failed to parse " << config_path() << ": " << e.what() << "\n";
    }
    return cfg;
}

enum class AuthKind { None, OAuth, ApiKey };

struct Auth {
    AuthKind    kind = AuthKind::None;
    std::string credential;
};

struct StreamState {
    std::string          sse_buffer;
    std::string          raw_buffer;
    std::string          text;
    int                  input_tokens        = 0;
    int                  output_tokens       = 0;
    bool                 saw_text            = false;
    bool                 stream_error        = false;
    std::string          stream_error_message;
    tui::Spinner*        spinner             = nullptr;
    tui::MarkdownRenderer renderer;

    // Structured content accumulation for tool-use support.
    std::vector<json>    content_blocks;           // finalized text + tool_use blocks
    std::string          current_type;             // "text" / "tool_use" while streaming a block
    std::string          current_text;
    std::string          current_tool_id;
    std::string          current_tool_name;
    std::string          current_tool_input_raw;   // partial JSON being accumulated
    std::string          stop_reason;              // set via message_delta
};

struct SendResult {
    int                exit_code = 0;
    std::string        assistant_text;
    int                input_tokens  = 0;
    int                output_tokens = 0;
    std::vector<json>  content_blocks;
    std::string        stop_reason;
};

void process_sse_event(const std::string& event, StreamState* state) {
    std::string data;
    std::istringstream iss(event);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) != 0) continue;
        std::string payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        if (!data.empty()) data += '\n';
        data += payload;
    }
    if (data.empty()) return;

    try {
        const json j = json::parse(data);
        const std::string type = j.value("type", "");

        if (type == "content_block_start") {
            const auto& cb = j.value("content_block", json::object());
            state->current_type = cb.value("type", std::string{});
            state->current_text.clear();
            state->current_tool_id.clear();
            state->current_tool_name.clear();
            state->current_tool_input_raw.clear();
            if (state->current_type == "tool_use") {
                state->current_tool_id   = cb.value("id",   std::string{});
                state->current_tool_name = cb.value("name", std::string{});
            }
        } else if (type == "content_block_delta") {
            const auto& delta = j.value("delta", json::object());
            const std::string dtype = delta.value("type", std::string{});
            if (dtype == "text_delta") {
                const std::string chunk = delta.value("text", "");
                state->renderer.write(chunk);
                state->current_text += chunk;
                state->text += chunk;
                state->saw_text = true;
                if (g_stream_progress) {
                    std::lock_guard<std::mutex> lk(g_stream_progress->mu);
                    g_stream_progress->text += chunk;
                    g_stream_progress->version.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (dtype == "input_json_delta") {
                state->current_tool_input_raw += delta.value("partial_json", "");
            }
        } else if (type == "content_block_stop") {
            if (state->current_type == "text") {
                state->content_blocks.push_back({
                    {"type", "text"},
                    {"text", state->current_text},
                });
            } else if (state->current_type == "tool_use") {
                json parsed_input = json::object();
                try {
                    if (!state->current_tool_input_raw.empty()) {
                        parsed_input = json::parse(state->current_tool_input_raw);
                    }
                } catch (const json::exception&) {
                    parsed_input = json::object();
                }
                state->content_blocks.push_back({
                    {"type",  "tool_use"},
                    {"id",    state->current_tool_id},
                    {"name",  state->current_tool_name},
                    {"input", parsed_input},
                });
            }
            state->current_type.clear();
        } else if (type == "message_start") {
            if (state->spinner) {
                state->spinner->stop();
                state->spinner = nullptr;
            }
            if (j.contains("message") && j["message"].contains("usage")) {
                const auto& u = j["message"]["usage"];
                state->input_tokens  = u.value("input_tokens",  0);
                state->output_tokens = u.value("output_tokens", 0);
            }
        } else if (type == "message_delta") {
            if (j.contains("delta") && j["delta"].contains("stop_reason")
                && j["delta"]["stop_reason"].is_string()) {
                state->stop_reason = j["delta"]["stop_reason"].get<std::string>();
            }
            if (j.contains("usage")) {
                const auto& u = j["usage"];
                state->output_tokens = u.value("output_tokens", state->output_tokens);
            }
        } else if (type == "error") {
            state->stream_error = true;
            if (j.contains("error") && j["error"].contains("message")) {
                state->stream_error_message = j["error"]["message"].get<std::string>();
            }
        }
    } catch (const json::exception&) {
        // Ignore partial/invalid payloads (e.g. ping events).
    }
}

size_t stream_write_callback(char* data, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    auto* state = static_cast<StreamState*>(userp);
    state->raw_buffer.append(data, total);
    state->sse_buffer.append(data, total);

    size_t pos;
    while ((pos = state->sse_buffer.find("\n\n")) != std::string::npos) {
        const std::string event = state->sse_buffer.substr(0, pos);
        state->sse_buffer.erase(0, pos + 2);
        process_sse_event(event, state);
    }
    return total;
}

void print_usage(const char* prog, const std::string& default_model, int default_max_tokens) {
    std::cerr << "Usage: " << prog << " [OPTIONS] [MESSAGE...]\n"
              << "\n"
              << "Sends a one-shot message to the Claude API and streams the reply.\n"
              << "If stdin is not a terminal (piped input), its contents are appended\n"
              << "to the message so `cat file.txt | " << prog << " \"summarize\"` works.\n"
              << "\n"
              << "Commands:\n"
              << "  login                Authenticate via Claude.ai (OAuth + PKCE).\n"
              << "  logout               Delete stored credentials.\n"
              << "\n"
              << "Options:\n"
              << "  -i, --interactive    Start a multi-turn REPL session.\n"
              << "  -m, --model MODEL    Model to use (default: " << default_model << ").\n"
              << "  -t, --max-tokens N   Max tokens in response (default: " << default_max_tokens << ").\n"
              << "  -s, --system TEXT    Custom system prompt (appended after the\n"
              << "                       required Claude Code prefix when OAuth is used).\n"
              << "  -u, --usage          After the response, print input/output token\n"
              << "                       usage to stderr.\n"
              << "  -r, --resume         Start the REPL pre-loaded with the last saved\n"
              << "                       session (implies -i).\n"
              << "      --plain          Disable ANSI color output.\n"
              << "      --color          Force ANSI color output, even when piped.\n"
              << "  -V, --version        Print version and exit.\n"
              << "  -h, --help           Show this help and exit.\n"
              << "\n"
              << "Config file: " << config_path() << "\n"
              << "  Optional JSON with keys: model, max_tokens, system, show_usage,\n"
              << "  prices. CLI flags override config values.\n"
              << "\n"
              << "Memory files (prepended to the system prompt, user before project):\n"
              << "  " << user_memory_path() << "\n"
              << "  ./CLAUDE.md (per-project, loaded from the current working directory)\n"
              << "\n"
              << "Authentication (in priority order):\n"
              << "  1. OAuth tokens from 'claude login' (uses Pro/Max quota).\n"
              << "  2. ANTHROPIC_API_KEY environment variable (billed per token).\n";
}

Auth resolve_auth() {
    if (auto stored = load_tokens(); stored) {
        if (stored->is_expired()) {
            if (auto refreshed = refresh_tokens(*stored); refreshed) {
                save_tokens(*refreshed);
                return {AuthKind::OAuth, refreshed->access_token};
            }
            std::cerr << "warning: OAuth refresh failed, falling back to API key\n";
        } else {
            return {AuthKind::OAuth, stored->access_token};
        }
    }

    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {
        return {AuthKind::ApiKey, k};
    }
    return {};
}

SendResult send_conversation(const Auth& auth, const std::string& model, int max_tokens,
                             const json& messages, const std::string& custom_system,
                             bool include_tools) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "error: curl_easy_init failed\n";
        return {1, {}, 0, 0, {}, {}};
    }

    json body = {
        {"model",      model},
        {"max_tokens", max_tokens},
        {"stream",     true},
        {"messages",   messages},
    };
    if (include_tools) {
        body["tools"] = tools::definitions();
    }

    // When OAuth is active, Anthropic gates the request unless the
    // system field's first entry is the Claude Code preamble. Earlier
    // versions concatenated extra content into a single string which
    // worked for small flag-only system prompts, but started failing
    // once CLAUDE.md memory files were appended. Sending `system` as
    // an array with the preamble as element 0 and any extra content
    // as element 1 passes the check.
    if (auth.kind == AuthKind::OAuth) {
        json system_array = json::array();
        system_array.push_back({{"type", "text"}, {"text", kOAuthSystem}});
        if (!custom_system.empty()) {
            system_array.push_back({{"type", "text"}, {"text", custom_system}});
        }
        body["system"] = system_array;
    } else if (!custom_system.empty()) {
        body["system"] = custom_system;
    }
    const std::string body_str = body.dump();

    curl_slist* headers = nullptr;
    if (auth.kind == AuthKind::OAuth) {
        headers = curl_slist_append(headers, ("authorization: Bearer " + auth.credential).c_str());
        headers = curl_slist_append(headers, (std::string("anthropic-beta: ") + kOAuthBeta).c_str());
    } else {
        headers = curl_slist_append(headers, ("x-api-key: " + auth.credential).c_str());
    }
    headers = curl_slist_append(headers, (std::string("anthropic-version: ") + kApiVersion).c_str());
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "accept: text/event-stream");

    StreamState state;
    tui::Spinner spinner("thinking");
    state.spinner = &spinner;
    state.renderer.set_spinner(&spinner);

    curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    const std::string ua = std::string("haiku-claude-cli/") + kVersion;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);

    g_interrupted = 0;
    const CURLcode res = curl_easy_perform(curl);
    spinner.stop();
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (g_interrupted) {
        state.renderer.flush();
        std::cout << "\n" << tui::meta("[interrupted]") << "\n";
        return {1, state.text, state.input_tokens, state.output_tokens,
                state.content_blocks, state.stop_reason};
    }

    if (res != CURLE_OK) {
        std::cerr << "\nerror: request failed: " << curl_easy_strerror(res) << "\n";
        return {1, {}, 0, 0, {}, {}};
    }

    if (http_status < 200 || http_status >= 300) {
        // Parse Anthropic's error envelope for a user-friendly message,
        // then map the HTTP code to a plain-language explanation.
        std::string api_msg;
        try {
            const json err = json::parse(state.raw_buffer);
            if (err.contains("error") && err["error"].is_object()
                && err["error"].contains("message")
                && err["error"]["message"].is_string()) {
                api_msg = err["error"]["message"].get<std::string>();
            }
        } catch (const json::exception&) {}

        std::cerr << "\n" << tui::error_label() << " ";
        switch (http_status) {
            case 401:
                std::cerr << "unauthorized (HTTP 401) — your OAuth token may be "
                             "expired. Run `claude logout` and then `claude login`.";
                break;
            case 403:
                std::cerr << "forbidden (HTTP 403) — this client or account is not "
                             "permitted to use the endpoint.";
                break;
            case 429:
                if (api_msg == "Error") {
                    std::cerr << "gated (HTTP 429) — Anthropic's OAuth-client check "
                                 "rejected the request shape; see project notes.";
                } else {
                    std::cerr << "rate limited (HTTP 429)";
                    if (!api_msg.empty()) std::cerr << ": " << api_msg;
                }
                break;
            case 500: case 502: case 503: case 504:
                std::cerr << "server error (HTTP " << http_status << ")";
                if (!api_msg.empty()) std::cerr << ": " << api_msg;
                break;
            default:
                std::cerr << "HTTP " << http_status;
                if (!api_msg.empty()) std::cerr << ": " << api_msg;
                break;
        }
        std::cerr << "\n";
        log_line("error http=" + std::to_string(http_status)
                 + (api_msg.empty() ? "" : " msg=" + api_msg));
        return {1, {}, 0, 0, {}, {}};
    }

    if (state.stream_error) {
        std::cerr << "\nerror: stream error: " << state.stream_error_message << "\n";
        return {1, state.text, state.input_tokens, state.output_tokens,
                state.content_blocks, state.stop_reason};
    }

    if (state.content_blocks.empty()) {
        std::cerr << "error: no content received in stream\n";
        std::cerr << "response body: " << state.raw_buffer << "\n";
        return {1, {}, 0, 0, {}, {}};
    }

    state.renderer.flush();
    std::cout << "\n";
    return {0, state.text, state.input_tokens, state.output_tokens,
            state.content_blocks, state.stop_reason};
}

std::string short_input_summary(const json& input) {
    const std::string dumped = input.dump();
    if (dumped.size() <= 80) return dumped;
    return dumped.substr(0, 77) + "...";
}

enum class Permission { Allow, Deny };

// Session-scoped allowlist of tool names the user has explicitly
// approved with "(a)lways".
std::unordered_set<std::string>& always_allowed() {
    static std::unordered_set<std::string> s;
    return s;
}

// Non-interactive mode is set by the Telegram bridge: there's no
// stdin to prompt on, so destructive tools are either blanket-allowed
// or blanket-denied based on config.
bool g_non_interactive_tools        = false;
bool g_non_interactive_allow_destructive = false;

Permission prompt_permission(const std::string& tool_name, const json& input) {
    if (always_allowed().count(tool_name)) return Permission::Allow;
    if (!tools::requires_permission(tool_name)) return Permission::Allow;

    if (g_non_interactive_tools) {
        return g_non_interactive_allow_destructive
               ? Permission::Allow
               : Permission::Deny;
    }

    const std::string extra = tools::preview(tool_name, input);
    if (!extra.empty()) {
        std::cout << tui::dim(extra) << "\n";
    } else {
        std::cout << tui::meta("  -> " + tool_name + " " + short_input_summary(input)) << "\n";
    }
    std::cout << tui::bold("allow " + tool_name + "? ")
              << tui::dim("(y)es once, (a)lways this session, (n)o: ")
              << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) return Permission::Deny;
    const char c = line.empty() ? 'n' : static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
    if (c == 'a') {
        always_allowed().insert(tool_name);
        return Permission::Allow;
    }
    if (c == 'y') return Permission::Allow;
    return Permission::Deny;
}

SendResult send_with_tools(const Auth& auth, const std::string& model, int max_tokens,
                           json& messages, const std::string& custom_system) {
    SendResult aggregate;
    aggregate.exit_code = 0;

    while (true) {
        SendResult result = send_conversation(auth, model, max_tokens, messages,
                                              custom_system, /*include_tools=*/true);
        aggregate.input_tokens  += result.input_tokens;
        aggregate.output_tokens += result.output_tokens;
        aggregate.assistant_text = result.assistant_text;
        aggregate.stop_reason    = result.stop_reason;

        if (result.exit_code != 0) {
            aggregate.exit_code = result.exit_code;
            return aggregate;
        }

        messages.push_back({{"role", "assistant"}, {"content", result.content_blocks}});

        if (result.stop_reason != "tool_use") {
            return aggregate;
        }

        json tool_results = json::array();
        for (const auto& block : result.content_blocks) {
            if (block.value("type", "") != "tool_use") continue;
            const std::string tname = block.value("name", std::string{});
            const std::string tid   = block.value("id",   std::string{});
            const json        tinput = block.value("input", json::object());

            std::cout << tui::meta("[tool: " + tname + " " + short_input_summary(tinput) + "]") << "\n";
            log_line("tool " + tname + " input=" + short_input_summary(tinput));

            tools::ToolResult tres;
            const json pre_payload = { {"tool_input", tinput} };
            if (hooks::fire(hooks::Event::PreToolUse, pre_payload, tname) == hooks::Outcome::Block) {
                tres.content  = "hook blocked " + tname;
                tres.is_error = true;
                std::cout << tui::meta("[tool: " + tname + " -> blocked by hook]") << "\n";
            } else if (prompt_permission(tname, tinput) == Permission::Deny) {
                tres.content  = "user denied permission to run " + tname;
                tres.is_error = true;
                std::cout << tui::meta("[tool: " + tname + " -> denied]") << "\n";
            } else if (tname == "Task") {
                // Spawn a no-tools sub-agent: fresh messages array,
                // single round-trip via send_conversation. Streams to
                // the terminal like a normal turn so the user can
                // follow along. The final text becomes this tool's
                // result.
                const std::string sub_prompt = tinput.value("prompt", std::string{});
                if (sub_prompt.empty()) {
                    tres.content  = "error: Task requires a `prompt` argument";
                    tres.is_error = true;
                } else {
                    const std::string sub_label = tinput.value("description", std::string{"sub-agent"});
                    std::cout << tui::meta("  -> " + sub_label + ":") << "\n"
                              << tui::claude_prompt();
                    json sub_messages = json::array({{{"role", "user"}, {"content", sub_prompt}}});
                    const auto sub = send_conversation(auth, model, max_tokens,
                                                       sub_messages, custom_system,
                                                       /*include_tools=*/false);
                    std::cout << "\n";
                    if (sub.exit_code != 0) {
                        tres.content  = "error: sub-agent failed";
                        tres.is_error = true;
                    } else {
                        tres.content  = sub.assistant_text;
                        tres.is_error = false;
                        aggregate.input_tokens  += sub.input_tokens;
                        aggregate.output_tokens += sub.output_tokens;
                    }
                }
                std::cout << tui::meta(tres.is_error
                                       ? "[tool: Task -> error]"
                                       : "[tool: Task -> " + std::to_string(tres.content.size()) + " bytes]")
                          << "\n";
                const json post_payload = {
                    {"tool_input",  tinput},
                    {"tool_result", tres.content},
                    {"is_error",    tres.is_error},
                };
                hooks::fire(hooks::Event::PostToolUse, post_payload, tname);
            } else {
                tres = tools::run(tname, tinput);
                const std::string rsize = std::to_string(tres.content.size());
                std::cout << tui::meta(tres.is_error
                                       ? "[tool: " + tname + " -> error]"
                                       : "[tool: " + tname + " -> " + rsize + " bytes]")
                          << "\n";
                const json post_payload = {
                    {"tool_input",  tinput},
                    {"tool_result", tres.content},
                    {"is_error",    tres.is_error},
                };
                hooks::fire(hooks::Event::PostToolUse, post_payload, tname);
            }

            tool_results.push_back({
                {"type",        "tool_result"},
                {"tool_use_id", tid},
                {"content",     tres.content},
                {"is_error",    tres.is_error},
            });
        }

        if (tool_results.empty()) {
            // stop_reason said tool_use but no tool_use blocks — bail to avoid a loop.
            return aggregate;
        }
        messages.push_back({{"role", "user"}, {"content", tool_results}});
    }
}

void print_usage_line(const SendResult& result) {
    std::cerr << "[usage] input: " << result.input_tokens
              << " tokens  output: " << result.output_tokens << " tokens\n";
}

enum class SlashAction { Continue, Quit, Passthrough };

struct LoopCtx {
    const Auth&        auth;
    int                max_tokens;
    const std::string& custom_system;
    const json&        prices;
    std::string&       model;
    int&               turn_count;
    int&               session_input;
    int&               session_output;
    json&              messages;
};

struct PriceEntry {
    double input;
    double output;
};

PriceEntry get_price(const std::string& model, const json& config_prices) {
    if (config_prices.is_object() && config_prices.contains(model)) {
        const auto& p = config_prices[model];
        return { p.value("input", 0.0), p.value("output", 0.0) };
    }
    // Per-million-token fallbacks based on publicly listed Claude pricing.
    // Config file overrides these by adding an entry under "prices".
    if (model.find("opus")   != std::string::npos) return { 15.0, 75.0 };
    if (model.find("haiku")  != std::string::npos) return { 0.8,   4.0 };
    if (model.find("sonnet") != std::string::npos) return { 3.0,  15.0 };
    return { 3.0, 15.0 };
}

SlashAction dispatch_slash(const std::string& line, LoopCtx& ctx,
                           std::string& passthrough_out) {
    std::string cmd = line;
    std::string args;
    if (const auto sp = line.find(' '); sp != std::string::npos) {
        cmd  = line.substr(0, sp);
        args = line.substr(sp + 1);
        while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    }

    if (cmd == "/help" || cmd == "/?") {
        std::cout << tui::meta(
            "slash commands:\n"
            "  /help              this list\n"
            "  /clear             reset the running conversation\n"
            "  /model <name>      swap the active model\n"
            "  /compact           summarize and replace the running history\n"
            "  /usage             session tokens, cost estimate, subscription windows\n"
            "  /todos             show the current in-session todo list\n"
            "  /memory [user]     open CLAUDE.md in $EDITOR (project by default)\n"
            "  /exit, /quit       leave the REPL\n")
                  << "\n";
        const auto custom = commands::names();
        if (!custom.empty()) {
            std::string body = "custom commands from .claude/commands/ and user dir:\n";
            for (const auto& c : custom) body += "  /" + c + "\n";
            std::cout << tui::meta(body) << "\n";
        }
        return SlashAction::Continue;
    }
    if (cmd == "/todos") {
        const auto result = tools::run("TodoRead", json::object());
        std::cout << tui::meta("current todos:") << "\n"
                  << result.content << "\n";
        return SlashAction::Continue;
    }
    if (cmd == "/memory") {
        const std::string target = (args == "user") ? user_memory_path()
                                                     : project_memory_path();
        if (args == "user") {
            const auto slash = target.rfind('/');
            if (slash != std::string::npos) mkdir_p(target.substr(0, slash));
        }
        const char* editor_env = std::getenv("EDITOR");
        const std::string editor = editor_env && *editor_env ? editor_env : "nano";
        const std::string cmdline = editor + " '" + target + "'";
        std::cout << tui::meta("[opening " + target + " with " + editor + "]") << "\n";
        const int rc = std::system(cmdline.c_str());
        if (rc != 0) {
            std::cout << tui::meta("[editor exited " + std::to_string(rc) + "]") << "\n";
        } else {
            std::cout << tui::meta("[memory will be reloaded on the next turn]") << "\n";
        }
        return SlashAction::Continue;
    }
    if (cmd == "/exit" || cmd == "/quit") {
        return SlashAction::Quit;
    }
    if (cmd == "/clear") {
        ctx.messages        = json::array();
        ctx.turn_count      = 0;
        ctx.session_input   = 0;
        ctx.session_output  = 0;
        std::cout << tui::meta("[conversation cleared]") << "\n";
        return SlashAction::Continue;
    }
    if (cmd == "/model") {
        if (args.empty()) {
            std::cout << tui::meta("[current model: " + ctx.model + "]") << "\n";
        } else {
            ctx.model = args;
            std::cout << tui::meta("[model set to " + ctx.model + "]") << "\n";
        }
        return SlashAction::Continue;
    }
    if (cmd == "/usage") {
        auto header = [](const std::string& key) -> std::string {
            const auto it = g_last_rate_headers.find(key);
            return it == g_last_rate_headers.end() ? std::string() : it->second;
        };

        auto render_bar = [](double pct) {
            constexpr int kBarWidth = 50;
            if (pct < 0.0)   pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            const int filled = static_cast<int>(pct * kBarWidth / 100.0 + 0.5);
            std::string out;
            for (int i = 0; i < filled;                ++i) out += "\u2588";
            for (int i = 0; i < kBarWidth - filled;    ++i) out += ' ';
            return out;
        };

        auto format_reset = [](const std::string& ts_str) {
            if (ts_str.empty()) return std::string();
            const time_t ts = static_cast<time_t>(std::atoll(ts_str.c_str()));
            std::tm tm {};
            localtime_r(&ts, &tm);
            char out[64];
            std::strftime(out, sizeof(out), "%a %b %d at %H:%M (%Z)", &tm);
            return std::string(out);
        };

        auto print_window = [&](const std::string& label,
                                const std::string& util_key,
                                const std::string& reset_key) {
            const std::string util_s  = header(util_key);
            const std::string reset_s = header(reset_key);
            if (util_s.empty()) return;
            const double util = std::atof(util_s.c_str());
            const double pct  = util * 100.0;
            char pct_str[16];
            std::snprintf(pct_str, sizeof(pct_str), "%3.0f%% used", pct);
            std::cout << "  " << tui::bold(label) << "\n"
                      << "  " << render_bar(pct) << " " << pct_str << "\n"
                      << "  " << tui::dim("Resets " + format_reset(reset_s)) << "\n"
                      << "\n";
        };

        // Session summary (our own state).
        const PriceEntry price = get_price(ctx.model, ctx.prices);
        const double in_cost   = (ctx.session_input  / 1'000'000.0) * price.input;
        const double out_cost  = (ctx.session_output / 1'000'000.0) * price.output;
        char session_buf[512];
        std::snprintf(session_buf, sizeof(session_buf),
            "  model %s  turns %d  in %d  out %d  est $%.4f",
            ctx.model.c_str(),
            ctx.turn_count,
            ctx.session_input,
            ctx.session_output,
            in_cost + out_cost);
        std::cout << tui::dim(session_buf) << "\n\n";

        if (header("anthropic-ratelimit-unified-5h-utilization").empty()) {
            std::cout << tui::dim("(no rate-limit data yet — make a request first)")
                      << "\n";
            return SlashAction::Continue;
        }

        print_window("Current session",
                     "anthropic-ratelimit-unified-5h-utilization",
                     "anthropic-ratelimit-unified-5h-reset");
        print_window("Current week (all models)",
                     "anthropic-ratelimit-unified-7d-utilization",
                     "anthropic-ratelimit-unified-7d-reset");
        print_window("Current week (Sonnet only)",
                     "anthropic-ratelimit-unified-7d_sonnet-utilization",
                     "anthropic-ratelimit-unified-7d_sonnet-reset");

        const std::string claim = header("anthropic-ratelimit-unified-representative-claim");
        if (!claim.empty()) {
            std::cout << tui::dim("  binding window: " + claim) << "\n";
        }
        return SlashAction::Continue;
    }
    if (cmd == "/compact") {
        if (ctx.messages.empty()) {
            std::cout << tui::meta("[nothing to compact]") << "\n";
            return SlashAction::Continue;
        }
        json request_messages = ctx.messages;
        request_messages.push_back({
            {"role",    "user"},
            {"content", "Summarize the preceding conversation in 2-3 short paragraphs, "
                        "preserving important context, decisions, code, and open "
                        "questions. Reply with only the summary."},
        });
        std::cout << "\n" << tui::claude_prompt();
        const std::string compact_system = compose_system(ctx.custom_system);
        const auto result = send_conversation(ctx.auth, ctx.model, ctx.max_tokens,
                                              request_messages, compact_system,
                                              /*include_tools=*/false);
        std::cout << "\n";
        if (result.exit_code != 0) {
            std::cout << tui::meta("[compact failed]") << "\n";
            return SlashAction::Continue;
        }
        ctx.session_input  += result.input_tokens;
        ctx.session_output += result.output_tokens;
        ctx.messages = json::array({
            {{"role", "user"},      {"content", "[previous conversation context follows]"}},
            {{"role", "assistant"}, {"content", result.assistant_text}},
        });
        char note[96];
        std::snprintf(note, sizeof(note),
            "[compacted: %d in / %d out tokens]",
            result.input_tokens, result.output_tokens);
        std::cout << tui::meta(note) << "\n";
        return SlashAction::Continue;
    }
    // Fall back to user-defined commands loaded from
    // .claude/commands/*.md (or the global dir). If a match exists we
    // substitute {{args}} and hand the expanded text back to the REPL
    // loop to send as a normal user message.
    const std::string cmd_name = cmd.substr(1); // drop leading '/'
    if (auto expanded = commands::expand(cmd_name, args); expanded) {
        passthrough_out = std::move(*expanded);
        return SlashAction::Passthrough;
    }

    std::cout << tui::meta("[unknown command: " + cmd + " — try /help]") << "\n";
    return SlashAction::Continue;
}

int interactive_loop(const Auth& auth, const std::string& initial_model, int max_tokens,
                     const std::string& custom_system, const json& prices, bool resume,
                     const std::string& initial_message) {
    InterruptGuard interrupt_guard;
    json messages = json::array();
    std::string model = initial_model;

    repl::init(repl_history_path());

    commands::load(config_dir() + "/commands");
    std::vector<std::string> all_slash = {
        "/help", "/clear", "/model", "/compact", "/usage",
        "/todos", "/memory", "/exit", "/quit",
    };
    for (const auto& c : commands::names()) all_slash.push_back("/" + c);
    repl::set_slash_commands(all_slash);

    hooks::fire(hooks::Event::SessionStart, json::object());
    log_line("session start (model=" + model + ")");

    int turn_count         = 0;
    int session_input      = 0;
    int session_output     = 0;

    if (resume) {
        if (auto loaded = load_history(); loaded && loaded->is_array()) {
            messages = *loaded;
            std::cout << tui::meta("[resumed " + std::to_string(messages.size())
                                   + " messages from " + history_path() + "]")
                      << "\n";
        } else {
            std::cout << tui::meta("[no prior session to resume at " + history_path() + "]")
                      << "\n";
        }
    }

    std::cout << tui::bold("Claude CLI interactive mode") << tui::dim(" (model: " + model + ")") << ".\n"
              << tui::dim("Type /help for commands, 'exit' or Ctrl+D to leave.") << "\n\n";

    std::string pending = initial_message;

    while (true) {
        std::string line;
        if (!pending.empty()) {
            line    = std::move(pending);
            pending.clear();
            std::cout << tui::user_prompt() << line << "\n";
        } else {
            if (!repl::read_message(tui::user_prompt(),
                                    tui::continuation_prompt(),
                                    line)) {
                std::cout << "\n";
                break;
            }
        }

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (line == "exit" || line == "quit" || line == ":q") break;

        bool already_recorded = false;
        if (!line.empty() && line.front() == '/') {
            LoopCtx ctx{auth, max_tokens, custom_system, prices, model,
                        turn_count, session_input, session_output, messages};
            std::string expanded;
            const SlashAction action = dispatch_slash(line, ctx, expanded);
            repl::record(line);
            already_recorded = true;
            if (action == SlashAction::Quit) break;
            if (action == SlashAction::Continue) continue;
            if (action == SlashAction::Passthrough) {
                // Custom command resolved to a prompt; fall through
                // with the expanded text as the actual user message.
                line = std::move(expanded);
            }
        }

        if (!already_recorded) repl::record(line);

        if (hooks::fire(hooks::Event::UserPromptSubmit, json{{"prompt", line}}) == hooks::Outcome::Block) {
            std::cout << tui::meta("[hook blocked prompt]") << "\n";
            continue;
        }

        const json snapshot = messages;
        messages.push_back({{"role", "user"}, {"content", line}});

        std::cout << "\n" << tui::claude_prompt();
        const auto turn_start = std::chrono::steady_clock::now();
        const std::string system_for_turn = compose_system(custom_system);
        const auto result = send_with_tools(auth, model, max_tokens, messages, system_for_turn);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - turn_start).count();
        std::cout << "\n";

        if (result.exit_code != 0) {
            messages = snapshot;
            continue;
        }

        ++turn_count;
        session_input  += result.input_tokens;
        session_output += result.output_tokens;

        char status[192];
        std::snprintf(status, sizeof(status),
            "[turn %d  %.1fs  in %d/%d  out %d/%d]",
            turn_count, elapsed,
            result.input_tokens, session_input,
            result.output_tokens, session_output);
        std::cout << tui::meta(status) << "\n";
        log_line("turn " + std::to_string(turn_count)
                 + " model=" + model
                 + " in=" + std::to_string(result.input_tokens)
                 + " out=" + std::to_string(result.output_tokens));

        save_history(messages, model);

        hooks::fire(hooks::Event::Stop, json{{"assistant_text", result.assistant_text}});
    }
    return 0;
}

} // namespace

// Scan `text` for a numbered list at line start (`1. foo`, `2. bar`,
// ...). If at least two consecutive options are found, return them
// so the bridge can render inline-keyboard buttons. Each pair is
// {number_string, item_label}; the label is trimmed to ~24 chars
// so the buttons stay readable in Telegram.
std::vector<std::pair<std::string, std::string>>
extract_numbered_options(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream iss(text);
    std::string        line;
    while (std::getline(iss, line)) {
        size_t i = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i == 0 || i > 2)                       continue;    // need 1-2 digits
        if (i >= line.size() || line[i] != '.')    continue;
        if (i + 1 >= line.size() || line[i + 1] != ' ') continue;
        std::string number = line.substr(0, i);
        std::string label  = line.substr(i + 2);
        // Truncate the label to fit inside a Telegram button cleanly.
        if (label.size() > 28) label = label.substr(0, 27) + "\xE2\x80\xA6"; // …
        out.emplace_back(std::move(number), std::move(label));
    }
    if (out.size() < 2) return {};
    return out;
}

int run_telegram_bridge(const Config& cfg) {
    if (!cfg.telegram.is_object()) {
        std::cerr << "error: config.telegram is missing from config.json\n";
        return 1;
    }
    const std::string token = cfg.telegram.value("bot_token", std::string{});
    if (token.empty()) {
        std::cerr << "error: config.telegram.bot_token is not set\n";
        return 1;
    }

    std::unordered_set<int64_t> allowed;
    if (cfg.telegram.contains("allowed_user_ids")
        && cfg.telegram["allowed_user_ids"].is_array()) {
        for (const auto& v : cfg.telegram["allowed_user_ids"]) {
            if (v.is_number_integer()) allowed.insert(v.get<int64_t>());
        }
    }
    if (allowed.empty()) {
        std::cerr << "error: config.telegram.allowed_user_ids must list "
                     "at least one Telegram user ID\n";
        return 1;
    }

    const bool allow_destructive =
        cfg.telegram.value("allow_destructive_tools", false);

    const Auth auth = resolve_auth();
    if (auth.kind == AuthKind::None) {
        std::cerr << "error: no authentication configured. Run `claude login` "
                     "or set ANTHROPIC_API_KEY before `claude telegram`.\n";
        return 1;
    }

    telegram::Client client(token);

    InterruptGuard interrupt_guard;

    std::cout << tui::bold("Telegram bridge active") << "\n"
              << tui::dim("  authorized user ids: ");
    bool first = true;
    for (const auto& id : allowed) {
        if (!first) std::cout << ",";
        std::cout << " " << id;
        first = false;
    }
    std::cout << "\n"
              << tui::dim(allow_destructive
                          ? "  destructive tools: ALLOWED"
                          : "  destructive tools: blocked (Bash/Write/Edit/MCP)")
              << "\n"
              << tui::dim("  local prompt below; also polling Telegram "
                          "in the background")
              << "\n\n";

    log_line("telegram bridge start (destructive="
             + std::string(allow_destructive ? "yes" : "no") + ")");

    std::mutex                process_mutex;
    std::map<int64_t, json>   user_messages;
    json                      local_messages = json::array();

    // Process one Telegram update — called from the poller thread
    // with `process_mutex` held. Runs send_with_tools in
    // non-interactive tool mode and mirrors the streaming response
    // to the originating chat via editMessageText, plus a periodic
    // `typing` action so the user sees live activity.
    auto process_telegram = [&](const telegram::Update& u) {
        if (u.is_callback) {
            client.answer_callback(u.callback_query_id);
        }

        const std::string who = u.username.empty()
            ? std::to_string(u.user_id)
            : u.username;
        const std::string arrow = u.is_callback ? "tap" : "text";
        std::cout << tui::meta("[telegram " + who + " " + arrow + "] " + u.text)
                  << "\n";
        log_line("telegram rx user=" + std::to_string(u.user_id)
                 + " " + arrow + "=" + u.text);

        if (u.text == "/new" || u.text == "/clear") {
            user_messages.erase(u.user_id);
            client.send_message(u.chat_id, "(history cleared)");
            return;
        }
        if (u.text == "/help" || u.text == "/start") {
            client.send_message(u.chat_id,
                "haiku-claude-cli bridge\n"
                "\n"
                "Send any message and I'll run it through Claude on the "
                "local machine.\n"
                "\n"
                "Commands:\n"
                "  /new     reset this chat's rolling history\n"
                "  /help    this message");
            return;
        }

        json& messages = user_messages[u.user_id];
        if (!messages.is_array()) messages = json::array();
        const json snapshot = messages;
        messages.push_back({{"role", "user"}, {"content", u.text}});

        // Send an initial placeholder; we'll edit it as tokens
        // stream in. Capture its message_id.
        const int64_t placeholder_id =
            client.send_message_with_id(u.chat_id, "\xE2\x80\xA6"); // …

        // Thread hooks: the updater thread watches g_stream_progress
        // and pushes periodic editMessageText + typing actions
        // without blocking the main Claude call.
        StreamProgress progress;
        g_stream_progress = &progress;
        std::atomic<bool> updater_running { true };
        int last_version = 0;

        std::thread updater([&]() {
            while (updater_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (!updater_running.load()) break;

                // Keep the typing indicator alive (~5s TTL in Telegram).
                client.send_chat_action(u.chat_id, "typing");

                if (placeholder_id == 0) continue;
                const int v = progress.version.load(std::memory_order_relaxed);
                if (v == last_version) continue;
                last_version = v;

                std::string snapshot_text;
                {
                    std::lock_guard<std::mutex> lk(progress.mu);
                    snapshot_text = progress.text;
                }
                if (!snapshot_text.empty()) {
                    client.edit_message_text(u.chat_id, placeholder_id, snapshot_text);
                }
            }
        });

        g_non_interactive_tools              = true;
        g_non_interactive_allow_destructive  = allow_destructive;
        std::cout << tui::claude_prompt();
        const std::string effective_system = compose_system(cfg.system);
        const auto result = send_with_tools(auth, cfg.model, cfg.max_tokens,
                                            messages, effective_system);
        std::cout << "\n";
        g_non_interactive_tools = false;

        updater_running.store(false);
        if (updater.joinable()) updater.join();
        g_stream_progress = nullptr;

        if (result.exit_code != 0 || result.assistant_text.empty()) {
            messages = snapshot;
            if (placeholder_id) {
                client.edit_message_text(u.chat_id, placeholder_id,
                    "(error: Claude did not return a response)");
            } else {
                client.send_message(u.chat_id,
                    "(error: Claude did not return a response)");
            }
            log_line("telegram tx user=" + std::to_string(u.user_id)
                     + " -> error");
            return;
        }

        // Auto-detect numbered lists -> inline keyboard buttons.
        std::vector<std::vector<telegram::Button>> keyboard;
        const auto options = extract_numbered_options(result.assistant_text);
        for (const auto& opt : options) {
            telegram::Button b;
            b.text          = opt.first + ". " + opt.second;
            b.callback_data = opt.first;
            keyboard.push_back({ std::move(b) });
        }

        // Final edit — commits the complete text and attaches the
        // inline keyboard in one call. Falls back to a fresh
        // sendMessage if we never got a placeholder id (shouldn't
        // happen in practice).
        if (placeholder_id) {
            client.edit_message_text(u.chat_id, placeholder_id,
                                     result.assistant_text, keyboard);
        } else {
            client.send_message(u.chat_id, result.assistant_text, keyboard);
        }
        log_line("telegram tx user=" + std::to_string(u.user_id)
                 + " out=" + std::to_string(result.output_tokens)
                 + (keyboard.empty()
                        ? ""
                        : " buttons=" + std::to_string(keyboard.size())));
    };

    // Background Telegram poller. Long-polls getUpdates; when
    // messages arrive, grabs process_mutex and hands off to
    // process_telegram.
    std::thread poller([&]() {
        while (!g_interrupted) {
            const auto updates = client.poll(10);
            if (g_interrupted) break;

            for (const auto& u : updates) {
                if (g_interrupted) break;
                if (!allowed.count(u.user_id)) {
                    log_line("telegram reject user=" + std::to_string(u.user_id));
                    continue;
                }
                std::lock_guard<std::mutex> lk(process_mutex);
                process_telegram(u);
            }
        }
    });

    // Main thread — libedit-backed local REPL. Each committed line
    // grabs the same process_mutex so it serializes cleanly against
    // concurrent Telegram traffic.
    repl::init(repl_history_path());
    while (!g_interrupted) {
        std::string line;
        if (!repl::read_message(tui::user_prompt(),
                                tui::continuation_prompt(), line)) {
            std::cout << "\n";
            break;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (line == "exit" || line == "quit" || line == ":q") break;
        repl::record(line);

        std::lock_guard<std::mutex> lk(process_mutex);
        g_non_interactive_tools = false; // local prompt can answer y/a/n

        const json snapshot = local_messages;
        local_messages.push_back({{"role", "user"}, {"content", line}});

        std::cout << "\n" << tui::claude_prompt();
        const std::string effective_system = compose_system(cfg.system);
        const auto result = send_with_tools(auth, cfg.model, cfg.max_tokens,
                                            local_messages, effective_system);
        std::cout << "\n";

        if (result.exit_code != 0) {
            local_messages = snapshot;
        }
    }

    g_interrupted = 1;
    if (poller.joinable()) poller.join();

    std::cout << tui::meta("[telegram bridge stopped]") << "\n";
    log_line("telegram bridge stop");
    return 0;
}

int main(int argc, char* argv[]) {
    tui::init();

    if (argc >= 2) {
        const std::string cmd = argv[1];
        if (cmd == "login")  return do_login();
        if (cmd == "logout") return do_logout();
    }

    const Config cfg = load_config();
    init_logging(cfg.logging_enabled);
    hooks::load(cfg.hooks);
    mcp::init(cfg.mcp_servers);

    if (argc >= 2 && std::string(argv[1]) == "telegram") {
        return run_telegram_bridge(cfg);
    }

    std::string              model         = cfg.model;
    int                      max_tokens    = cfg.max_tokens;
    bool                     interactive   = false;
    bool                     show_usage    = cfg.show_usage;
    bool                     resume        = false;
    std::string              custom_system = cfg.system;
    std::vector<std::string> parts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0], model, max_tokens);
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "haiku-claude-cli " << kVersion << "\n";
            return 0;
        }
        if (arg == "-i" || arg == "--interactive") {
            interactive = true;
            continue;
        }
        if (arg == "-u" || arg == "--usage") {
            show_usage = true;
            continue;
        }
        if (arg == "-r" || arg == "--resume") {
            resume      = true;
            interactive = true;
            continue;
        }
        if (arg == "--plain") {
            tui::set_color_enabled(false);
            continue;
        }
        if (arg == "--color") {
            tui::set_color_enabled(true);
            continue;
        }
        if (arg == "-m" || arg == "--model") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return 1;
            }
            model = argv[++i];
            continue;
        }
        if (arg == "-t" || arg == "--max-tokens") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return 1;
            }
            max_tokens = std::atoi(argv[++i]);
            if (max_tokens <= 0) {
                std::cerr << "error: --max-tokens must be a positive integer\n";
                return 1;
            }
            continue;
        }
        if (arg == "-s" || arg == "--system") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return 1;
            }
            custom_system = argv[++i];
            continue;
        }
        parts.push_back(arg);
    }

    std::string message;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) message += ' ';
        message += parts[i];
    }

    if (!interactive && !isatty(fileno(stdin))) {
        // Only slurp stdin if data is actually ready. Without this
        // check, fread blocks forever when stdin is an open-but-empty
        // pipe (e.g. `ssh host 'claude hi'` without -t, or CI jobs
        // that inherit a runner's idle stdin). 100ms is imperceptible
        // for real pipelines like `cat file | claude "summarize"` but
        // saves the invocation from hanging in non-interactive shells.
        struct pollfd pfd;
        pfd.fd      = STDIN_FILENO;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        const bool has_input =
            poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP));
        if (has_input) {
            std::string stdin_data;
            char        buf[4096];
            size_t      n;
            while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
                stdin_data.append(buf, n);
            }
            while (!stdin_data.empty() && (stdin_data.back() == '\n' || stdin_data.back() == '\r')) {
                stdin_data.pop_back();
            }
            if (!stdin_data.empty()) {
                if (message.empty()) {
                    message = std::move(stdin_data);
                } else {
                    message += "\n\n";
                    message += stdin_data;
                }
            }
        }
    }

    if (!interactive && message.empty()) {
        // With no message and no -i flag, default to interactive mode
        // when stdin is a real terminal. Pipe/redirected cases still
        // get the usage error so scripts fail loudly on empty input.
        if (isatty(fileno(stdin))) {
            interactive = true;
        } else {
            print_usage(argv[0], model, max_tokens);
            return 1;
        }
    }

    const Auth auth = resolve_auth();
    if (auth.kind == AuthKind::None) {
        std::cerr << "error: no authentication configured.\n"
                  << "Run '" << argv[0] << " login' to authenticate with your Claude account,\n"
                  << "or set ANTHROPIC_API_KEY.\n";
        return 1;
    }

    if (interactive) {
        return interactive_loop(auth, model, max_tokens, custom_system, cfg.prices, resume, message);
    }

    InterruptGuard interrupt_guard;
    json messages = json::array({{{"role", "user"}, {"content", message}}});
    const std::string effective_system = compose_system(custom_system);
    const auto result = send_with_tools(auth, model, max_tokens, messages, effective_system);
    if (show_usage) {
        print_usage_line(result);
    }
    return result.exit_code;
}
