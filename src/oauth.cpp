#include "oauth.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "paths.h"

using json = nlohmann::json;

namespace {

constexpr const char* kAuthEndpoint  = "https://claude.ai/oauth/authorize";
constexpr const char* kTokenEndpoint = "https://console.anthropic.com/v1/oauth/token";
constexpr const char* kClientId      = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* kRedirectUri   = "https://console.anthropic.com/oauth/code/callback";
constexpr const char* kScopes        = "org:create_api_key user:profile user:inference";

std::string base64url(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = (i + 1 < len) ? data[i + 1] : 0u;
        const unsigned b2 = (i + 2 < len) ? data[i + 2] : 0u;
        const unsigned n  = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(n >> 18) & 0x3f]);
        out.push_back(tbl[(n >> 12) & 0x3f]);
        if (i + 1 < len) out.push_back(tbl[(n >> 6) & 0x3f]);
        if (i + 2 < len) out.push_back(tbl[n & 0x3f]);
    }
    return out;
}

std::string random_base64url(size_t raw_bytes) {
    std::vector<unsigned char> buf(raw_bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(raw_bytes)) != 1) return {};
    return base64url(buf.data(), raw_bytes);
}

std::string sha256_base64url(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    EVP_MD_CTX*   ctx      = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    return base64url(hash, hash_len);
}

std::string url_encode(CURL* curl, const std::string& s) {
    char* enc = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string result = enc ? enc : "";
    curl_free(enc);
    return result;
}

size_t write_cb(char* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(data, size * nmemb);
    return size * nmemb;
}

std::string trim(std::string s) {
    const auto keep = [](int c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), keep));
    s.erase(std::find_if(s.rbegin(), s.rend(), keep).base(), s.end());
    return s;
}

std::optional<OAuthTokens> post_token_json(const std::string& body_str) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;

    std::string  response;
    curl_slist*  headers = nullptr;
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, kTokenEndpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli/0.1");

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "error: token request failed: " << curl_easy_strerror(res) << "\n";
        return std::nullopt;
    }

    json parsed;
    try {
        parsed = json::parse(response);
    } catch (const json::exception& e) {
        std::cerr << "error: token response parse failed (HTTP " << status << "): " << e.what() << "\n";
        return std::nullopt;
    }

    if (status < 200 || status >= 300 || parsed.contains("error")) {
        std::cerr << "error: token endpoint HTTP " << status;
        if (parsed.contains("error")) {
            std::cerr << ": " << parsed["error"].dump();
            if (parsed.contains("error_description")) {
                std::cerr << " — " << parsed["error_description"].get<std::string>();
            }
        }
        std::cerr << "\n";
        return std::nullopt;
    }

    try {
        OAuthTokens t;
        t.access_token  = parsed.at("access_token").get<std::string>();
        t.refresh_token = parsed.value("refresh_token", std::string{});
        const long expires_in = parsed.value("expires_in", 28800L);
        t.expires_at = static_cast<long>(std::time(nullptr)) + expires_in;
        return t;
    } catch (const json::exception& e) {
        std::cerr << "error: token response missing fields: " << e.what() << "\n";
        return std::nullopt;
    }
}

} // namespace

bool OAuthTokens::is_expired() const {
    return static_cast<long>(std::time(nullptr)) >= (expires_at - 60);
}

std::string credentials_path() {
#ifdef __HAIKU__
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "/boot/home") + "/config/settings/claude-cli";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string dir;
    if (xdg && *xdg) {
        dir = std::string(xdg) + "/claude-cli";
    } else {
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : ".") + "/.config/claude-cli";
    }
#endif
    return dir + "/credentials.json";
}

std::optional<OAuthTokens> load_tokens() {
    std::ifstream f(credentials_path());
    if (!f.is_open()) return std::nullopt;
    try {
        const json j = json::parse(f);
        OAuthTokens t;
        t.access_token  = j.at("access_token").get<std::string>();
        t.refresh_token = j.value("refresh_token", std::string{});
        t.expires_at    = j.value("expires_at", 0L);
        return t;
    } catch (...) {
        return std::nullopt;
    }
}

bool save_tokens(const OAuthTokens& tokens) {
    const std::string path = credentials_path();
    const auto        slash = path.rfind('/');
    if (slash == std::string::npos) return false;
    const std::string dir = path.substr(0, slash);
    if (!paths::mkdir_p(dir)) {
        std::cerr << "error: cannot create " << dir << "\n";
        return false;
    }
    const json j = {
        {"access_token",  tokens.access_token},
        {"refresh_token", tokens.refresh_token},
        {"expires_at",    tokens.expires_at},
    };
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "error: cannot write " << path << "\n";
        return false;
    }
    f << j.dump(2) << "\n";
    f.close();
    chmod(path.c_str(), 0600);
    return true;
}

bool delete_tokens() {
    return std::remove(credentials_path().c_str()) == 0;
}

std::optional<OAuthTokens> refresh_tokens(const OAuthTokens& existing) {
    const json body = {
        {"grant_type",    "refresh_token"},
        {"client_id",     kClientId},
        {"refresh_token", existing.refresh_token},
    };
    return post_token_json(body.dump());
}

int do_login() {
    if (auto existing = load_tokens(); existing) {
        std::cout << "Already logged in. Run 'claude logout' to re-authenticate.\n";
        return 0;
    }

    const std::string verifier  = random_base64url(32);
    const std::string challenge = sha256_base64url(verifier);
    const std::string state     = random_base64url(32);

    if (verifier.empty() || state.empty()) {
        std::cerr << "error: failed to generate PKCE parameters\n";
        return 1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "error: curl_easy_init failed\n";
        return 1;
    }

    std::ostringstream url;
    url << kAuthEndpoint
        << "?response_type=code"
        << "&client_id="             << url_encode(curl, kClientId)
        << "&redirect_uri="          << url_encode(curl, kRedirectUri)
        << "&scope="                 << url_encode(curl, kScopes)
        << "&code_challenge="        << url_encode(curl, challenge)
        << "&code_challenge_method=S256"
        << "&state="                 << url_encode(curl, state);
    curl_easy_cleanup(curl);

    const std::string auth_url = url.str();

    std::cout << "Open this URL in your browser to authorize:\n\n  "
              << auth_url << "\n\n";

#if defined(__APPLE__) || defined(__HAIKU__)
    const std::string open_cmd = "open '" + auth_url + "' >/dev/null 2>&1";
#else
    const std::string open_cmd = "xdg-open '" + auth_url + "' >/dev/null 2>&1";
#endif
    (void)std::system(open_cmd.c_str());

    std::cout << "After authorizing, paste the code from the redirect page.\n"
              << "(It may look like 'code#state' — paste the whole thing.)\n"
              << "Code: " << std::flush;

    std::string pasted;
    if (!std::getline(std::cin, pasted)) {
        std::cerr << "error: no input\n";
        return 1;
    }
    pasted = trim(pasted);
    if (pasted.empty()) {
        std::cerr << "error: empty code\n";
        return 1;
    }

    std::string code = pasted;
    std::string returned_state;
    if (const auto pos = pasted.find('#'); pos != std::string::npos) {
        code           = pasted.substr(0, pos);
        returned_state = pasted.substr(pos + 1);
    }
    if (!returned_state.empty() && returned_state != state) {
        std::cerr << "error: state mismatch (possible CSRF)\n";
        return 1;
    }

    const json body = {
        {"grant_type",    "authorization_code"},
        {"client_id",     kClientId},
        {"code",          code},
        {"redirect_uri",  kRedirectUri},
        {"code_verifier", verifier},
        {"state",         state},
    };

    auto tokens = post_token_json(body.dump());
    if (!tokens) {
        std::cerr << "error: token exchange failed\n";
        return 1;
    }
    if (!save_tokens(*tokens)) return 1;

    std::cout << "Logged in. Credentials saved to " << credentials_path() << "\n";
    return 0;
}

int do_logout() {
    if (delete_tokens()) {
        std::cout << "Logged out.\n";
    } else {
        std::cout << "No stored credentials.\n";
    }
    return 0;
}
