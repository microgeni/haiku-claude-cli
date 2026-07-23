#include "sse_parser.h"

#include <sstream>

namespace api {

void ProcessSseEvent(const std::string& event, StreamState* state) {
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
			state->current_thinking.clear();
			state->current_thinking_signature.clear();
			state->current_redacted_thinking.clear();
			if (state->current_type == "tool_use") {
				state->current_tool_id   = cb.value("id",   std::string{});
				state->current_tool_name = cb.value("name", std::string{});
			} else if (state->current_type == "redacted_thinking") {
				// Redacted thinking arrives whole in the start event as an
				// opaque `data` string; there are no deltas to accumulate.
				state->current_redacted_thinking = cb.value("data", std::string{});
			}
		} else if (type == "content_block_delta") {
			const auto& delta = j.value("delta", json::object());
			const std::string dtype = delta.value("type", std::string{});
			if (dtype == "text_delta") {
				const std::string chunk = delta.value("text", "");
				if (state->sink) state->sink->OnText(chunk);
				state->current_text += chunk;
				state->text += chunk;
				state->saw_text = true;
			} else if (dtype == "input_json_delta") {
				state->current_tool_input_raw += delta.value("partial_json", "");
			} else if (dtype == "thinking_delta") {
				const std::string chunk = delta.value("thinking", "");
				if (state->sink) state->sink->OnThinking(chunk);
				state->current_thinking += chunk;
			} else if (dtype == "signature_delta") {
				state->current_thinking_signature += delta.value("signature", "");
			}
		} else if (type == "content_block_stop") {
			if (state->current_type == "text") {
				state->content_blocks.push_back({
					{"type", "text"},
					{"text", state->current_text},
				});
			} else if (state->current_type == "thinking") {
				// Preserve the thinking block verbatim (with its signature)
				// so it can be replayed on the next tool-use turn — the API
				// rejects a continuation whose thinking block is missing or
				// has a mismatched signature.
				state->content_blocks.push_back({
					{"type",      "thinking"},
					{"thinking",  state->current_thinking},
					{"signature", state->current_thinking_signature},
				});
			} else if (state->current_type == "redacted_thinking") {
				state->content_blocks.push_back({
					{"type", "redacted_thinking"},
					{"data", state->current_redacted_thinking},
				});
			} else if (state->current_type == "tool_use") {
				json parsed_input = json::object();
				try {
					if (!state->current_tool_input_raw.empty()) {
						parsed_input = json::parse(state->current_tool_input_raw);
					}
				} catch (const json::exception&) {
					parsed_input = json::object();
				}
				state->content_blocks.push_back({
					{"type",  "tool_use"},
					{"id",    state->current_tool_id},
					{"name",  state->current_tool_name},
					{"input", parsed_input},
				});
			}
			state->current_type.clear();
		} else if (type == "message_start") {
			// Intentionally leave the spinner running — we want the
			// "(elapsed · ↑ N tokens)" tail to render during the
			// window between prompt ingestion and the first
			// text_delta. MarkdownRenderer::Write() stops the
			// spinner on first real output.
			if (j.contains("message") && j["message"].contains("usage")) {
				const auto& u = j["message"]["usage"];
				state->input_tokens.store(u.value("input_tokens",  0),
										  std::memory_order_relaxed);
				state->output_tokens.store(u.value("output_tokens", 0),
										   std::memory_order_relaxed);
				state->cache_creation_input_tokens.store(
					u.value("cache_creation_input_tokens", 0),
					std::memory_order_relaxed);
				state->cache_read_input_tokens.store(
					u.value("cache_read_input_tokens", 0),
					std::memory_order_relaxed);
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
				if (e.contains("type") && e["type"].is_string())
					state->stream_error_type = e["type"].get<std::string>();
				if (e.contains("message") && e["message"].is_string())
					state->stream_error_message = e["message"].get<std::string>();
			}
		}
	} catch (const json::exception&) {
		// Ignore partial/invalid payloads (e.g. ping events).
	}
}

} // namespace api
