#ifndef HAIKU_CLAUDE_CLI_SSE_PARSER_H
#define HAIKU_CLAUDE_CLI_SSE_PARSER_H

#include <atomic>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "output_sink.h"

// sse_parser — the Anthropic Messages API server-sent-events state
// machine, extracted from api.cpp so it can be unit-tested without a
// live network connection or any BeAPI dependency.
//
// StreamState accumulates the streamed response: text (forwarded to the
// injected OutputSink as it arrives), structured content_blocks (text and
// tool_use), token usage, stop_reason, and any stream-level error. The
// HTTP write callback in api.cpp splits the raw byte stream on the SSE
// "\n\n" event delimiter and hands each event to ProcessSseEvent.

namespace api {

using json = nlohmann::json;

struct StreamState {
	std::string          sse_buffer;
	std::string          raw_buffer;
	std::string          text;
	std::atomic<int>     input_tokens        { 0 };
	std::atomic<int>     output_tokens       { 0 };
	std::atomic<int>     cache_creation_input_tokens { 0 };
	std::atomic<int>     cache_read_input_tokens     { 0 };
	bool                 saw_text            = false;
	bool                 stream_error        = false;
	std::string          stream_error_type;
	std::string          stream_error_message;
	OutputSink*          sink                = nullptr; // injected

	// Structured content accumulation for tool-use support.
	std::vector<json>    content_blocks;
	std::string          current_type;
	std::string          current_text;
	std::string          current_tool_id;
	std::string          current_tool_name;
	std::string          current_tool_input_raw;
	// Extended-thinking accumulation. A thinking block streams its
	// reasoning via thinking_delta and a cryptographic signature via
	// signature_delta; both must be preserved in content_blocks so the
	// block round-trips on subsequent tool-use turns.
	std::string          current_thinking;
	std::string          current_thinking_signature;
	std::string          current_redacted_thinking; // opaque, for redacted blocks
	std::string          stop_reason;
};

// Parse one complete SSE event (the text between two "\n\n" delimiters)
// and fold it into `state`. Handles content_block_start / _delta / _stop
// (text and tool_use), message_start / message_delta (usage + stop_reason),
// and error events. Partial or non-JSON payloads (ping events) are ignored.
void ProcessSseEvent(const std::string& event, StreamState* state);

} // namespace api

#endif // HAIKU_CLAUDE_CLI_SSE_PARSER_H
