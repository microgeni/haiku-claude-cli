// Unit tests for transcript::ToMarkdown — the pure conversation-to-Markdown
// serializer extracted from ChatWindow::_ExportTranscript. No BeAPI, so it
// runs on every target.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/transcript_export.h"

#include <string>

using transcript::ToMarkdown;
using nlohmann::json;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
	return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("header includes model and turn count; topic omitted when empty") {
	json msgs = json::array();
	std::string md = ToMarkdown("", "claude-sonnet-4-6", 3, msgs);
	CHECK(contains(md, "# Claude transcript"));
	CHECK(contains(md, "**Model:** claude-sonnet-4-6"));
	CHECK(contains(md, "**Turns:** 3"));
	CHECK_FALSE(contains(md, "**Topic:**"));
}

TEST_CASE("topic line appears when a topic is given") {
	std::string md = ToMarkdown("Refactor the parser", "m", 1, json::array());
	CHECK(contains(md, "**Topic:** Refactor the parser"));
}

TEST_CASE("plain string user and assistant turns get labelled sections") {
	json msgs = json::array({
		{{"role", "user"},      {"content", "hello"}},
		{{"role", "assistant"}, {"content", "hi there"}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "## You\n\nhello"));
	CHECK(contains(md, "## Claude\n\nhi there"));
}

TEST_CASE("array content: text blocks are concatenated") {
	json msgs = json::array({
		{{"role", "assistant"}, {"content", json::array({
			{{"type", "text"}, {"text", "part one "}},
			{{"type", "text"}, {"text", "part two"}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "## Claude\n\npart one part two"));
}

TEST_CASE("image blocks render a placeholder with the media type") {
	json msgs = json::array({
		{{"role", "user"}, {"content", json::array({
			{{"type", "image"},
			 {"source", {{"media_type", "image/png"}}}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "_[image attachment: image/png]_"));
}

TEST_CASE("a user turn of only tool_result blocks is labelled Tool result") {
	json msgs = json::array({
		{{"role", "user"}, {"content", json::array({
			{{"type", "tool_result"}, {"tool_use_id", "t1"}, {"content", "42"}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "## Tool result"));
	CHECK_FALSE(contains(md, "## You"));
	CHECK(contains(md, "**tool result:**"));
	CHECK(contains(md, "```\n42\n```"));
}

TEST_CASE("tool_use block renders the tool name as a call") {
	json msgs = json::array({
		{{"role", "assistant"}, {"content", json::array({
			{{"type", "text"}, {"text", "let me look"}},
			{{"type", "tool_use"}, {"name", "Read"}, {"input", json::object()}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "**tool call:** `Read`"));
}

TEST_CASE("long tool_result content is truncated at 1000 chars") {
	std::string big(2000, 'x');
	json msgs = json::array({
		{{"role", "user"}, {"content", json::array({
			{{"type", "tool_result"}, {"content", big}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "[truncated]"));
	// The full 2000-char blob must not appear verbatim.
	CHECK_FALSE(contains(md, big));
}

TEST_CASE("tool_result with array content collects text blocks") {
	json msgs = json::array({
		{{"role", "user"}, {"content", json::array({
			{{"type", "tool_result"}, {"content", json::array({
				{{"type", "text"}, {"text", "line1"}},
				{{"type", "text"}, {"text", "line2"}},
			})}},
		})}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "line1line2"));
}

TEST_CASE("turns with empty bodies are skipped") {
	json msgs = json::array({
		{{"role", "user"}, {"content", ""}},           // empty string
		{{"role", "assistant"}, {"content", "kept"}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK_FALSE(contains(md, "## You"));
	CHECK(contains(md, "## Claude\n\nkept"));
}

TEST_CASE("non-array messages input yields just the header") {
	std::string md = ToMarkdown("", "m", 0, json::object());
	CHECK(contains(md, "# Claude transcript"));
	CHECK_FALSE(contains(md, "## "));
}

TEST_CASE("an unknown role is used verbatim as the section heading") {
	json msgs = json::array({
		{{"role", "system"}, {"content", "sys note"}},
	});
	std::string md = ToMarkdown("", "m", 1, msgs);
	CHECK(contains(md, "## system\n\nsys note"));
}
