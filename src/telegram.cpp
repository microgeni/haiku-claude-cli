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

// ── TelegramSink ────────────────────────────────────────────────────────────

int64_t TelegramSink::NowMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}

TelegramSink::TelegramSink(Client& client, int64_t chatId,
                            bool allowDestructive,
                            std::unordered_set<std::string>& allowedSet,
                            PermQueue& permQueue)
	: fClient(client)
	, fChatId(chatId)
	, fAllowDestructive(allowDestructive)
	, fAllowedSet(allowedSet)
	, fPermQueue(permQueue)
{
}

int64_t TelegramSink::SentPlaceholder(const std::string& text) {
	return fClient.SendMessageWithId(fChatId, text);
}

// Build the full display text: assistant text followed by any
// collapsed tool cards.
std::string TelegramSink::BuildDisplayText() const {
	std::string out = fBuffer;
	for (const auto& card : fToolCards) {
		if (!card.started) continue;
		if (!out.empty() && out.back() != '\n') out += '\n';
		if (!card.finished) {
			// Tool still running.
			out += "\xF0\x9F\x94\xA7 " + card.name  // 🔧
			    + ": " + card.summary + "\xE2\x80\xA6"; // …
		} else {
			out += (card.ok ? "\xE2\x9C\x85" : "\xE2\x9C\x8B") // ✅ / ✋
			    + std::string(" ") + card.name + " "
			    + (card.ok ? "\xE2\x9C\x93" : "\xE2\x9C\x97"); // ✓ / ✗
		}
	}
	return out;
}

bool TelegramSink::EditCurrent(bool final) {
	if (fCurrentMsgId == 0) return false;
	const int64_t now = NowMs();
	if (!final && (now - fLastEditMs) < kEditThrottleMs) return true; // throttled
	const std::string text = BuildDisplayText();
	const std::string display = (!final && fInMessage)
		? text + " \xE2\x96\x8C"   // append streaming cursor ▌
		: text;
	const bool ok = fClient.EditMessageText(fChatId, fCurrentMsgId, display);
	if (ok) fLastEditMs = now;
	return ok;
}

// ── StructuredSink methods ───────────────────────────────────────────────────

void TelegramSink::BeginMessage(const std::string& /*role*/) {
	fBuffer.clear();
	fToolCards.clear();
	fInMessage = true;
	fCurrentMsgId = SentPlaceholder("\xE2\x8F\xB3 thinking\xE2\x80\xA6"); // ⏳ thinking…
	fLastEditMs   = NowMs();
}

void TelegramSink::AppendText(const std::string& chunk) {
	if (!fInMessage) return;
	fBuffer += chunk;
	EditCurrent(/*final=*/false);
}

void TelegramSink::EndMessage() {
	if (!fInMessage) return;
	fInMessage = false;
	EditCurrent(/*final=*/true);
	fCurrentMsgId = 0;
}

void TelegramSink::ToolStarted(const std::string& name,
                                const std::string& summary) {
	ToolCard card;
	card.name    = name;
	card.summary = summary;
	card.started = true;
	fToolCards.push_back(std::move(card));
	EditCurrent(/*final=*/false); // show "🔧 bash: …" immediately
}

void TelegramSink::ToolFinished(const std::string& name,
                                 bool ok,
                                 const std::string& detail) {
	for (auto& card : fToolCards) {
		if (card.name == name && card.started && !card.finished) {
			card.finished = true;
			card.ok       = ok;
			(void)detail; // stored implicitly; not displayed by default
			break;
		}
	}
	EditCurrent(/*final=*/false); // update ✅ / ✋
}

int TelegramSink::AskChoice(const std::string& prompt,
                             const std::vector<std::string>& options) {
	if (options.empty()) return -1;

	// Build an inline keyboard — one button per option.
	std::vector<std::vector<Button>> kb;
	for (size_t i = 0; i < options.size(); ++i) {
		Button b;
		b.text          = std::to_string(i + 1) + ". " + options[i];
		b.callback_data = "choice:" + std::to_string(i);
		kb.push_back({ std::move(b) });
	}
	const int64_t msg_id = fClient.SendMessageWithId(fChatId, prompt, kb);
	if (msg_id == 0) return -1;

	// Block waiting for a callback tap.
	std::unique_lock<std::mutex> lk(fPermQueue.mu);
	fPermQueue.cv.wait_for(lk, std::chrono::seconds(120),
		[&]{ return !fPermQueue.callbacks.empty(); });
	if (fPermQueue.callbacks.empty()) return -1;
	const std::string cb = fPermQueue.callbacks.front();
	fPermQueue.callbacks.pop_front();
	lk.unlock();

	if (cb.rfind("choice:", 0) == 0) {
		try { return std::stoi(cb.substr(7)); } catch (...) {}
	}
	return -1;
}

sink::Permission TelegramSink::AskPermission(const std::string& tool,
                                              const std::string& preview) {
	// Already session-approved.
	if (fAllowedSet.count(tool)) return sink::Permission::kAllow;

	// Non-interactive mode with blanket allow.
	if (fAllowDestructive) return sink::Permission::kAllow;

	// Send an inline-keyboard permission prompt.
	const std::string question =
		"\xF0\x9F\x94\x90 Allow " + tool + "?\n\n" + preview; // 🔐
	const std::vector<std::vector<Button>> kb = {
		{{ "✅ Yes, once",            "perm:yes"    }},
		{{ "🔓 Yes, always this session", "perm:always" }},
		{{ "🚫 No, deny",             "perm:no"     }},
	};
	const int64_t msg_id = fClient.SendMessageWithId(fChatId, question, kb);

	// Block waiting for a perm:* callback tap.
	std::unique_lock<std::mutex> lk(fPermQueue.mu);
	fPermQueue.cv.wait_for(lk, std::chrono::seconds(120),
		[&]{ return !fPermQueue.callbacks.empty() || g_interrupted != 0; });

	if (fPermQueue.callbacks.empty() || g_interrupted) {
		if (msg_id)
			fClient.EditMessageText(fChatId, msg_id,
				"\xF0\x9F\x94\x90 " + tool + " \xE2\x86\x92 \xF0\x9F\x9A\xAB timed out / cancelled");
		return sink::Permission::kDeny;
	}
	const std::string cb = fPermQueue.callbacks.front();
	fPermQueue.callbacks.pop_front();
	lk.unlock();

	if (cb == "perm:always") {
		fAllowedSet.insert(tool);
		if (msg_id)
			fClient.EditMessageText(fChatId, msg_id,
				"\xF0\x9F\x94\x90 " + tool + " \xE2\x86\x92 \xE2\x9C\x85 always allowed this session");
		return sink::Permission::kAllowAlways;
	}
	if (cb == "perm:yes") {
		if (msg_id)
			fClient.EditMessageText(fChatId, msg_id,
				"\xF0\x9F\x94\x90 " + tool + " \xE2\x86\x92 \xE2\x9C\x85 allowed once");
		return sink::Permission::kAllow;
	}
	// perm:no or anything else.
	if (msg_id)
		fClient.EditMessageText(fChatId, msg_id,
			"\xF0\x9F\x94\x90 " + tool + " \xE2\x86\x92 \xF0\x9F\x9A\xAB denied");
	return sink::Permission::kDeny;
}

void TelegramSink::SetStatus(sink::StatusKind kind) {
	// Sends a transient "typing…" action — never a persistent message.
	if (kind == sink::StatusKind::kIdle) return;
	fClient.SendChatAction(fChatId, "typing");
}

void TelegramSink::OnError(const std::string& message) {
	const std::string text = "\xE2\x9D\x8C Error: " + message; // ❌
	if (fCurrentMsgId != 0) {
		fClient.EditMessageText(fChatId, fCurrentMsgId, text);
		fCurrentMsgId = 0;
		fInMessage    = false;
	} else {
		fClient.SendMessage(fChatId, text);
	}
}

// ── OutputSink adapter methods (removed when SendWithTools takes StructuredSink*) ──

void TelegramSink::OnText(const std::string& chunk) {
	AppendText(chunk);
}

void TelegramSink::OnToolStatus(const std::string& phase) {
	if (phase.empty()) {
		// Tool finished — find the last unfinished card and mark it done.
		for (auto it = fToolCards.rbegin(); it != fToolCards.rend(); ++it) {
			if (it->started && !it->finished) {
				it->finished = true;
				it->ok       = true;
				break;
			}
		}
		EditCurrent(/*final=*/false);
	} else {
		// Tool started. Extract name from "🔧 running Name…" format.
		// OnToolStatus phase strings come from SendWithTools as
		// "\xF0\x9F\x94\xA7 running <name>\xE2\x80\xA6" — extract the name.
		const std::string prefix = " running ";
		std::string name = phase;
		const auto p = phase.find(prefix);
		if (p != std::string::npos) {
			name = phase.substr(p + prefix.size());
			// Strip trailing …
			while (!name.empty() && (unsigned char)name.back() > 127) name.pop_back();
		}
		ToolStarted(name, "running");
	}
}

api::Permission TelegramSink::AskPermission(const std::string& tool,
                                             const nlohmann::json& input,
                                             std::string* denial_reason) {
	// Build a brief preview from the tool input.
	std::string preview;
	if (input.contains("command") && input["command"].is_string())
		preview = input["command"].get<std::string>();
	else
		preview = input.dump(-1, ' ', false,
		    nlohmann::json::error_handler_t::replace).substr(0, 200);

	const sink::Permission p = AskPermission(tool, preview);
	switch (p) {
		case sink::Permission::kAllow:
		case sink::Permission::kAllowAlways:
			return api::Permission::Allow;
		case sink::Permission::kDeny:
			if (denial_reason)
				*denial_reason = "user denied permission via Telegram";
			return api::Permission::Deny;
	}
	return api::Permission::Deny;
}

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

RemoteControl::RemoteControl(const config::Config& cfg,
                              std::function<config::Auth()> authGetter,
                              const std::string& custom_system)
	: fClient(cfg.telegram.value("bot_token", std::string{})),
	  fAuthGetter(std::move(authGetter)),
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
	fActiveChatId = fPrimaryUserId;
	// Clear the session-scoped always-allow set so approvals from a
	// previous session don't silently carry over.
	fAllowedTools.clear();
	fRunning.store(true);
	fWorkerRunning.store(true);
	fWorker = std::thread(&RemoteControl::WorkLoop, this);
	fPoller = std::thread(&RemoteControl::PollLoop, this);
	return true;
}

void RemoteControl::Stop() {
	if (!fRunning.exchange(false)) return;
	// Wake any thread blocked in AcquireTurn() so it sees
	// fRunning == false and unblocks cleanly.
	fTurnCv.notify_all();
	if (fPoller.joinable()) fPoller.join();
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

void RemoteControl::SetSharedHistory(std::function<json()> provider) {
	fSharedHistory = std::move(provider);
}

void RemoteControl::SetSharedHistoryAppender(
		std::function<void(json, json)> appender) {
	fSharedHistoryAppend = std::move(appender);
}

// Send a "> user_text" preamble to the primary chat before a local turn
// starts streaming. Called by LocalWorker under the turn lock.
void RemoteControl::SendPromptNotice(const std::string& user_text) {
	if (!fRunning.load() || fPrimaryUserId == 0 || g_muted.load()) return;
	fClient.SendMessage(fPrimaryUserId, "> " + user_text);
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
			// Route perm:* and choice:* button taps to the permission
			// queue so TelegramSink::AskPermission / AskChoice can drain it.
			if (u.is_callback && (u.text.rfind("perm:", 0) == 0
			                      || u.text.rfind("choice:", 0) == 0)) {
				// Answer the callback query to dismiss the loading spinner.
				fClient.AnswerCallback(u.callback_query_id);
				{
					std::lock_guard<std::mutex> lk(fPermQueue.mu);
					fPermQueue.callbacks.push_back(u.text);
				}
				fPermQueue.cv.notify_one();
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

	if (u.text.empty() || u.text.front() != '/') {
		// Plain prompt — must go through AcquireTurn / ProcessUpdate,
		// which will print the [remote who] header itself.  Return now
		// without printing so ProcessUpdate doesn't duplicate it.
		return false;
	}

	// Mirror the incoming slash command to the local terminal.
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
	commands::LoopCtx ctx{fAuthGetter(), fCfgMaxTokens, fCustomSystem, rc_prices,
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
		// consume the update here; restore cursor and tell caller it
		// needs the turn lock.
		std::cout << "\x1b""8" << std::flush;
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

	// Save cursor, blank the input row, move to scroll-region bottom.
	// RAII guard restores on every exit path so libedit stays clean.
	std::cout << "\x1b""7";
	tui::ClearInputRow();
	tui::PositionCursorForChat();
	const std::string user_prompt = tui::UserPrompt();
	struct CursorGuard {
		const std::string& prompt;
		~CursorGuard() {
			tui::RepaintInputRow(prompt);
			std::cout << "\x1b""8" << std::flush;
		}
	} _guard{user_prompt};

	std::cout << tui::Meta("[remote " + who + "] " + u.text) << "\n";
	config::LogLine("remote-control rx user=" + std::to_string(u.user_id)
			 + " text=" + u.text);

	if (u.text == "/mute") {
		if (!g_muted.exchange(true))
			fClient.SendMessage(u.chat_id, "Remote muted. No replies until /unmute.");
		return;
	}
	if (u.text == "/unmute") {
		if (g_muted.exchange(false))
			fClient.SendMessage(u.chat_id, "Remote unmuted. Replies will be sent again.");
		return;
	}
	if (u.text == "/new") {
		fUserMessages.erase(u.user_id);
		TgSend(u.chat_id, "(history cleared)");
		return;
	}
	if (!u.text.empty() && u.text.front() == '/') {
		const std::string cmd_word = u.text.substr(0, u.text.find(' '));
		if (cmd_word == "/exit" || cmd_word == "/quit"
				|| cmd_word == "/remote-control") {
			TgSend(u.chat_id, "(" + cmd_word + " is not available from Telegram)");
			return;
		}
		const std::string dispatched = (u.text == "/start") ? "/help" : u.text;

		std::ostringstream capture;
		std::streambuf* old_buf = std::cout.rdbuf(capture.rdbuf());

		json& messages_ref = fUserMessages[u.user_id];
		if (!messages_ref.is_array()) messages_ref = json::array();
		int    rc_turn = 0, rc_in = 0, rc_out = 0;
		bool   rc_notify = false;
		double rc_thresh = 60.0;
		std::vector<std::string> rc_urls;
		json rc_prices = json::object();
		commands::LoopCtx ctx{fAuthGetter(), fCfgMaxTokens, fCustomSystem, rc_prices,
		                      fCfgModel, rc_turn, rc_in, rc_out,
		                      messages_ref, rc_urls, rc_notify, rc_thresh, {}};
		std::string passthrough;
		const commands::SlashAction action = commands::Dispatch(dispatched, ctx, passthrough);
		std::cout.rdbuf(old_buf);
		const std::string output = capture.str();

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
			} else { plain += output[i++]; }
		}
		while (!plain.empty() && (plain.front() == '\n' || plain.front() == ' '))
			plain.erase(plain.begin());
		while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' '))
			plain.pop_back();

		if (action == commands::SlashAction::Passthrough) {
			u.text = passthrough;
			// Fall through to normal Claude turn below.
		} else {
			if (!plain.empty()) TgSend(u.chat_id, plain);
			return;
		}
	}

	// ── Claude turn via TelegramSink ──────────────────────────────────────
	json& msgs = fUserMessages[u.user_id];
	if (!msgs.is_array()) msgs = json::array();

	fActiveChatId.store(u.chat_id);

	const json snapshot = msgs;
	msgs.push_back({{"role", "user"}, {"content", u.text}});

	// Build the messages array: shared local history (if any) + this
	// user's per-chat thread.
	json call_msgs;
	if (fSharedHistory) {
		call_msgs = fSharedHistory();
		for (const auto& m : msgs) call_msgs.push_back(m);
	} else {
		call_msgs = msgs;
	}

	const config::Auth auth = fAuthGetter();
	if (auth.kind == config::AuthKind::None) {
		const std::string err =
			"(error: authentication expired \xE2\x80\x94 "
			"run `claude logout && claude login` on the server)";
		if (!g_muted.load()) fClient.SendMessage(u.chat_id, err);
		msgs = snapshot;
		config::LogLine("remote-control tx user=" + std::to_string(u.user_id)
				 + " -> auth expired");
		fActiveChatId.store(fPrimaryUserId);
		return;
	}

	// Stack-allocate TelegramSink for the lifetime of this one turn.
	// No globals needed — the sink carries all state internally.
	TelegramSink tg_sink(fClient, u.chat_id, fAllowDestructive,
	                     fAllowedTools, fPermQueue);

	if (!g_muted.load())
		tg_sink.BeginMessage("assistant");

	std::cout << tui::ClaudePrompt();
	const std::string effective_system = config::ComposeSystem(fCustomSystem);
	const auto result = api::SendWithTools(auth, fCfgModel, fCfgMaxTokens,
	                                       call_msgs, effective_system,
	                                       g_muted.load() ? nullptr : &tg_sink);
	std::cout << "\n";
	fActiveChatId.store(fPrimaryUserId);

	if (!g_muted.load())
		tg_sink.EndMessage();

	if (result.exit_code != 0 || result.assistant_text.empty()) {
		msgs = snapshot;
		config::LogLine("remote-control tx user=" + std::to_string(u.user_id)
				 + " -> error");
		return;
	}

	// Append to this user's Telegram thread for context on next turn.
	const json assistant_msg = {{"role", "assistant"},
	                             {"content", result.assistant_text}};
	msgs.push_back(assistant_msg);

	// Write-back to local REPL history.
	if (fSharedHistoryAppend) {
		const json user_msg = msgs[msgs.size() - 2];
		fSharedHistoryAppend(user_msg, assistant_msg);
	}

	config::LogLine("remote-control tx user=" + std::to_string(u.user_id)
			 + " out=" + std::to_string(result.output_tokens));
}

} // namespace telegram
