#include "oauth.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
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

// Wrap a string in single quotes for safe use in a shell command line,
// escaping any embedded single quotes ('\'' idiom). The auth URL we build
// is our own construction, but routing it through here keeps the naked
// interpolation from ever becoming a shell-injection foothold.
std::string shell_single_quote(const std::string& s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else           out += c;
	}
	out += "'";
	return out;
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

bool OAuthTokens::IsExpired() const {
	return static_cast<long>(std::time(nullptr)) >= (expires_at - 60);
}

std::string CredentialsPath() {
#ifdef __HAIKU__
	const char* home = std::getenv("HOME");  // flawfinder: ignore
	const std::string dir = std::string(home ? home : "/boot/home") + "/config/settings/claude-cli";
#else
	const char* xdg = std::getenv("XDG_CONFIG_HOME");  // flawfinder: ignore
	std::string dir;
	if (xdg && *xdg) {
		dir = std::string(xdg) + "/claude-cli";
	} else {
		const char* home = std::getenv("HOME");  // flawfinder: ignore
		dir = std::string(home ? home : ".") + "/.config/claude-cli";
	}
#endif
	return dir + "/credentials.json";
}

std::optional<OAuthTokens> LoadTokens() {
	std::ifstream f(CredentialsPath());
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

bool SaveTokens(const OAuthTokens& tokens) {
	const std::string path = CredentialsPath();
	const auto        slash = path.rfind('/');
	if (slash == std::string::npos) return false;
	const std::string dir = path.substr(0, slash);
	if (!paths::MkdirP(dir)) {
		std::cerr << "error: cannot create " << dir << "\n";
		return false;
	}
	const json j = {
		{"access_token",  tokens.access_token},
		{"refresh_token", tokens.refresh_token},
		{"expires_at",    tokens.expires_at},
	};
	// Open with O_CREAT|0600 so the file is never world-readable even
	// transiently — avoids the chmod-after-open TOCTOU race (CWE-362).
	// Note that O_CREAT only applies the mode when it actually creates
	// the file, so nothing may open it beforehand.
	const int fd = ::open(path.c_str(),
	                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		std::cerr << "error: cannot write " << path << "\n";
		return false;
	}
	FILE* fp = ::fdopen(fd, "w");
	if (!fp) { ::close(fd); return false; }
	const std::string serialized = j.dump(2) + "\n";
	const bool ok = std::fwrite(serialized.data(), 1,
	                            serialized.size(), fp) == serialized.size();
	std::fclose(fp); // also closes fd
	if (!ok) {
		std::cerr << "error: cannot write " << path << "\n";
		return false;
	}
	return true;
}

bool DeleteTokens() {
	return std::remove(CredentialsPath().c_str()) == 0;
}

std::optional<OAuthTokens> RefreshTokens(const OAuthTokens& existing) {
	const json body = {
		{"grant_type",    "refresh_token"},
		{"client_id",     kClientId},
		{"refresh_token", existing.refresh_token},
	};
	return post_token_json(body.dump());
}

bool BuildAuthUrl(std::string& outUrl, std::string& outVerifier,
                  std::string& outState) {
	const std::string verifier  = random_base64url(32);
	const std::string challenge = sha256_base64url(verifier);
	const std::string state     = random_base64url(32);
	if (verifier.empty() || state.empty()) return false;

	CURL* curl = curl_easy_init();
	if (!curl) return false;

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

	outUrl      = url.str();
	outVerifier = verifier;
	outState    = state;
	return true;
}

std::optional<OAuthTokens> ExchangeCode(const std::string& pastedCode,
                                        const std::string& verifier,
                                        const std::string& state) {
	std::string pasted = trim(pastedCode);
	if (pasted.empty()) return std::nullopt;

	std::string code = pasted;
	std::string returned_state;
	if (const auto pos = pasted.find('#'); pos != std::string::npos) {
		code           = pasted.substr(0, pos);
		returned_state = pasted.substr(pos + 1);
	}
	if (!returned_state.empty() && returned_state != state)
		return std::nullopt;   // state mismatch (possible CSRF)

	const json body = {
		{"grant_type",    "authorization_code"},
		{"client_id",     kClientId},
		{"code",          code},
		{"redirect_uri",  kRedirectUri},
		{"code_verifier", verifier},
		{"state",         state},
	};
	auto tokens = post_token_json(body.dump());
	if (!tokens) return std::nullopt;
	if (!SaveTokens(*tokens)) return std::nullopt;
	return tokens;
}

int DoLogin() {
	if (auto existing = LoadTokens(); existing) {
		std::cout << "Already logged in. Run 'claude logout' to re-authenticate.\n";
		return 0;
	}

	std::string fAuthurl, verifier, state;
	if (!BuildAuthUrl(fAuthurl, verifier, state)) {
		std::cerr << "error: failed to generate PKCE parameters\n";
		return 1;
	}

	std::cout << "Open this URL in your browser to authorize:\n\n  "
			  << fAuthurl << "\n\n";

#if defined(__APPLE__) || defined(__HAIKU__)
	const std::string open_cmd = "open " + shell_single_quote(fAuthurl) + " >/dev/null 2>&1";
#else
	const std::string open_cmd = "xdg-open " + shell_single_quote(fAuthurl) + " >/dev/null 2>&1";
#endif
	(void)std::system(open_cmd.c_str());  // flawfinder: ignore

	std::cout << "After authorizing, paste the code from the redirect page.\n"
			  << "(It may look like 'code#state' — paste the whole thing.)\n"
			  << "Code: " << std::flush;

	std::string pasted;
	if (!std::getline(std::cin, pasted)) {
		std::cerr << "error: no input\n";
		return 1;
	}

	auto tokens = ExchangeCode(pasted, verifier, state);
	if (!tokens) {
		std::cerr << "error: token exchange failed (or empty/invalid code)\n";
		return 1;
	}

	std::cout << "Logged in. Credentials saved to " << CredentialsPath() << "\n";
	return 0;
}

int DoLogout() {
	if (DeleteTokens()) {
		std::cout << "Logged out.\n";
	} else {
		std::cout << "No stored credentials.\n";
	}
	return 0;
}
