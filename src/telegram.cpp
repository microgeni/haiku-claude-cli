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

} // namespace

Client::Client(std::string bot_token) : token_(std::move(bot_token)) {}

std::string Client::api_url(const std::string& method) const {
    return "https://api.telegram.org/bot" + token_ + "/" + method;
}

bool Client::post_json(const std::string& method, const std::string& body,
                       std::string* out_response, long timeout_sec) {
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

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;
    if (http_code < 200 || http_code >= 300) return false;
    return true;
}

std::vector<Update> Client::poll(int timeout_sec) {
    const json body = {
        {"offset",          next_offset_},
        {"timeout",         timeout_sec},
        {"allowed_updates", json::array({"message"})},
    };

    std::string response;
    if (!post_json("getUpdates", body.dump(), &response,
                   static_cast<long>(timeout_sec + 10))) {
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

bool Client::send_message(int64_t chat_id, const std::string& text) {
    // Telegram's per-message cap is 4096 characters; chunk at 3800 to
    // leave room for any prefix formatting we might add later.
    constexpr size_t kChunk = 3800;
    size_t i = 0;
    bool   all_ok = true;
    while (i < text.size()) {
        const std::string piece = text.substr(i, kChunk);
        i += kChunk;

        const json body = {
            {"chat_id", chat_id},
            {"text",    piece},
        };
        if (!post_json("sendMessage", body.dump(), nullptr, 15)) {
            all_ok = false;
            break;
        }
    }
    if (text.empty()) {
        const json body = {
            {"chat_id", chat_id},
            {"text",    "(empty)"},
        };
        if (!post_json("sendMessage", body.dump(), nullptr, 15)) {
            all_ok = false;
        }
    }
    return all_ok;
}

} // namespace telegram
