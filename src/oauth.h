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

int DoLogin();
int DoLogout();

#endif
