#ifndef HAIKU_CLAUDE_CLI_TELEGRAM_H
#define HAIKU_CLAUDE_CLI_TELEGRAM_H

#include <cstdint>
#include <string>
#include <vector>

// Tiny Telegram Bot API client over libcurl. Enough for the v1.1
// remote-control bridge: long-polling getUpdates and sending text
// back via sendMessage.
//
// Does NOT own an event loop; callers are expected to call poll()
// in their own loop. Thread-unsafe — one client per thread.
namespace telegram {

struct Update {
    int64_t     update_id = 0;
    int64_t     chat_id   = 0;
    int64_t     user_id   = 0;
    std::string username;     // without leading @, may be empty
    std::string text;
};

class Client {
public:
    explicit Client(std::string bot_token);

    // Long-poll for new updates, advancing the internal offset past
    // whatever comes back. `timeout_sec` is passed to Telegram (it
    // blocks server-side for up to that long). Libcurl adds a few
    // seconds on top before giving up on the request itself.
    //
    // Returns an empty vector on timeout, network error, or when the
    // response has no text messages.
    std::vector<Update> poll(int timeout_sec = 25);

    // POST sendMessage to the given chat. Long messages are chunked
    // into ~4000-char pieces to stay under Telegram's 4096-char
    // per-message limit. Returns false on any HTTP/transport failure.
    bool send_message(int64_t chat_id, const std::string& text);

    const std::string& token() const { return token_; }

private:
    std::string api_url(const std::string& method) const;
    bool post_json(const std::string& method,
                   const std::string& body,
                   std::string*       out_response,
                   long               timeout_sec);

    std::string token_;
    int64_t     next_offset_ = 0;
};

} // namespace telegram

#endif
