#include "telegram.h"

#include <cstring>
#include <iostream>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

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

Client::Client(std::string bot_token) : token_(std::move(bot_token)) {}

std::string Client::api_url(const std::string& method) const {
    return "https://api.telegram.org/bot" + token_ + "/" + method;
}

bool Client::post_json(const std::string& method, const std::string& body,
                       std::string* out_response, long timeout_sec,
                       std::atomic<bool>* keep_running) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "content-type: application/json");

    std::string sink;
    if (!out_response) out_response = &sink;

    curl_easy_setopt(curl, CURLOPT_URL, api_url(method).c_str());
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
        {"offset",          next_offset_},
        {"timeout",         timeout_sec},
        {"allowed_updates", json::array({"message", "callback_query"})},
    };

    std::string response;
    if (!post_json("getUpdates", body.dump(), &response,
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
            if (id >= next_offset_) next_offset_ = id + 1;

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

bool Client::send_message(int64_t chat_id, const std::string& text,
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
        if (!post_json("sendMessage", body.dump(), nullptr, 15)) {
            all_ok = false;
            break;
        }
    }
    return all_ok;
}

int64_t Client::send_message_with_id(int64_t chat_id, const std::string& text,
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
    if (!post_json("sendMessage", body.dump(), &response, 15)) return 0;

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
        send_message(chat_id, effective.substr(first_len), keyboard);
    }
    return message_id;
}

bool Client::edit_message_text(int64_t chat_id, int64_t message_id,
                               const std::string& text,
                               const std::vector<std::vector<Button>>& keyboard) {
    if (message_id == 0) return false;
    // Telegram caps edited text at 4096. Truncate with a marker so
    // we always stay under the cap.
    std::string body_text = text;
    constexpr size_t kCap = 3800;
    if (body_text.size() > kCap) {
        body_text = body_text.substr(0, kCap) + "\n\n[... truncated]";
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
    const bool ok = post_json("editMessageText", body.dump(), &response, 10);
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

bool Client::send_chat_action(int64_t chat_id, const std::string& action) {
    const json body = {
        {"chat_id", chat_id},
        {"action",  action},
    };
    return post_json("sendChatAction", body.dump(), nullptr, 5);
}

bool Client::answer_callback(const std::string& callback_query_id,
                             const std::string& notice) {
    json body = {
        {"callback_query_id", callback_query_id},
    };
    if (!notice.empty()) body["text"] = notice;
    return post_json("answerCallbackQuery", body.dump(), nullptr, 10);
}

} // namespace telegram
