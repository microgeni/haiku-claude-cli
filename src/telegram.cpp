#include "telegram.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "api.h"
#include "commands.h"
#include "models.h"
#include "tools.h"
#include "tui.h"

namespace telegram {

using json = nlohmann::json;

namespace {

size_t append_cb(void* ptr, size_t size, size_t nmemb, void* userp) {
	auto* out = static_cast<std::string*>(userp);
	out->append(static_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}

// curl progress / transfer callback used to abort an in-flight
// long-poll when the caller flips its keep_running flag to false.
// curl invokes it roughly once per second; returning non-zero
// causes curl_easy_perform to fail with CURLE_ABORTED_BY_CALLBACK.
int cancel_cb(void* clientp,
			  curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
			  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
	auto* flag = static_cast<std::atomic<bool>*>(clientp);
	return (flag && !flag->load()) ? 1 : 0;
}

} // namespace

Client::Client(std::string bot_token) : fToken(std::move(bot_token)) {}

std::string Client::ApiUrl(const std::string& method) const {
	return "https://api.telegram.org/bot" + fToken + "/" + method;
}

bool Client::PostJson(const std::string& method, const std::string& body,
					   std::string* out_response, long timeout_sec,
					   std::atomic<bool>* keep_running) {
	CURL* curl = curl_easy_init();
	if (!curl) return false;

	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "content-type: application/json");

	std::string sink;
	if (!out_response) out_response = &sink;

	curl_easy_setopt(curl, CURLOPT_URL, ApiUrl(method).c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli");

	if (keep_running) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_cb);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, keep_running);
	}

	const CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) return false;
	if (http_code < 200 || http_code >= 300) return false;
	return true;
}

std::vector<Update> Client::poll(int timeout_sec, std::atomic<bool>* keep_running) {
	const json body = {
		{"offset",          fNextOffset},
		{"timeout",         timeout_sec},
		{"fAllowedupdates", json::array({"message", "callback_query"})},
	};

	std::string response;
	if (!PostJson("getUpdates", body.dump(), &response,
				   static_cast<long>(timeout_sec + 10),
				   keep_running)) {
		return {};
	}

	std::vector<Update> out;
	try {
		const json j = json::parse(response);
		if (!j.value("ok", false)) return out;
		if (!j.contains("result") || !j["result"].is_array()) return out;

		for (const auto& entry : j["result"]) {
			const int64_t id = entry.value("update_id", int64_t{0});
			if (id >= fNextOffset) fNextOffset = id + 1;

			// Button tap from a previous inline keyboard.
			if (entry.contains("callback_query")) {
				const auto& cb = entry["callback_query"];
				Update u;
				u.update_id        = id;
				u.is_callback      = true;
				u.callback_query_id = cb.value("id", std::string{});
				u.text             = cb.value("data", std::string{});
				if (cb.contains("from")) {
					u.user_id  = cb["from"].value("id", int64_t{0});
					u.username = cb["from"].value("username", std::string{});
				}
				if (cb.contains("message") && cb["message"].contains("chat")) {
					u.chat_id = cb["message"]["chat"].value("id", int64_t{0});
				}
				out.push_back(std::move(u));
				continue;
			}

			if (!entry.contains("message")) continue;
			const auto& msg = entry["message"];
			if (!msg.contains("text")) continue; // skip non-text (photos, etc.)

			Update u;
			u.update_id = id;
			if (msg.contains("chat") && msg["chat"].contains("id")) {
				u.chat_id = msg["chat"]["id"].get<int64_t>();
			}
			if (msg.contains("from")) {
				u.user_id = msg["from"].value("id", int64_t{0});
				u.username = msg["from"].value("username", std::string{});
			}
			u.text = msg.value("text", std::string{});
			out.push_back(std::move(u));
		}
	} catch (const json::exception& e) {
		std::cerr << "telegram: failed to parse getUpdates response: "
				  << e.what() << "\n";
		return {};
	}
	return out;
}

bool Client::SendMessage(int64_t chat_id, const std::string& text,
						  const std::vector<std::vector<Button>>& keyboard) {
	// Build the reply_markup once; it's only attached to the final
	// chunk so the buttons render under the last visible piece.
	json reply_markup;
	if (!keyboard.empty()) {
		json rows = json::array();
		for (const auto& row : keyboard) {
			json cols = json::array();
			for (const auto& btn : row) {
				cols.push_back({
					{"text",          btn.text},
					{"callback_data", btn.callback_data},
				});
			}
			rows.push_back(cols);
		}
		reply_markup = {{"inline_keyboard", rows}};
	}

	const std::string effective = text.empty() ? std::string{"(empty)"} : text;
	constexpr size_t  kChunk    = 3800;
	size_t            i         = 0;
	bool              all_ok    = true;

	while (i < effective.size()) {
		const std::string piece = effective.substr(i, kChunk);
		i += kChunk;
		const bool last = (i >= effective.size());

		json body = {
			{"chat_id", chat_id},
			{"text",    piece},
		};
		if (last && !reply_markup.is_null()) {
			body["reply_markup"] = reply_markup;
		}
		if (!PostJson("sendMessage",
		              body.dump(-1, ' ', false, json::error_handler_t::replace),
		              nullptr, 15)) {
			all_ok = false;
			break;
		}
	}
	return all_ok;
}

int64_t Client::SendMessageWithId(int64_t chat_id, const std::string& text,
									 const std::vector<std::vector<Button>>& keyboard) {
	// Only the first chunk's message_id is returned; subsequent
	// chunks (if any) aren't tracked by callers that want to edit.
	const std::string effective = text.empty() ? std::string{"(empty)"} : text;
	const size_t      first_len = std::min<size_t>(effective.size(), 3800);
	const std::string first     = effective.substr(0, first_len);
	const bool        only_chunk = (first_len == effective.size());

	json body = {
		{"chat_id", chat_id},
		{"text",    first},
	};
	if (only_chunk && !keyboard.empty()) {
		json rows = json::array();
		for (const auto& row : keyboard) {
			json cols = json::array();
			for (const auto& btn : row) {
				cols.push_back({{"text", btn.text}, {"callback_data", btn.callback_data}});
			}
			rows.push_back(cols);
		}
		body["reply_markup"] = {{"inline_keyboard", rows}};
	}

	std::string response;
	if (!PostJson("sendMessage",
	              body.dump(-1, ' ', false, json::error_handler_t::replace),
	              &response, 15)) return 0;

	int64_t message_id = 0;
	try {
		const json j = json::parse(response);
		if (j.value("ok", false) && j.contains("result")) {
			message_id = j["result"].value("message_id", int64_t{0});
		}
	} catch (const json::exception&) {}

	// Send any remaining chunks as follow-up messages (no message_id
	// returned — caller only edits the first chunk).
	if (!only_chunk) {
		SendMessage(chat_id, effective.substr(first_len), keyboard);
	}
	return message_id;
}

bool Client::EditMessageText(int64_t chat_id, int64_t message_id,
							   const std::string& text,
							   const std::vector<std::vector<Button>>& keyboard) {
	if (message_id == 0) return false;
	// Telegram caps edited text at 4096. Truncate with a marker so
	// we always stay under the cap.
	std::string body_text = text;
	constexpr size_t kCap = 3800;
	if (body_text.size() > kCap) {
		body_text.resize(kCap);
		body_text += "\n\n[... truncated]";
	}

	json body = {
		{"chat_id",    chat_id},
		{"message_id", message_id},
		{"text",       body_text},
	};
	if (!keyboard.empty()) {
		json rows = json::array();
		for (const auto& row : keyboard) {
			json cols = json::array();
			for (const auto& btn : row) {
				cols.push_back({{"text", btn.text}, {"callback_data", btn.callback_data}});
			}
			rows.push_back(cols);
		}
		body["reply_markup"] = {{"inline_keyboard", rows}};
	}

	std::string response;
	const bool ok = PostJson("editMessageText",
	                         body.dump(-1, ' ', false, json::error_handler_t::replace),
	                         &response, 10);
	// Distinguish "message is not modified" (harmless, treat as ok)
	// from real failures (network error, rate-limit, wrong message_id)
	// so callers can fall back to sendMessage when the edit fails.
	if (!ok) {
		// Check if Telegram rejected with "message is not modified" —
		// that's cosmetically fine and should not trigger a fallback.
		try {
			const json j = nlohmann::json::parse(response);
			if (!j.value("ok", true)) {
				const std::string desc = j.value("description", std::string{});
				if (desc.find("message is not modified") != std::string::npos) {
					return true; // not actually an error
				}
			}
		} catch (...) {}
		return false;
	}
	return true;
}

bool Client::SendChatAction(int64_t chat_id, const std::string& action) {
	const json body = {
		{"chat_id", chat_id},
		{"action",  action},
	};
	return PostJson("sendChatAction", body.dump(), nullptr, 5);
}

bool Client::AnswerCallback(const std::string& callback_query_id,
							 const std::string& notice) {
	json body = {
		{"callback_query_id", callback_query_id},
	};
	if (!notice.empty()) body["text"] = notice;
	return PostJson("answerCallbackQuery", body.dump(), nullptr, 10);
}

bool Client::DeleteMessage(int64_t chat_id, int64_t message_id) {
	const json body = {
		{"chat_id",    chat_id},
		{"message_id", message_id},
	};
	return PostJson("deleteMessage", body.dump(), nullptr, 10);
}

std::atomic<bool> g_muted { false };

// ── RemoteControl ─────────────────────────────────────────────

bool RemoteControl::ConfigIsValid(const config::Config& cfg, std::string* reason) {
	if (!cfg.telegram.is_object()) {
		if (reason) *reason = "config.telegram is missing from config.json";
		return false;
	}
	if (cfg.telegram.value("bot_token", std::string{}).empty()) {
		if (reason) *reason = "config.telegram.bot_token is not set";
		return false;
	}
	if (!cfg.telegram.contains("allowed_user_ids")
		|| !cfg.telegram["allowed_user_ids"].is_array()
		|| cfg.telegram["allowed_user_ids"].empty()) {
		if (reason) *reason = "config.telegram.allowed_user_ids must list at least one Telegram user ID";
		return false;
	}
	return true;
}

RemoteControl::RemoteControl(const config::Config& cfg, const config::Auth& auth,
                              const std::string& custom_system)
	: fClient(cfg.telegram.value("bot_token", std::string{})),
	  fAuth(auth),
	  fCustomSystem(custom_system),
	  fCfgModel(cfg.model),
	  fCfgMaxTokens(cfg.max_tokens) {
	for (const auto& v : cfg.telegram["allowed_user_ids"]) {
		if (v.is_number_integer()) fAllowed.insert(v.get<int64_t>());
	}
	fAllowDestructive = cfg.telegram.value("fAllowDestructiveTools",
		cfg.telegram.value("fAllowDestructivetools", false));
}

RemoteControl::~RemoteControl() { Stop(); }

bool RemoteControl::Running() const { return fRunning.load(); }

bool RemoteControl::Start() {
	if (fRunning.load()) return false;
	// Determine the primary user for local mirroring —
	// smallest allowed user_id.
	fPrimaryUserId = 0;
	for (const auto& id : fAllowed) {
		if (fPrimaryUserId == 0 || id < fPrimaryUserId) fPrimaryUserId = id;
	}
	// Default the active perm chat to the primary user so that a
	// locally-initiated turn can send permission prompts to Telegram
	// before any Telegram message has arrived.
	fActiveChatId = fPrimaryUserId;
	// Clear the always-allowed tool set so approvals from a previous
	// session don't silently carry over.
	api::AlwaysAllowed().clear();
	fRunning.store(true);
	fWorkerRunning.store(true);
	fWorker = std::thread(&RemoteControl::WorkLoop, this);
	fPoller = std::thread(&RemoteControl::PollLoop, this);

	// Install global hooks once for the lifetime of this
	// RemoteControl session. Both local REPL turns and Telegram-
	// origin turns will share these hooks; fActiveChatId is
	// updated by ProcessUpdate() before each Telegram-origin
	// SendWithTools call, and reset to fPrimaryUserId for local
	// turns so permission prompts always reach the right chat.
	api::g_telegram_permission_hook = [this](const std::string& tool_name,
	                                          const std::string& preview,
	                                          std::atomic<bool>* local_answered) -> api::Permission {
		const int64_t chat = fActiveChatId.load();
		if (chat == 0) return api::Permission::Deny;
		const std::string question =
			"\xF0\x9F\x94\x90 allow " + tool_name + "?\n\n" + preview;
		const std::vector<std::vector<Button>> kb = {
			{{ "1. Yes, allow once",          "perm:yes"    }},
			{{ "2. Always allow this session", "perm:always" }},
			{{ "3. No, deny",                 "perm:no"     }},
		};
		api::g_telegram_updater_paused.store(true);
		const int64_t perm_msg_id = fClient.SendMessageWithId(chat, question, kb);
		while (!g_interrupted) {
			if (local_answered && local_answered->load()) {
				api::g_telegram_updater_paused.store(false);
				return api::Permission::Deny; // result ignored by the race winner path
			}
			std::unique_lock<std::mutex> lk(fPermMu);
			fPermCv.wait_for(lk, std::chrono::seconds(2),
				[&]{ return !fPermQueue.empty() || g_interrupted
					|| (local_answered && local_answered->load()); });
			if (local_answered && local_answered->load()) {
				api::g_telegram_updater_paused.store(false);
				return api::Permission::Deny;
			}
			while (!fPermQueue.empty()) {
				const Update upd = fPermQueue.front();
				fPermQueue.pop_front();
				lk.unlock();
				api::g_telegram_updater_paused.store(false);
				fClient.AnswerCallback(upd.callback_query_id);
				if (upd.text == "perm:always") {
					api::AlwaysAllowed().insert(tool_name);
					fClient.EditMessageText(chat, perm_msg_id,
						"\xF0\x9F\x94\x90 allow " + tool_name
						+ "? \xE2\x86\x92 \xE2\x9C\x85 always allowed this session", {});
					return api::Permission::Allow;
				}
				if (upd.text == "perm:yes") {
					fClient.EditMessageText(chat, perm_msg_id,
						"\xF0\x9F\x94\x90 allow " + tool_name
						+ "? \xE2\x86\x92 \xE2\x9C\x85 allowed once", {});
					return api::Permission::Allow;
				}
				if (upd.text == "perm:no") {
					fClient.EditMessageText(chat, perm_msg_id,
						"\xF0\x9F\x94\x90 allow " + tool_name
						+ "? \xE2\x86\x92 \xF0\x9F\x9A\xAB denied", {});
					return api::Permission::Deny;
				}
				lk.lock();
			}
		}
		api::g_telegram_updater_paused.store(false);
		return api::Permission::Deny;
	};

	api::g_tool_status_hook = [this](const std::string& notice) {
		const int64_t chat = fActiveChatId.load();
		if (chat != 0 && !g_muted.load())
			fClient.SendMessage(chat, notice);
	};

	return true;
}

void RemoteControl::Stop() {
	if (!fRunning.exchange(false)) return;
	api::g_telegram_permission_hook = nullptr;
	api::g_tool_status_hook         = nullptr;
	// Wake any thread blocked in AcquireTurn() so it sees
	// fRunning == false and unblocks cleanly.
	fTurnCv.notify_all();
	if (fPoller.joinable()) fPoller.join();
	// Stop the worker thread so it doesn't dangle after the
	// poller exits.
	fWorkerRunning.store(false);
	fWorkCv.notify_one();
	if (fWorker.joinable()) fWorker.join();
}

void RemoteControl::AcquireTurn() {
	std::unique_lock<std::mutex> lk(fTurnMu);
	fTurnCv.wait(lk, [this]{ return !fTurnInProgress || !fRunning.load(); });
	fTurnInProgress = true;
}

void RemoteControl::ReleaseTurn() {
	{
		std::lock_guard<std::mutex> lk(fTurnMu);
		fTurnInProgress = false;
	}
	fTurnCv.notify_all();
}

// Send the user's local prompt to the primary Telegram chat
// immediately — before Claude starts working — so the phone side
// sees what is being asked in real time.
void RemoteControl::MirrorPrompt(const std::string& user_text) {
	if (!fRunning.load()) return;
	if (fPrimaryUserId == 0) return;
	if (g_muted.load()) return;
	fClient.SendMessage(fPrimaryUserId, "> " + user_text);
	fPrimaryThinkingMsgId = fClient.SendMessageWithId(
		fPrimaryUserId,
		"\xE2\x8F\xB3 thinking\xE2\x80\xA6"); // ⏳ thinking…
	StartThinkingUpdater();
}

void RemoteControl::MirrorToPrimary(const std::string& assistant_text) {
	StopThinkingUpdater();
	if (!fRunning.load()) return;
	if (fPrimaryUserId == 0) return;
	if (g_muted.load()) {
		fPrimaryThinkingMsgId = 0;
		return;
	}
	const int64_t ph = fPrimaryThinkingMsgId;
	fPrimaryThinkingMsgId = 0;
	if (!assistant_text.empty()) {
		if (ph != 0) {
			if (!fClient.EditMessageText(fPrimaryUserId, ph, assistant_text))
				fClient.SendMessage(fPrimaryUserId, assistant_text);
		} else {
			fClient.SendMessage(fPrimaryUserId, assistant_text);
		}
	} else if (ph != 0) {
		fClient.EditMessageText(fPrimaryUserId, ph, "(no response)");
	}
}

void RemoteControl::MirrorCancel() {
	StopThinkingUpdater();
	if (fPrimaryUserId == 0 || fPrimaryThinkingMsgId == 0) return;
	fClient.EditMessageText(fPrimaryUserId, fPrimaryThinkingMsgId,
							"\xE2\x9D\x8C error \xE2\x80\x94 turn aborted"); // ❌ error — turn aborted
	fPrimaryThinkingMsgId = 0;
}

void RemoteControl::StartThinkingUpdater() {
	if (fPrimaryThinkingMsgId == 0) return;
	fUpdaterRunning.store(true);
	fUpdaterThread = std::thread([this]() {
		int  dot_phase    = 0;
		int  last_version = 0;
		static const char* kDots[] = {
			"\xE2\x8F\xB3 thinking\xE2\x80\xA6",                      // ⏳ thinking…
			"\xE2\x8F\xB3 thinking\xE2\x80\xA4",                      // ⏳ thinking.
			"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4",          // ⏳ thinking..
			"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4\xE2\x80\xA4", // ⏳ thinking...
		};
		while (fUpdaterRunning.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1200));
			if (!fUpdaterRunning.load()) break;
			if (api::g_telegram_updater_paused.load()) continue;
			const int64_t ph = fPrimaryThinkingMsgId;
			if (ph == 0) break;
			if (api::g_stream_progress) {
				const int v = api::g_stream_progress->version.load(
					std::memory_order_relaxed);
				if (v != last_version) {
					last_version = v;
					std::string snap;
					{
						std::lock_guard<std::mutex> lk(api::g_stream_progress->mu);
						snap = api::g_stream_progress->text;
					}
					if (!snap.empty()) {
						fClient.EditMessageText(fPrimaryUserId, ph,
							snap + " \xE2\x96\x8C"); // ▌ streaming cursor
						continue;
					}
				}
			}
			fClient.EditMessageText(fPrimaryUserId, ph,
				kDots[dot_phase % 4]);
			++dot_phase;
		}
	});
}

void RemoteControl::StopThinkingUpdater() {
	fUpdaterRunning.store(false);
	if (fUpdaterThread.joinable()) fUpdaterThread.join();
}

void RemoteControl::PollLoop() {
	while (fRunning.load() && !g_interrupted) {
		const auto updates = fClient.poll(10, &fRunning);
		if (!fRunning.load() || g_interrupted) break;
		for (const auto& u : updates) {
			if (!fRunning.load() || g_interrupted) break;
			if (!fAllowed.count(u.user_id)) {
				config::LogLine("remote-control reject user=" + std::to_string(u.user_id));
				continue;
			}
			// Route perm:* button taps to the permission queue so
			// the hook's wait loop sees them.
			if (u.is_callback && u.text.rfind("perm:", 0) == 0) {
				{
					std::lock_guard<std::mutex> lk(fPermMu);
					fPermQueue.push_back(u);
				}
				fPermCv.notify_one();
				continue;
			}
			{
				std::lock_guard<std::mutex> lk(fWorkMu);
				fWorkQueue.push_back(u);
			}
			fWorkCv.notify_one();
		}
	}
}

// Handle slash commands that do NOT require a Claude turn — i.e. every
// slash command that ProcessUpdate handles before the final "fall through
// to SendWithTools" block.  Returns true when the command was fully
// serviced here so the caller can skip AcquireTurn() / ProcessUpdate().
// Returns false for plain prompts or passthrough-to-Claude commands so
// the caller must still go through the normal (blocking) path.
bool RemoteControl::TryHandleSlashImmediate(const Update& u_in) {
	Update u = u_in;
	if (u.is_callback) {
		// Callback taps that reach WorkLoop are never slash commands.
		return false;
	}

	const std::string who = u.username.empty()
		? std::to_string(u.user_id) : u.username;

	// Mirror the incoming command to the local terminal (same as
	// ProcessUpdate does for every message).
	std::cout << "\x1b""7";
	tui::PositionCursorForChat();
	std::cout << tui::Meta("[remote " + who + "] " + u.text) << "\n";
	config::LogLine("remote-control rx user=" + std::to_string(u.user_id)
			 + " text=" + u.text);

	if (u.text == "/mute") {
		if (!g_muted.exchange(true))
			fClient.SendMessage(u.chat_id,
				"Remote muted. No replies until /unmute.");
		std::cout << "\x1b""8" << std::flush;
		return true;
	}
	if (u.text == "/unmute") {
		if (g_muted.exchange(false))
			fClient.SendMessage(u.chat_id,
				"Remote unmuted. Replies will be sent again.");
		std::cout << "\x1b""8" << std::flush;
		return true;
	}
	if (u.text == "/new") {
		fUserMessages.erase(u.user_id);
		TgSend(u.chat_id, "(history cleared)");
		std::cout << "\x1b""8" << std::flush;
		return true;
	}

	if (u.text.empty() || u.text.front() != '/') {
		// Plain prompt — must go through AcquireTurn.
		return false;
	}

	const std::string cmd_word = u.text.substr(0, u.text.find(' '));
	if (cmd_word == "/exit" || cmd_word == "/quit"
			|| cmd_word == "/remote-control") {
		TgSend(u.chat_id, "(" + cmd_word + " is not available from Telegram)");
		std::cout << "\x1b""8" << std::flush;
		return true;
	}

	// All other slash commands: dispatch through commands::Dispatch.
	// If the result is Passthrough the command resolves to a plain prompt
	// and must still be handled by ProcessUpdate / AcquireTurn.
	const std::string dispatched = (u.text == "/start") ? "/help" : u.text;

	std::ostringstream capture;
	std::streambuf* old_buf = std::cout.rdbuf(capture.rdbuf());

	json& messages_ref = fUserMessages[u.user_id];
	if (!messages_ref.is_array()) messages_ref = json::array();
	int    rc_turn   = 0;
	int    rc_in     = 0;
	int    rc_out    = 0;
	bool   rc_notify = false;
	double rc_thresh = 60.0;
	std::vector<std::string> rc_urls;
	json   rc_prices = json::object();
	commands::LoopCtx ctx{fAuth, fCfgMaxTokens, fCustomSystem, rc_prices,
	                      fCfgModel, rc_turn, rc_in, rc_out,
	                      messages_ref, rc_urls, rc_notify, rc_thresh,
	                      {}};
	std::string passthrough;
	const commands::SlashAction action = commands::Dispatch(dispatched, ctx, passthrough);

	std::cout.rdbuf(old_buf);
	const std::string output = capture.str();

	if (action == commands::SlashAction::Passthrough) {
		// The command wants Claude to handle it — let WorkLoop proceed
		// through AcquireTurn.  ProcessUpdate will re-run the full
		// dispatch (including the passthrough rewrite) so we do NOT
		// consume the update here; just tell the caller it needs the
		// turn lock.
		return false;
	}

	// Strip ANSI before sending to Telegram.
	std::string plain;
	plain.reserve(output.size());
	for (size_t i = 0; i < output.size(); ) {
		if (output[i] == '\x1b' && i + 1 < output.size() && output[i+1] == '[') {
			i += 2;
			while (i < output.size()
					&& output[i] != 'm' && output[i] != 'K'
					&& output[i] != 'H' && output[i] != 'A'
					&& output[i] != 'B' && output[i] != 'J') ++i;
			if (i < output.size()) ++i;
		} else {
			plain += output[i++];
		}
	}
	while (!plain.empty() && (plain.front() == '\n' || plain.front() == ' '))
		plain.erase(plain.begin());
	while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' '))
		plain.pop_back();

	if (!plain.empty()) TgSend(u.chat_id, plain);
	std::cout << "\x1b""8" << std::flush;
	return true;
}

void RemoteControl::WorkLoop() {
	while (fWorkerRunning.load()) {
		Update job;
		{
			std::unique_lock<std::mutex> lk(fWorkMu);
			fWorkCv.wait(lk, [&]{
				return !fWorkQueue.empty() || !fWorkerRunning.load();
			});
			if (!fWorkerRunning.load()) break;
			job = fWorkQueue.front();
			fWorkQueue.pop_front();
		}
		// Slash commands that don't invoke Claude are handled here
		// immediately — without waiting for AcquireTurn() — so the
		// Telegram user can always use /help, /mute, /new, etc. even
		// while a long Claude turn is in progress locally or via a
		// prior Telegram message.
		if (TryHandleSlashImmediate(job)) continue;
		AcquireTurn();
		ProcessUpdate(job);
		ReleaseTurn();
	}
}

void RemoteControl::TgSend(int64_t chat, const std::string& text) {
	if (g_muted.load()) return;
	fClient.SendMessage(chat, text);
}

void RemoteControl::ProcessUpdate(const Update& u_in) {
	// Copy so we can rewrite u.text on Passthrough below.
	Update u = u_in;
	if (u.is_callback) fClient.AnswerCallback(u.callback_query_id);

	const std::string who = u.username.empty()
		? std::to_string(u.user_id) : u.username;

	// The poller thread runs while libedit has the cursor parked on
	// the fixed input row. Route all our stdout writes into the
	// scroll region via save/restore so we don't clobber the prompt.
	std::cout << "\x1b""7";          // save cursor
	tui::PositionCursorForChat();
	std::cout << tui::Meta("[remote " + who + "] " + u.text) << "\n";
	config::LogLine("remote-control rx user=" + std::to_string(u.user_id)
			 + " text=" + u.text);

	if (u.text == "/mute") {
		if (!g_muted.exchange(true)) {
			fClient.SendMessage(u.chat_id,
				"Remote muted. No replies until /unmute.");
		}
		std::cout << "\x1b""8" << std::flush;
		return;
	}
	if (u.text == "/unmute") {
		if (g_muted.exchange(false)) {
			fClient.SendMessage(u.chat_id,
				"Remote unmuted. Replies will be sent again.");
		}
		std::cout << "\x1b""8" << std::flush;
		return;
	}
	// /new is a Telegram-friendly alias for /clear.
	if (u.text == "/new") {
		fUserMessages.erase(u.user_id);
		TgSend(u.chat_id, "(history cleared)");
		std::cout << "\x1b""8" << std::flush;
		return;
	}

	// All other slash commands go through commands::Dispatch.
	// /exit, /quit, and /remote-control are not meaningful here.
	if (!u.text.empty() && u.text.front() == '/') {
		const std::string cmd_word = u.text.substr(0, u.text.find(' '));
		if (cmd_word == "/exit" || cmd_word == "/quit"
				|| cmd_word == "/remote-control") {
			TgSend(u.chat_id, "(" + cmd_word + " is not available from Telegram)");
			std::cout << "\x1b""8" << std::flush;
			return;
		}
		const std::string dispatched = (u.text == "/start") ? "/help" : u.text;

		std::ostringstream capture;
		std::streambuf* old_buf = std::cout.rdbuf(capture.rdbuf());

		json& messages_ref = fUserMessages[u.user_id];
		if (!messages_ref.is_array()) messages_ref = json::array();
		// Throwaway locals for the LoopCtx fields that Dispatch may
		// update (/model, /compact, etc.) — RemoteControl doesn't
		// track session totals per user.
		int    rc_turn   = 0;
		int    rc_in     = 0;
		int    rc_out    = 0;
		bool   rc_notify = false;
		double rc_thresh = 60.0;
		std::vector<std::string> rc_urls;
		json   rc_prices = json::object();
		commands::LoopCtx ctx{fAuth, fCfgMaxTokens, fCustomSystem, rc_prices,
		                      fCfgModel, rc_turn, rc_in, rc_out,
		                      messages_ref, rc_urls, rc_notify, rc_thresh,
		                      {}};
		std::string passthrough;
		const commands::SlashAction action = commands::Dispatch(dispatched, ctx, passthrough);

		std::cout.rdbuf(old_buf);
		const std::string output = capture.str();

		// Strip ANSI escape sequences before sending to Telegram.
		std::string plain;
		plain.reserve(output.size());
		for (size_t i = 0; i < output.size(); ) {
			if (output[i] == '\x1b' && i + 1 < output.size() && output[i+1] == '[') {
				i += 2;
				while (i < output.size()
						&& output[i] != 'm' && output[i] != 'K'
						&& output[i] != 'H' && output[i] != 'A'
						&& output[i] != 'B' && output[i] != 'J') ++i;
				if (i < output.size()) ++i;
			} else {
				plain += output[i++];
			}
		}
		while (!plain.empty() && (plain.front() == '\n' || plain.front() == ' '))
			plain.erase(plain.begin());
		while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' '))
			plain.pop_back();

		if (action == commands::SlashAction::Passthrough) {
			u.text = passthrough;
			// Fall through to normal prompt handling below.
		} else {
			if (!plain.empty()) TgSend(u.chat_id, plain);
			std::cout << "\x1b""8" << std::flush;
			return;
		}
	}

	json& msgs = fUserMessages[u.user_id];
	if (!msgs.is_array()) msgs = json::array();

	fActiveChatId.store(u.chat_id);

	// Post a placeholder and start a streaming-edit updater thread.
	const int64_t placeholder_id = [&]() -> int64_t {
		if (g_muted.load()) return 0;
		return fClient.SendMessageWithId(u.chat_id, "\xE2\x80\xA6"); // …
	}();

	api::StreamProgress progress;
	api::g_stream_progress = &progress;
	std::atomic<bool> updater_running { true };
	int               last_version = 0;
	int               dot_phase    = 0;
	std::thread updater([&]() {
		while (updater_running.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			if (!updater_running.load()) break;
			if (api::g_telegram_updater_paused.load()) continue;
			if (placeholder_id == 0) continue;

			// While a tool is actively running, show its name.
			std::string tool_ph;
			{
				std::lock_guard<std::mutex> lk(progress.mu);
				tool_ph = progress.tool_phase;
			}
			if (!tool_ph.empty()) {
				fClient.EditMessageText(u.chat_id, placeholder_id, tool_ph);
				continue;
			}

			const int v = progress.version.load(std::memory_order_relaxed);
			if (v == last_version) {
				static const char* kDots[] = {
					"\xE2\x8F\xB3 thinking\xE2\x80\xA6",
					"\xE2\x8F\xB3 thinking\xE2\x80\xA4",
					"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4",
					"\xE2\x8F\xB3 thinking\xE2\x80\xA4\xE2\x80\xA4\xE2\x80\xA4",
				};
				fClient.EditMessageText(u.chat_id, placeholder_id,
										kDots[dot_phase % 4]);
				++dot_phase;
			} else {
				last_version = v;
				std::string snap;
				{ std::lock_guard<std::mutex> lk(progress.mu); snap = progress.text; }
				if (!snap.empty())
					fClient.EditMessageText(u.chat_id, placeholder_id,
											snap + " \xE2\x96\x8C"); // ▌
			}
		}
	});

	const json snapshot = msgs;
	msgs.push_back({{"role", "user"}, {"content", u.text}});

	api::g_non_interactive_tools             = true;
	api::g_non_interactive_allow_destructive = fAllowDestructive;

	std::cout << tui::ClaudePrompt();
	const std::string effective_system = config::ComposeSystem(fCustomSystem);
	const auto result = api::SendWithTools(fAuth, fCfgModel, fCfgMaxTokens,
										    msgs, effective_system);
	std::cout << "\n";
	api::g_non_interactive_tools = false;
	// Restore active chat to primary so any local-turn permission
	// prompts (after this Telegram turn) go to the right place.
	fActiveChatId.store(fPrimaryUserId);

	updater_running.store(false);
	if (updater.joinable()) updater.join();
	api::g_stream_progress = nullptr;

	if (result.exit_code != 0 || result.assistant_text.empty()) {
		msgs = snapshot;
		const std::string err = "(error: Claude did not return a response)";
		if (placeholder_id)
			fClient.EditMessageText(u.chat_id, placeholder_id, err);
		else if (!g_muted.load())
			fClient.SendMessage(u.chat_id, err);
		config::LogLine("remote-control tx user=" + std::to_string(u.user_id) + " -> error");
		std::cout << "\x1b""8" << std::flush;
		return;
	}

	// Numbered options → inline keyboard buttons.
	std::vector<std::vector<Button>> keyboard;
	const auto options = models::ExtractNumberedOptions(result.assistant_text);
	for (const auto& opt : options) {
		Button b;
		b.text          = opt.first + ". " + opt.second;
		b.callback_data = opt.first;
		keyboard.push_back({ std::move(b) });
	}

	if (!g_muted.load()) {
		if (placeholder_id) {
			if (!fClient.EditMessageText(u.chat_id, placeholder_id,
											result.assistant_text, keyboard)) {
				fClient.SendMessage(u.chat_id, result.assistant_text, keyboard);
			}
		} else {
			fClient.SendMessage(u.chat_id, result.assistant_text, keyboard);
		}
	}
	config::LogLine("remote-control tx user=" + std::to_string(u.user_id)
			 + " out=" + std::to_string(result.output_tokens));
	std::cout << "\x1b""8" << std::flush;
}

} // namespace telegram
