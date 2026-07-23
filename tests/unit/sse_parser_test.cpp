// Unit tests for api::ProcessSseEvent — the Anthropic Messages API SSE state
// machine. Drives canned events through the parser (no network, no BeAPI) and
// asserts the accumulated content_blocks, streamed text, token usage,
// stop_reason, and error handling.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/sse_parser.h"

#include <string>
#include <vector>

using api::StreamState;
using api::ProcessSseEvent;
using nlohmann::json;

namespace {

// Minimal OutputSink that records every OnText chunk so tests can assert
// the live-streamed text separately from the accumulated content_blocks.
struct RecordingSink : OutputSink {
	std::vector<std::string> chunks;

	void OnText(const std::string& c) override { chunks.push_back(c); }
	void OnMeta(const std::string&) override {}
	void OnDiag(const std::string&) override {}
	void OnError(const std::string&) override {}
	void OnToolStatus(const std::string&) override {}
	api::Permission AskPermission(const std::string&, const nlohmann::json&,
	                              std::string*) override {
		return api::Permission::Allow;
	}
};

// Wrap a JSON payload as a single "data: ..." SSE event body.
std::string ev(const std::string& jsonPayload) {
	return "data: " + jsonPayload;
}

} // namespace

// ── Text streaming ──────────────────────────────────────────────────────────

TEST_CASE("text block: deltas accumulate and stream to the sink") {
	StreamState st;
	RecordingSink sink;
	st.sink = &sink;

	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"text"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Hello, "}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"world"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);

	CHECK(st.text == "Hello, world");
	CHECK(st.saw_text == true);
	REQUIRE(sink.chunks.size() == 2);
	CHECK(sink.chunks[0] == "Hello, ");
	CHECK(sink.chunks[1] == "world");

	REQUIRE(st.content_blocks.size() == 1);
	CHECK(st.content_blocks[0]["type"] == "text");
	CHECK(st.content_blocks[0]["text"] == "Hello, world");
}

TEST_CASE("text streaming works with a null sink") {
	StreamState st;  // st.sink defaults to nullptr
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"text"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"hi"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);
	CHECK(st.text == "hi");
	REQUIRE(st.content_blocks.size() == 1);
	CHECK(st.content_blocks[0]["text"] == "hi");
}

// ── tool_use blocks ─────────────────────────────────────────────────────────

TEST_CASE("tool_use: id/name captured, input JSON assembled across deltas") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"tool_use","id":"toolu_01","name":"Read"}})"), &st);
	// Anthropic streams the input object as fragments of raw JSON text.
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"\"a.txt\"}"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);

	REQUIRE(st.content_blocks.size() == 1);
	const auto& b = st.content_blocks[0];
	CHECK(b["type"] == "tool_use");
	CHECK(b["id"]   == "toolu_01");
	CHECK(b["name"] == "Read");
	CHECK(b["input"]["path"] == "a.txt");
}

TEST_CASE("tool_use: empty input yields an empty object, not null") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"tool_use","id":"t1","name":"TodoRead"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);

	REQUIRE(st.content_blocks.size() == 1);
	CHECK(st.content_blocks[0]["input"].is_object());
	CHECK(st.content_blocks[0]["input"].empty());
}

TEST_CASE("tool_use: malformed input JSON falls back to an empty object") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"tool_use","id":"t2","name":"Bash"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"{not valid"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);

	REQUIRE(st.content_blocks.size() == 1);
	CHECK(st.content_blocks[0]["input"].is_object());
	CHECK(st.content_blocks[0]["input"].empty());
}

TEST_CASE("mixed turn: a text block then a tool_use block, in order") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"text"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Let me check."}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_start","content_block":{"type":"tool_use","id":"t3","name":"Glob"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"{\"pattern\":\"*.cpp\"}"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);

	REQUIRE(st.content_blocks.size() == 2);
	CHECK(st.content_blocks[0]["type"] == "text");
	CHECK(st.content_blocks[0]["text"] == "Let me check.");
	CHECK(st.content_blocks[1]["type"] == "tool_use");
	CHECK(st.content_blocks[1]["name"] == "Glob");
	CHECK(st.content_blocks[1]["input"]["pattern"] == "*.cpp");
}

// ── usage & stop_reason ─────────────────────────────────────────────────────

TEST_CASE("message_start records prompt-cache and input token usage") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"message_start","message":{"usage":{"input_tokens":123,"output_tokens":1,"cache_read_input_tokens":100,"cache_creation_input_tokens":20}}})"), &st);
	CHECK(st.input_tokens.load()  == 123);
	CHECK(st.cache_read_input_tokens.load()     == 100);
	CHECK(st.cache_creation_input_tokens.load() == 20);
}

TEST_CASE("message_delta updates output tokens and stop_reason") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":42}})"), &st);
	CHECK(st.stop_reason == "tool_use");
	CHECK(st.output_tokens.load() == 42);
}

TEST_CASE("end_turn stop_reason is captured") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})"), &st);
	CHECK(st.stop_reason == "end_turn");
}

// ── errors & robustness ─────────────────────────────────────────────────────

TEST_CASE("error event sets the error flag and captures type/message") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"), &st);
	CHECK(st.stream_error == true);
	CHECK(st.stream_error_type    == "overloaded_error");
	CHECK(st.stream_error_message == "Overloaded");
}

TEST_CASE("ping and unknown event types are ignored") {
	StreamState st;
	ProcessSseEvent(ev(R"({"type":"ping"})"), &st);
	ProcessSseEvent(ev(R"({"type":"some_future_event","x":1})"), &st);
	CHECK(st.content_blocks.empty());
	CHECK(st.stream_error == false);
	CHECK(st.text.empty());
}

TEST_CASE("non-JSON data payloads are swallowed without throwing") {
	StreamState st;
	ProcessSseEvent(ev("this is not json"), &st);
	ProcessSseEvent("data: {broken", &st);
	CHECK(st.content_blocks.empty());
	CHECK(st.stream_error == false);
}

TEST_CASE("event with no data: lines is a no-op") {
	StreamState st;
	ProcessSseEvent("event: message_stop", &st);
	ProcessSseEvent("", &st);
	CHECK(st.content_blocks.empty());
}

TEST_CASE("CRLF line endings and multi-line data are handled") {
	StreamState st;
	// SSE allows the payload to span multiple data: lines, joined by '\n'.
	// Here the two lines together form one valid JSON object.
	std::string event =
		"data: {\"type\":\"content_block_start\",\r\n"
		"data: \"content_block\":{\"type\":\"text\"}}\r";
	ProcessSseEvent(event, &st);
	ProcessSseEvent(ev(R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"ok"}})"), &st);
	ProcessSseEvent(ev(R"({"type":"content_block_stop"})"), &st);
	REQUIRE(st.content_blocks.size() == 1);
	CHECK(st.content_blocks[0]["text"] == "ok");
}
