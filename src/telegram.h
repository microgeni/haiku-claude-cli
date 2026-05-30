#ifndef HAIKU_CLAUDE_CLI_TELEGRAM_H
#define HAIKU_CLAUDE_CLI_TELEGRAM_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "output_sink.h"
#include "structured_sink.h"

// StreamProgress is still used by api.h for the local-mirror stub.
// No longer needed directly in telegram.h after Step 5.


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

// TelegramSink — implements both OutputSink (Step 2 interface, so it can
// be passed to api::SendWithTools unchanged) and sink::StructuredSink (the
// Step 3 lean-Telegram model). The OutputSink methods are a thin adapter
// layer that delegates to the StructuredSink methods; they will be removed
// when api::SendWithTools is upgraded to take StructuredSink* directly
// (Step 4).
//
// One instance per user turn. Stack-allocated inside ProcessUpdate() for
// the lifetime of the SendWithTools call.
//
// Lean rules enforced structurally (empty method bodies, not flags):
//   OnMeta / OnDiag → suppressed (no chat message sent)
//   SetStatus       → sendChatAction("typing"), ephemeral, no history
//   ToolFinished    → collapsed "🔧 bash ✓" line, detail hidden
//   OnError         → edits the current message in place (or sends new)
class TelegramSink : public OutputSink, public sink::StructuredSink {
public:
	// `client` must outlive this object (owned by RemoteControl).
	// `chatId` is the chat to send replies to.
	// `allowDestructive` controls whether AskPermission auto-approves.
	// `allowedSet` is the session-scoped always-allow tool set.
	// `permQueue` / `permMu` / `permCv` are the shared queues that
	// PollLoop pushes perm:* callback taps into.
	struct PermQueue {
		std::deque<std::string>  callbacks; // callback_data values
		std::mutex               mu;
		std::condition_variable  cv;
	};

	TelegramSink(Client& client, int64_t chatId, bool allowDestructive,
	             std::unordered_set<std::string>& allowedSet,
	             PermQueue& permQueue);
	~TelegramSink() override = default;

	TelegramSink(const TelegramSink&)            = delete;
	TelegramSink& operator=(const TelegramSink&) = delete;

	// ── sink::StructuredSink ──────────────────────────────────────────────
	void BeginMessage(const std::string& role) override;
	void AppendText(const std::string& chunk)  override;
	void EndMessage()                          override;
	void ToolStarted(const std::string& name,
	                 const std::string& summary) override;
	void ToolFinished(const std::string& name,
	                  bool ok,
	                  const std::string& detail) override;
	int  AskChoice(const std::string& prompt,
	               const std::vector<std::string>& options) override;
	sink::Permission AskPermission(const std::string& tool,
	                               const std::string& preview) override;
	void SetStatus(sink::StatusKind kind) override;
	void OnError(const std::string& message) override; // StructuredSink version

	// ── OutputSink (adapter, removed in Step 4) ───────────────────────────
	void OnText(const std::string& chunk) override;
	void OnMeta(const std::string&)       override {} // suppressed by design
	void OnDiag(const std::string&)       override {} // suppressed by design
	void OnToolStatus(const std::string& phase) override;
	api::Permission AskPermission(const std::string& tool,
	                              const nlohmann::json& input,
	                              std::string* denial_reason) override;

private:
	static constexpr int64_t kEditThrottleMs = 500; // max ~2 edits/sec

	Client&                          fClient;
	int64_t                          fChatId;
	bool                             fAllowDestructive;
	std::unordered_set<std::string>& fAllowedSet;
	PermQueue&                       fPermQueue;

	// Streaming state for the current assistant message.
	std::string fBuffer;           // accumulated text
	int64_t     fCurrentMsgId{0}; // Telegram message_id being live-edited
	int64_t     fLastEditMs{0};   // epoch-ms of last editMessageText call
	bool        fInMessage{false};

	// Collapsed tool cards appended to the current message.
	struct ToolCard {
		std::string name;
		std::string summary;
		bool        started{false};
		bool        finished{false};
		bool        ok{false};
	};
	std::vector<ToolCard> fToolCards;

	// Internal helpers.
	int64_t        SentPlaceholder(const std::string& text = "\xE2\x80\xA6"); // "…"
	bool           EditCurrent(bool final = false);
	std::string    BuildDisplayText() const;
	static int64_t NowMs();
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

	// Register a provider that returns a read-only snapshot of the
	// local REPL's `messages` array.  When set, ProcessUpdate prepends
	// this shared context before the Telegram user's own thread so
	// Claude sees both sides of the conversation on every remote turn.
	//
	// The lambda is called under the turn lock (AcquireTurn has
	// already been acquired), so the snapshot is always consistent
	// with the just-finished local turn — no additional mutex needed.
	// Passing nullptr (or never calling this) restores the original
	// silo behaviour where each Telegram user has independent context.
	void SetSharedHistory(std::function<nlohmann::json()> provider);

	// Register a write-back callback invoked (under the turn lock)
	// after each successful Telegram-origin turn. The two arguments
	// are the user message and the assistant message that were just
	// exchanged, both as JSON objects with "role"/"content" keys.
	// Wire this up alongside SetSharedHistory so the local REPL's
	// messages[] array (and thus its scroll history) is kept in sync
	// with remote turns.
	void SetSharedHistoryAppender(
		std::function<void(nlohmann::json, nlohmann::json)> appender);

	// Accessors used by session.cpp LocalWorker to construct a
	// TelegramSink for local turns that should stream to the primary chat.
	int64_t                          PrimaryUserId()    const { return fPrimaryUserId; }
	Client&                          GetClient()              { return fClient; }
	bool                             AllowDestructive() const { return fAllowDestructive; }
	std::unordered_set<std::string>& AllowedToolsRef()        { return fAllowedTools; }
	TelegramSink::PermQueue&         PermQueueRef()           { return fPermQueue; }

	// Send a "> user_text" preamble to the primary chat before a local
	// turn starts streaming. Called by LocalWorker under the turn lock.
	void SendPromptNotice(const std::string& user_text);

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
	// Chat ID of the turn currently running. Set by ProcessUpdate.
	std::atomic<int64_t>         fActiveChatId { 0 };
	bool                         fAllowDestructive = false;
	// Session-scoped always-allow set. Shared between TelegramSink
	// instances (one per turn) so "allow always" persists across turns.
	std::unordered_set<std::string> fAllowedTools;
	std::function<config::Auth()> fAuthGetter;
	std::string                  fCustomSystem;
	std::string                  fCfgModel;
	int                          fCfgMaxTokens;
	std::function<nlohmann::json()> fSharedHistory;
	std::function<void(nlohmann::json, nlohmann::json)> fSharedHistoryAppend;
	std::map<int64_t, nlohmann::json> fUserMessages;
	std::atomic<bool>            fRunning { false };
	std::thread                  fPoller;
	// Turn-token: serialises local and Telegram-origin turns.
	std::mutex                   fTurnMu;
	std::condition_variable      fTurnCv;
	bool                         fTurnInProgress = false;
	// Worker thread for Telegram-origin turns.
	std::deque<Update>           fWorkQueue;
	std::mutex                   fWorkMu;
	std::condition_variable      fWorkCv;
	std::atomic<bool>            fWorkerRunning { false };
	std::thread                  fWorker;
	// Shared permission callback queue. PollLoop pushes perm:* taps
	// here; TelegramSink::AskPermission drains it.
	TelegramSink::PermQueue      fPermQueue;
};

} // namespace telegram

#endif
