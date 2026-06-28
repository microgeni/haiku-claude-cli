#ifndef HAIKU_CLAUDE_CLI_OAUTH_H
#define HAIKU_CLAUDE_CLI_OAUTH_H

#include <optional>
#include <string>

struct OAuthTokens {
	std::string access_token;
	std::string refresh_token;
	long        expires_at = 0;

	bool IsExpired() const;
};

std::string CredentialsPath();

std::optional<OAuthTokens> LoadTokens();
bool                       SaveTokens(const OAuthTokens& tokens);
bool                       DeleteTokens();

std::optional<OAuthTokens> RefreshTokens(const OAuthTokens& existing);

// ── GUI-friendly login building blocks ────────────────────────────────────
// DoLogin() is terminal-bound (prints the URL, reads the pasted code from
// stdin). These split the same flow into two steps a GUI can drive:
//
//   1. BuildAuthUrl() generates PKCE params + the authorize URL. The
//      caller opens the URL in a browser. `outVerifier` / `outState` must
//      be kept and handed to ExchangeCode().
//   2. ExchangeCode() takes the user-pasted "code" (or "code#state"),
//      validates state, exchanges it for tokens, and SaveTokens()s them.
//      Returns the tokens on success, nullopt on any failure.
bool BuildAuthUrl(std::string& outUrl,
                  std::string& outVerifier,
                  std::string& outState);
std::optional<OAuthTokens> ExchangeCode(const std::string& pastedCode,
                                        const std::string& verifier,
                                        const std::string& state);

int DoLogin();
int DoLogout();

#endif
