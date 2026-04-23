#ifndef HAIKU_CLAUDE_CLI_TELEGRAM_H
#define HAIKU_CLAUDE_CLI_TELEGRAM_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"

// Forward declaration so the StartThinkingUpdater signature can
// reference api::StreamProgress without pulling in all of api.h
// (which would create a circular include with api.h → telegram.h).
namespace api { struct StreamProgress; }

// Tiny Telegram Bot API client over libcurl. Enough for the v1.1
// remote-control bridge: long-polling getUpdates and sending text
// back via sendMessage.
//
// Does NOT own an event loop; callers are expected to call poll()
// in their own loop. Thread-unsafe — one client per thread.
namespace telegram {

// Remote-control mute toggle. When true, every outbound call to the
// bot API (sendMessage / editMessageText / sendChatAction) is
// suppressed by the RemoteControl wrappers. Incoming messages still
// process locally; nothing leaves the machine until /unmute. Lives
// here rather than inside RemoteControl so the REPL's status bar can
// read it to show the "muted" label.
extern std::atomic<bool> g_muted;

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
	//
	// If `keep_running` is non-null, a curl progress callback
	// checks it roughly every second and aborts the in-flight
	// HTTP request as soon as `*keep_running` becomes false. This
	// lets callers (e.g. RemoteControl::stop) cut a blocking
	// long-poll short without waiting for the full timeout.
	std::vector<Update> poll(int timeout_sec = 25,
							 std::atomic<bool>* keep_running = nullptr);

	// POST sendMessage to the given chat. Long messages are chunked
	// into ~4000-char pieces to stay under Telegram's 4096-char
	// per-message limit. Returns false on any HTTP/transport failure.
	//
	// Optional `keyboard` renders an inline keyboard below the
	// message — the outer vector is rows, the inner one is columns.
	// Only the final chunk of a split message carries the keyboard.
	bool SendMessage(int64_t chat_id, const std::string& text,
					  const std::vector<std::vector<Button>>& keyboard = {});

	// Like SendMessage, but returns the message_id of the first
	// chunk — callers can pass it to EditMessageText() later for
	// streaming updates. Returns 0 on failure. Only the final chunk
	// carries the keyboard.
	int64_t SendMessageWithId(int64_t chat_id, const std::string& text,
								 const std::vector<std::vector<Button>>& keyboard = {});

	// editMessageText — replace the text of a previously-sent
	// message. Telegram rate-limits edits to about once per second
	// per message, so callers should throttle. Silently ignores the
	// "message is not modified" error which Telegram returns when
	// new text matches the old.
	bool EditMessageText(int64_t chat_id, int64_t message_id,
						   const std::string& text,
						   const std::vector<std::vector<Button>>& keyboard = {});

	// sendChatAction — push a transient "typing..." (or other)
	// indicator to the chat for about 5 seconds. Call periodically
	// while a long operation is in flight.
	bool SendChatAction(int64_t chat_id, const std::string& action = "typing");

	// answerCallbackQuery — call after receiving a callback Update so
	// Telegram dismisses the spinner on the tapped button. Optional
	// `notice` pops a short toast on the user's side.
	bool AnswerCallback(const std::string& callback_query_id,
						 const std::string& notice = {});

	// deleteMessage — remove a previously-sent message from the chat.
	// Best-effort: returns false on failure but callers should not
	// treat that as fatal (e.g. the message may already be gone).
	bool DeleteMessage(int64_t chat_id, int64_t message_id);

	const std::string& token() const { return fToken; }

private:
	std::string ApiUrl(const std::string& method) const;
	bool PostJson(const std::string& method,
				   const std::string& body,
				   std::string*       out_response,
				   long               timeout_sec,
				   std::atomic<bool>* keep_running = nullptr);

	std::string fToken;
	int64_t     fNextOffset = 0;
};

// Self-contained background Telegram poller spawned on demand by
// the /remote-control slash command inside the REPL. Runs in its
// own thread, polls Telegram for incoming messages from allowed
// users, and hands each one to api::SendWithTools on the local
// machine. Each Telegram user gets an independent rolling history
// (not shared with the REPL's own messages, but local turns are
// mirrored to the primary chat).
//
// Fully bidirectional: streaming edits, typing indicator, inline
// permission buttons, numbered-option buttons, and local input
// mirroring.
class RemoteControl {
public:
	// Validate config.telegram has the required bot_token and at
	// least one allowed_user_id. On failure, populates *reason
	// with a user-friendly explanation and returns false.
	static bool ConfigIsValid(const config::Config& cfg, std::string* reason);

	// authGetter is called before each Claude turn to obtain a
	// fresh token — pass a lambda that returns the REPL's own
	// auth variable so Telegram piggybacks on the session that
	// is already being refreshed by the interactive loop.
	RemoteControl(const config::Config& cfg,
				  std::function<config::Auth()> authGetter,
				  const std::string& custom_system);
	~RemoteControl();
	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;

	bool Start();
	void Stop();
	bool Running() const;

	// Serialise turns between the local REPL and the Telegram
	// worker. AcquireTurn() blocks until no other turn is in
	// progress; ReleaseTurn() clears the token.
	void AcquireTurn();
	void ReleaseTurn();

	// Mirror a locally-initiated turn to the primary Telegram chat.
	// Call order (all called from the local REPL turn, after
	// AcquireTurn() so the turn lock is already held):
	//   1. MirrorPrompt()  — sends "> text" + placeholder to Telegram.
	//   2. StartThinkingUpdater(progress) — starts the animated
	//      placeholder thread, pinned to the caller's StreamProgress.
	//   3. api::SendWithTools(…)
	//   4. StopThinkingUpdater() — joins the updater thread.
	//   5. ReleaseTurn()   — MUST be after StopThinkingUpdater.
	//   6. MirrorToPrimary() or MirrorCancel().
	void MirrorPrompt(const std::string& user_text);
	void MirrorToPrimary(const std::string& assistant_text);
	void MirrorCancel();

	// Animate the primary chat's "thinking" placeholder while a
	// local turn is in progress. `progress` must remain valid for
	// the lifetime of the updater thread — pass the StreamProgress
	// that lives on the same stack frame as api::SendWithTools so
	// the pointer is guaranteed live until StopThinkingUpdater()
	// returns. Pass nullptr to suppress streaming-text updates (dot
	// animation only).
	// StopThinkingUpdater() must be called before ReleaseTurn().
	void StartThinkingUpdater(api::StreamProgress* progress);
	void StopThinkingUpdater();

private:
	void PollLoop();
	void WorkLoop();
	// Try to handle a slash command immediately, without waiting for
	// AcquireTurn().  Returns true if the command was fully serviced
	// (so the caller can skip AcquireTurn / ProcessUpdate).  Returns
	// false for plain prompts or Passthrough commands that still need
	// to go through the full Claude turn.
	bool TryHandleSlashImmediate(const Update& u);
	void TgSend(int64_t chat, const std::string& text);
	void ProcessUpdate(const Update& u);

	Client                       fClient;
	std::unordered_set<int64_t>  fAllowed;
	int64_t                      fPrimaryUserId = 0;
	int64_t                      fPrimaryThinkingMsgId = 0;
	// Chat ID that the persistent permission/status hooks target.
	// Set to the sender's chat_id at the start of each Telegram-
	// origin turn; reset to fPrimaryUserId when that turn ends.
	std::atomic<int64_t>         fActiveChatId { 0 };
	std::atomic<bool>            fUpdaterRunning { false };
	std::thread                  fUpdaterThread;
	bool                         fAllowDestructive = false;
	std::function<config::Auth()> fAuthGetter;
	std::string                  fCustomSystem;
	std::string                  fCfgModel;
	int                          fCfgMaxTokens;
	std::map<int64_t, nlohmann::json> fUserMessages;
	std::atomic<bool>            fRunning { false };
	std::thread                  fPoller;
	// Turn-token: serialises local and Telegram-origin turns
	// without holding a mutex across SendWithTools.
	std::mutex                   fTurnMu;
	std::condition_variable      fTurnCv;
	bool                         fTurnInProgress = false;
	// Worker thread for Telegram-origin turns.
	std::deque<Update>           fWorkQueue;
	std::mutex                   fWorkMu;
	std::condition_variable      fWorkCv;
	std::atomic<bool>            fWorkerRunning { false };
	std::thread                  fWorker;
	// Shared queue for perm:* callback taps.
	std::deque<Update>           fPermQueue;
	std::mutex                   fPermMu;
	std::condition_variable      fPermCv;
};

} // namespace telegram

#endif
