#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "oauth.h"
#include "repl.h"
#include "tui.h"

using json = nlohmann::json;

namespace {

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
};

struct SendResult {
    int         exit_code = 0;
    std::string assistant_text;
    int         input_tokens  = 0;
    int         output_tokens = 0;
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

        if (type == "content_block_delta") {
            const auto& delta = j["delta"];
            if (delta.value("type", "") == "text_delta") {
                const std::string chunk = delta.value("text", "");
                state->renderer.write(chunk);
                state->text += chunk;
                state->saw_text = true;
            }
        } else if (type == "message_start") {
            if (j.contains("message") && j["message"].contains("usage")) {
                const auto& u = j["message"]["usage"];
                state->input_tokens  = u.value("input_tokens",  0);
                state->output_tokens = u.value("output_tokens", 0);
            }
        } else if (type == "message_delta") {
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

void print_usage(const char* prog) {
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
              << "  -m, --model MODEL    Model to use (default: " << kDefaultModel << ").\n"
              << "  -t, --max-tokens N   Max tokens in response (default: " << kMaxTokens << ").\n"
              << "  -s, --system TEXT    Custom system prompt (appended after the\n"
              << "                       required Claude Code prefix when OAuth is used).\n"
              << "  -u, --usage          After the response, print input/output token\n"
              << "                       usage to stderr.\n"
              << "  -r, --resume         Start the REPL pre-loaded with the last saved\n"
              << "                       session (implies -i).\n"
              << "      --plain          Disable ANSI color output.\n"
              << "      --color          Force ANSI color output, even when piped.\n"
              << "  -h, --help           Show this help and exit.\n"
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
                             const json& messages, const std::string& custom_system) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "error: curl_easy_init failed\n";
        return {1, {}};
    }

    json body = {
        {"model",      model},
        {"max_tokens", max_tokens},
        {"stream",     true},
        {"messages",   messages},
    };

    std::string system_prompt;
    if (auth.kind == AuthKind::OAuth) {
        system_prompt = kOAuthSystem;
        if (!custom_system.empty()) {
            system_prompt += "\n\n";
            system_prompt += custom_system;
        }
    } else if (!custom_system.empty()) {
        system_prompt = custom_system;
    }
    if (!system_prompt.empty()) {
        body["system"] = system_prompt;
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli/0.1");

    const CURLcode res = curl_easy_perform(curl);
    spinner.stop();
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "\nerror: request failed: " << curl_easy_strerror(res) << "\n";
        return {1, {}};
    }

    if (http_status < 200 || http_status >= 300) {
        std::cerr << "\nerror: API returned HTTP " << http_status << "\n";
        std::cerr << "response body: " << state.raw_buffer << "\n";
        return {1, {}};
    }

    if (state.stream_error) {
        std::cerr << "\nerror: stream error: " << state.stream_error_message << "\n";
        return {1, state.text, state.input_tokens, state.output_tokens};
    }

    if (!state.saw_text) {
        std::cerr << "error: no text received in stream\n";
        std::cerr << "response body: " << state.raw_buffer << "\n";
        return {1, {}, 0, 0};
    }

    state.renderer.flush();
    std::cout << "\n";
    return {0, state.text, state.input_tokens, state.output_tokens};
}

void print_usage_line(const SendResult& result) {
    std::cerr << "[usage] input: " << result.input_tokens
              << " tokens  output: " << result.output_tokens << " tokens\n";
}

int interactive_loop(const Auth& auth, const std::string& model, int max_tokens,
                     const std::string& custom_system, bool show_usage, bool resume,
                     const std::string& initial_message) {
    json messages = json::array();

    repl::init(repl_history_path());

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
              << tui::dim("Type 'exit', 'quit', or press Ctrl+D to leave.") << "\n\n";

    std::string pending = initial_message;

    while (true) {
        std::string line;
        if (!pending.empty()) {
            line    = std::move(pending);
            pending.clear();
            std::cout << tui::user_prompt() << line << "\n";
        } else {
            if (!repl::read_line(tui::user_prompt(), line)) {
                std::cout << "\n";
                break;
            }
        }

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (line == "exit" || line == "quit" || line == ":q") break;

        repl::record(line);
        messages.push_back({{"role", "user"}, {"content", line}});

        std::cout << "\n" << tui::claude_prompt();
        const auto result = send_conversation(auth, model, max_tokens, messages, custom_system);
        std::cout << "\n";

        if (show_usage) {
            print_usage_line(result);
        }

        if (result.exit_code != 0) {
            messages.erase(messages.end() - 1);
            continue;
        }
        messages.push_back({{"role", "assistant"}, {"content", result.assistant_text}});
        save_history(messages, model);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    tui::init();

    if (argc >= 2) {
        const std::string cmd = argv[1];
        if (cmd == "login")  return do_login();
        if (cmd == "logout") return do_logout();
    }

    std::string              model         = kDefaultModel;
    int                      max_tokens    = kMaxTokens;
    bool                     interactive   = false;
    bool                     show_usage    = false;
    bool                     resume        = false;
    std::string              custom_system;
    std::vector<std::string> parts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
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
        print_usage(argv[0]);
        return 1;
    }

    const Auth auth = resolve_auth();
    if (auth.kind == AuthKind::None) {
        std::cerr << "error: no authentication configured.\n"
                  << "Run '" << argv[0] << " login' to authenticate with your Claude account,\n"
                  << "or set ANTHROPIC_API_KEY.\n";
        return 1;
    }

    if (interactive) {
        return interactive_loop(auth, model, max_tokens, custom_system, show_usage, resume, message);
    }

    const json messages = json::array({{{"role", "user"}, {"content", message}}});
    const auto result = send_conversation(auth, model, max_tokens, messages, custom_system);
    if (show_usage) {
        print_usage_line(result);
    }
    return result.exit_code;
}
