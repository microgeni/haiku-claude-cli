// Unit tests for config::CapHistoryMessages — the pure rolling-history cap.
// Ensures a saved history file can't grow without bound and that a capped
// (resumed) conversation always starts on a valid turn boundary, i.e. never
// with an orphaned tool_result whose matching tool_use was dropped.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/history_util.h"

using config::CapHistoryMessages;
using nlohmann::json;

namespace {

// A plain user text message.
json userMsg(const std::string& text) {
	return json{{"role", "user"}, {"content", text}};
}

// An assistant text message.
json asstMsg(const std::string& text) {
	return json{{"role", "assistant"}, {"content", text}};
}

// A user message whose content array leads with a tool_result block.
json toolResultMsg(const std::string& id) {
	return json{
		{"role", "user"},
		{"content", json::array({
			{{"type", "tool_result"}, {"tool_use_id", id}, {"content", "ok"}},
		})},
	};
}

} // namespace

TEST_CASE("under the cap: array is returned unchanged") {
	json msgs = json::array({ userMsg("a"), asstMsg("b"), userMsg("c") });
	json out = CapHistoryMessages(msgs, 10);
	CHECK(out == msgs);
}

TEST_CASE("exactly at the cap: unchanged") {
	json msgs = json::array({ userMsg("a"), asstMsg("b") });
	CHECK(CapHistoryMessages(msgs, 2) == msgs);
}

TEST_CASE("over the cap: keeps the last N, drops the oldest") {
	json msgs = json::array({
		userMsg("1"), asstMsg("2"), userMsg("3"), asstMsg("4"), userMsg("5"),
	});
	json out = CapHistoryMessages(msgs, 2);
	REQUIRE(out.size() == 2);
	CHECK(out[0]["content"] == "4");
	CHECK(out[1]["content"] == "5");
}

TEST_CASE("leading orphaned tool_result is skipped past the boundary") {
	// With cap=2 the window would start at the tool_result user turn,
	// whose matching tool_use (in the dropped assistant turn) is gone.
	// CapHistoryMessages must advance past it so the survivor starts clean.
	json msgs = json::array({
		userMsg("ask"),        // 0 (dropped)
		asstMsg("tool_use"),   // 1 (dropped — held the tool_use block)
		toolResultMsg("t1"),   // 2 (would be first survivor, but orphaned)
		asstMsg("answer"),     // 3
	});
	json out = CapHistoryMessages(msgs, 2);
	// The orphaned tool_result at index 2 is skipped; only index 3 remains.
	REQUIRE(out.size() == 1);
	CHECK(out[0]["role"] == "assistant");
	CHECK(out[0]["content"] == "answer");
}

TEST_CASE("a normal user turn at the boundary is NOT skipped") {
	json msgs = json::array({
		userMsg("1"), asstMsg("2"), userMsg("3"), asstMsg("4"),
	});
	json out = CapHistoryMessages(msgs, 2);
	REQUIRE(out.size() == 2);
	CHECK(out[0]["content"] == "3");   // plain user turn survives
	CHECK(out[1]["content"] == "4");
}

TEST_CASE("non-array input is returned unchanged") {
	json obj = json{{"role", "user"}, {"content", "x"}};
	CHECK(CapHistoryMessages(obj, 5) == obj);
	json nul = json();
	CHECK(CapHistoryMessages(nul, 5) == nul);
}

TEST_CASE("cap of zero collapses to an empty (but valid) array") {
	json msgs = json::array({ userMsg("a"), asstMsg("b") });
	json out = CapHistoryMessages(msgs, 0);
	CHECK(out.is_array());
	CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// RepairEmptyTextBlocks — no content block may be empty or whitespace-only,
// or the Messages API rejects the whole request with HTTP 400.
// ---------------------------------------------------------------------------

using config::RepairEmptyTextBlocks;

TEST_CASE("whitespace-only string content becomes a placeholder") {
	json msgs = json::array({ userMsg("   \n\t ") });
	json out = RepairEmptyTextBlocks(msgs);
	CHECK(out[0]["content"] == "(no content)");
}

TEST_CASE("well-formed messages are returned untouched") {
	json msgs = json::array({ userMsg("hello"), asstMsg("hi") });
	CHECK(RepairEmptyTextBlocks(msgs) == msgs);
}

TEST_CASE("an empty text block beside a tool_use is dropped, tool_use kept") {
	json msgs = json::array({{
		{"role", "assistant"},
		{"content", json::array({
			{{"type", "text"}, {"text", "\n"}},
			{{"type", "tool_use"}, {"id", "t1"}, {"name", "Bash"},
			 {"input", json::object()}},
		})},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	REQUIRE(out[0]["content"].size() == 1);
	CHECK(out[0]["content"][0]["type"] == "tool_use");
	CHECK(out[0]["content"][0]["id"] == "t1");
}

TEST_CASE("an empty tool_result gets the (no output) marker") {
	json msgs = json::array({{
		{"role", "user"},
		{"content", json::array({
			{{"type", "tool_result"}, {"tool_use_id", "t1"}, {"content", "  "}},
		})},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	CHECK(out[0]["content"][0]["content"] == "(no output)");
	CHECK(out[0]["content"][0]["tool_use_id"] == "t1");
}

TEST_CASE("a tool_result with missing or null content is repaired") {
	json msgs = json::array({{
		{"role", "user"},
		{"content", json::array({
			{{"type", "tool_result"}, {"tool_use_id", "t1"}},
			{{"type", "tool_result"}, {"tool_use_id", "t2"}, {"content", json()}},
		})},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	CHECK(out[0]["content"][0]["content"] == "(no output)");
	CHECK(out[0]["content"][1]["content"] == "(no output)");
}

TEST_CASE("a tool_result whose content is a block array is repaired in place") {
	json msgs = json::array({{
		{"role", "user"},
		{"content", json::array({
			{{"type", "tool_result"}, {"tool_use_id", "t1"},
			 {"content", json::array({ {{"type", "text"}, {"text", " "}} })}},
		})},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	const json& inner = out[0]["content"][0]["content"];
	REQUIRE(inner.size() == 1);
	CHECK(inner[0]["text"] == "(no output)");
}

TEST_CASE("an array emptied by the repair gets a placeholder text block") {
	json msgs = json::array({{
		{"role", "assistant"},
		{"content", json::array({ {{"type", "text"}, {"text", ""}} })},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	REQUIRE(out[0]["content"].size() == 1);
	CHECK(out[0]["content"][0]["type"] == "text");
	CHECK(out[0]["content"][0]["text"] == "(no content)");
}

TEST_CASE("image blocks survive an empty sibling text block") {
	json msgs = json::array({{
		{"role", "user"},
		{"content", json::array({
			{{"type", "text"}, {"text", ""}},
			{{"type", "image"}, {"source", {{"type", "base64"}}}},
		})},
	}});
	json out = RepairEmptyTextBlocks(msgs);
	REQUIRE(out[0]["content"].size() == 1);
	CHECK(out[0]["content"][0]["type"] == "image");
}

TEST_CASE("non-array input is returned unchanged by the repair") {
	json nul = json();
	CHECK(RepairEmptyTextBlocks(nul) == nul);
}
