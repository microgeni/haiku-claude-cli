#ifndef HAIKU_CLAUDE_CLI_OAUTH_H
#define HAIKU_CLAUDE_CLI_OAUTH_H

#include <optional>
#include <string>

struct OAuthTokens {
    std::string access_token;
    std::string refresh_token;
    long        expires_at = 0;

    bool is_expired() const;
};

std::string credentials_path();

std::optional<OAuthTokens> load_tokens();
bool                       save_tokens(const OAuthTokens& tokens);
bool                       delete_tokens();

std::optional<OAuthTokens> refresh_tokens(const OAuthTokens& existing);

int do_login();
int do_logout();

#endif
