#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "oauth.h"

using json = nlohmann::json;

namespace {

constexpr const char* kDefaultModel = "claude-sonnet-4-6";
constexpr const char* kApiUrl       = "https://api.anthropic.com/v1/messages";
constexpr const char* kApiVersion   = "2023-06-01";
constexpr const char* kOAuthBeta    = "oauth-2025-04-20";
constexpr const char* kOAuthSystem  = "You are Claude Code, Anthropic's official CLI for Claude.";
constexpr int         kMaxTokens    = 1024;

enum class AuthKind { None, OAuth, ApiKey };

struct Auth {
    AuthKind    kind = AuthKind::None;
    std::string credential;
};

size_t write_callback(char* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(data, size * nmemb);
    return size * nmemb;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-h] <command|message>...\n"
              << "\n"
              << "Commands:\n"
              << "  login                Authenticate via Claude.ai (OAuth + PKCE).\n"
              << "  logout               Delete stored credentials.\n"
              << "\n"
              << "Otherwise, remaining arguments are sent as a one-shot message.\n"
              << "\n"
              << "Authentication (in priority order):\n"
              << "  1. OAuth tokens from 'claude login' (uses Pro/Max quota).\n"
              << "  2. ANTHROPIC_API_KEY environment variable (billed per token).\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help           Show this help and exit.\n";
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

int send_message(const Auth& auth, const std::string& message) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "error: curl_easy_init failed\n";
        return 1;
    }

    json body = {
        {"model",      kDefaultModel},
        {"max_tokens", kMaxTokens},
        {"messages",   json::array({{{"role", "user"}, {"content", message}}})},
    };
    if (auth.kind == AuthKind::OAuth) {
        body["system"] = kOAuthSystem;
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

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli/0.1");

    const CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "error: request failed: " << curl_easy_strerror(res) << "\n";
        return 1;
    }

    json parsed;
    bool parse_ok = true;
    try {
        parsed = json::parse(response);
    } catch (const json::exception&) {
        parse_ok = false;
    }

    if (http_status < 200 || http_status >= 300 || (parse_ok && parsed.contains("error"))) {
        std::cerr << "error: API returned HTTP " << http_status << "\n";
        std::cerr << "response body: " << response << "\n";
        return 1;
    }

    if (!parse_ok) {
        std::cerr << "error: invalid JSON response (HTTP " << http_status << ")\n";
        std::cerr << "response body: " << response << "\n";
        return 1;
    }

    if (!parsed.contains("content") || !parsed["content"].is_array() || parsed["content"].empty()) {
        std::cerr << "error: unexpected response shape\n";
        return 1;
    }

    for (const auto& block : parsed["content"]) {
        if (block.value("type", "") == "text") {
            std::cout << block.value("text", "");
        }
    }
    std::cout << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        const std::string cmd = argv[1];
        if (cmd == "login")  return do_login();
        if (cmd == "logout") return do_logout();
    }

    std::vector<std::string> parts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        parts.push_back(arg);
    }

    if (parts.empty()) {
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

    std::string message = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        message += ' ';
        message += parts[i];
    }
    return send_message(auth, message);
}
