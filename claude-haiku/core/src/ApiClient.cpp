#include "cch/ApiClient.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace cch {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal SSE parse state
// ---------------------------------------------------------------------------

struct SseState {
	std::string         sse_buffer;       // unparsed SSE data
	std::string         raw_buffer;       // full response body (for error parsing)
	std::string         text;             // accumulated assistant text
	std::atomic<int>    input_tokens    { 0 };
	std::atomic<int>    output_tokens   { 0 };
	bool                saw_text        = false;
	bool                stream_error    = false;
	std::string         stream_error_type;
	std::string         stream_error_message;

	// Structured content block accumulation for tool_use.
	std::vector<ToolCall> tool_calls;
	std::string           current_type;
	std::string           current_text;
	std::string           current_tool_id;
	std::string           current_tool_name;
	std::string           current_tool_input_raw;
	std::string           stop_reason;

	// Injected by SendStreaming; called on each text_delta.
	const StreamSink* sink = nullptr;
};

// ---------------------------------------------------------------------------
// curl callbacks
// ---------------------------------------------------------------------------

static int XferCallback(void* /*clientp*/,
                        curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                        curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
	// Returning non-zero aborts the transfer. The ApiClient caller
	// signals cancellation by calling curl_multi_wakeup or by having
	// the xfer callback poll an atomic flag passed in clientp. For
	// now this is a placeholder — the AgentLoop will wire cancellation
	// through a shared atomic once the worker-thread model is in place.
	return 0;
}

static void ProcessSseEvent(const std::string& event, SseState* state)
{
	// Extract the data: line(s) from the SSE event block.
	std::string data;
	std::istringstream iss(event);
	std::string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("data:", 0) != 0) continue;
		std::string payload = line.substr(5);
		if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
		if (!data.empty()) data += '\n';
		data += payload;
	}
	if (data.empty()) return;

	try {
		const json j = json::parse(data);
		const std::string type = j.value("type", "");

		if (type == "content_block_start") {
			const auto& cb = j.value("content_block", json::object());
			state->current_type = cb.value("type", std::string{});
			state->current_text.clear();
			state->current_tool_id.clear();
			state->current_tool_name.clear();
			state->current_tool_input_raw.clear();
			if (state->current_type == "tool_use") {
				state->current_tool_id   = cb.value("id",   std::string{});
				state->current_tool_name = cb.value("name", std::string{});
			}
		} else if (type == "content_block_delta") {
			const auto& delta = j.value("delta", json::object());
			const std::string dtype = delta.value("type", std::string{});
			if (dtype == "text_delta") {
				const std::string chunk = delta.value("text", "");
				state->text += chunk;
				state->saw_text = true;
				if (state->sink && state->sink->onChunk)
					state->sink->onChunk(chunk);
			} else if (dtype == "input_json_delta") {
				state->current_tool_input_raw += delta.value("partial_json", "");
			}
		} else if (type == "content_block_stop") {
			if (state->current_type == "text") {
				// Text block complete — nothing extra to record; text is
				// already accumulated in state->text via the deltas above.
			} else if (state->current_type == "tool_use") {
				json parsed_input = json::object();
				try {
					if (!state->current_tool_input_raw.empty())
						parsed_input = json::parse(state->current_tool_input_raw);
				} catch (const json::exception&) {}

				ToolCall tc;
				tc.id    = state->current_tool_id;
				tc.name  = state->current_tool_name;
				tc.input = parsed_input.dump();
				state->tool_calls.push_back(std::move(tc));
			}
			state->current_type.clear();
		} else if (type == "message_start") {
			if (j.contains("message") && j["message"].contains("usage")) {
				const auto& u = j["message"]["usage"];
				state->input_tokens.store(
					u.value("input_tokens",  0), std::memory_order_relaxed);
				state->output_tokens.store(
					u.value("output_tokens", 0), std::memory_order_relaxed);
			}
		} else if (type == "message_delta") {
			if (j.contains("delta") && j["delta"].contains("stop_reason")
				&& j["delta"]["stop_reason"].is_string()) {
				state->stop_reason = j["delta"]["stop_reason"].get<std::string>();
			}
			if (j.contains("usage")) {
				const auto& u = j["usage"];
				state->output_tokens.store(
					u.value("output_tokens",
					        state->output_tokens.load(std::memory_order_relaxed)),
					std::memory_order_relaxed);
			}
		} else if (type == "error") {
			state->stream_error = true;
			if (j.contains("error") && j["error"].is_object()) {
				const auto& e = j["error"];
				if (e.contains("type")    && e["type"].is_string())
					state->stream_error_type    = e["type"].get<std::string>();
				if (e.contains("message") && e["message"].is_string())
					state->stream_error_message = e["message"].get<std::string>();
			}
		}
	} catch (const json::exception&) {
		// Ignore partial / ping events.
	}
}

static size_t StreamWriteCallback(char* data, size_t size, size_t nmemb,
                                  void* userp)
{
	const size_t total = size * nmemb;
	auto* state = static_cast<SseState*>(userp);
	state->raw_buffer.append(data, total);
	state->sse_buffer.append(data, total);

	size_t pos;
	while ((pos = state->sse_buffer.find("\n\n")) != std::string::npos) {
		const std::string event = state->sse_buffer.substr(0, pos);
		state->sse_buffer.erase(0, pos + 2);
		ProcessSseEvent(event, state);
	}
	return total;
}

// ---------------------------------------------------------------------------
// ApiClient
// ---------------------------------------------------------------------------

ApiClient::ApiClient(Config cfg)
	: fCfg(std::move(cfg))
{
}

ApiClient::~ApiClient() = default;

void ApiClient::SendStreaming(const std::vector<Message>& history,
                              const std::string& systemPrompt,
                              const std::string& toolSchemaJson,
                              const StreamSink& sink,
                              std::vector<ToolCall>& outToolCalls)
{
	constexpr int kMaxRetries = 3;
	constexpr int kBaseDelayMs = 1000;

	// Build the messages JSON array.
	json messages = json::array();
	for (const auto& m : history) {
		messages.push_back({{"role", m.role}, {"content", m.content}});
	}

	for (int attempt = 1; ; ++attempt) {
		// Build request body.
		json body = {
			{"model",      fCfg.model},
			{"max_tokens", fCfg.maxTokens},
			{"stream",     true},
			{"messages",   messages},
		};
		if (!systemPrompt.empty()) {
			body["system"] = systemPrompt;
		}
		if (!toolSchemaJson.empty()) {
			try {
				body["tools"] = json::parse(toolSchemaJson);
			} catch (const json::exception&) {
				if (sink.onError)
					sink.onError("invalid tool schema JSON");
				return;
			}
		}

		std::string body_str;
		try {
			body_str = body.dump();
		} catch (const json::exception& e) {
			if (sink.onError)
				sink.onError(std::string("failed to serialize request: ") + e.what());
			return;
		}

		CURL* curl = curl_easy_init();
		if (!curl) {
			if (sink.onError) sink.onError("curl_easy_init failed");
			return;
		}

		// Read the API key from the environment — never stored in the
		// object. This matches the interface comment in ApiClient.h.
		const char* api_key = std::getenv("ANTHROPIC_API_KEY");
		if (!api_key || !*api_key) {
			curl_easy_cleanup(curl);
			if (sink.onError)
				sink.onError("ANTHROPIC_API_KEY is not set");
			return;
		}

		curl_slist* headers = nullptr;
		headers = curl_slist_append(headers,
			(std::string("x-api-key: ") + api_key).c_str());
		headers = curl_slist_append(headers,
			"anthropic-version: 2023-06-01");
		headers = curl_slist_append(headers,
			"content-type: application/json");
		headers = curl_slist_append(headers,
			"accept: text/event-stream");

		SseState state;
		state.sink = &sink;

		curl_easy_setopt(curl, CURLOPT_URL,           fCfg.baseUrl.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body_str.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
		                 static_cast<long>(body_str.size()));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &state);
		curl_easy_setopt(curl, CURLOPT_USERAGENT,
		                 "haiku-claude-cli/core");
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS,    0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferCallback);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,  60L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

		const CURLcode res = curl_easy_perform(curl);
		long http_status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		// Curl-level transient errors — retry with backoff.
		if (res != CURLE_OK) {
			const bool retryable =
				res == CURLE_OPERATION_TIMEDOUT ||
				res == CURLE_COULDNT_CONNECT    ||
				res == CURLE_PARTIAL_FILE       ||
				res == CURLE_GOT_NOTHING        ||
				res == CURLE_RECV_ERROR         ||
				res == CURLE_SEND_ERROR;
			if (retryable && attempt < kMaxRetries) {
				const int delay = kBaseDelayMs << (attempt - 1);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(delay));
				continue;
			}
			if (sink.onError)
				sink.onError(std::string("request failed: ")
				             + curl_easy_strerror(res));
			return;
		}

		// HTTP-level errors.
		if (http_status < 200 || http_status >= 300) {
			// Parse Anthropic's error envelope.
			std::string api_msg;
			try {
				const json err = json::parse(state.raw_buffer);
				if (err.contains("error") && err["error"].is_object()
					&& err["error"].contains("message")
					&& err["error"]["message"].is_string()) {
					api_msg = err["error"]["message"].get<std::string>();
				}
			} catch (const json::exception&) {}

			const bool http_retryable =
				(http_status == 429 || http_status >= 500);
			if (http_retryable && attempt < kMaxRetries) {
				const int delay = kBaseDelayMs << (attempt - 1);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(delay));
				continue;
			}
			std::string msg = "HTTP " + std::to_string(http_status);
			if (!api_msg.empty()) msg += ": " + api_msg;
			if (sink.onError) sink.onError(msg);
			return;
		}

		// SSE-level stream error (arrives after HTTP 200 — e.g. overloaded).
		if (state.stream_error) {
			const bool retryable =
				state.stream_error_type == "overloaded_error" ||
				state.stream_error_type == "api_error";
			if (retryable && attempt < kMaxRetries) {
				const int delay = kBaseDelayMs << (attempt - 1);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(delay));
				continue;
			}
			std::string msg = "stream error";
			if (!state.stream_error_type.empty())
				msg += " (" + state.stream_error_type + ")";
			if (!state.stream_error_message.empty())
				msg += ": " + state.stream_error_message;
			if (sink.onError) sink.onError(msg);
			return;
		}

		// Success. Hand off tool calls and signal done.
		outToolCalls = std::move(state.tool_calls);
		// stop_reason == "tool_use" means the model wants tool dispatch;
		// "end_turn" means the conversation turn is complete.
		// We encode this in the onDone code: 0 = end_turn, 1 = tool_use.
		const int done_code = (state.stop_reason == "tool_use") ? 1 : 0;
		if (sink.onDone) sink.onDone(done_code);
		return;
	}
}

} // namespace cch
