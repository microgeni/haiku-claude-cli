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

struct Button {
    std::string text;          // label rendered on the button
    std::string callback_data; // opaque string Telegram echoes back on tap
};

struct Update {
    int64_t     update_id = 0;
    int64_t     chat_id   = 0;
    int64_t     user_id   = 0;
    std::string username;     // without leading @, may be empty
    std::string text;

    // When true this update is a button tap from an inline keyboard
    // on a prior message. `text` holds the callback_data; the REPL
    // treats it as if the user had just typed that string.
    bool        is_callback = false;
    std::string callback_query_id;
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
    //
    // Optional `keyboard` renders an inline keyboard below the
    // message — the outer vector is rows, the inner one is columns.
    // Only the final chunk of a split message carries the keyboard.
    bool send_message(int64_t chat_id, const std::string& text,
                      const std::vector<std::vector<Button>>& keyboard = {});

    // Like send_message, but returns the message_id of the first
    // chunk — callers can pass it to edit_message_text() later for
    // streaming updates. Returns 0 on failure. Only the final chunk
    // carries the keyboard.
    int64_t send_message_with_id(int64_t chat_id, const std::string& text,
                                 const std::vector<std::vector<Button>>& keyboard = {});

    // editMessageText — replace the text of a previously-sent
    // message. Telegram rate-limits edits to about once per second
    // per message, so callers should throttle. Silently ignores the
    // "message is not modified" error which Telegram returns when
    // new text matches the old.
    bool edit_message_text(int64_t chat_id, int64_t message_id,
                           const std::string& text,
                           const std::vector<std::vector<Button>>& keyboard = {});

    // sendChatAction — push a transient "typing..." (or other)
    // indicator to the chat for about 5 seconds. Call periodically
    // while a long operation is in flight.
    bool send_chat_action(int64_t chat_id, const std::string& action = "typing");

    // answerCallbackQuery — call after receiving a callback Update so
    // Telegram dismisses the spinner on the tapped button. Optional
    // `notice` pops a short toast on the user's side.
    bool answer_callback(const std::string& callback_query_id,
                         const std::string& notice = {});

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
