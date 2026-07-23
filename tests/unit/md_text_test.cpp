// Unit tests for md::md_text — the pure string transforms shared by the
// markdown renderers. No BeAPI dependency, so this links and runs on every
// target (Haiku, macOS/nix, CI).
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/md_text.h"

using md::IsTableRow;
using md::IsSeparatorRow;
using md::SplitCells;
using md::StripHtml;

// ── IsTableRow ──────────────────────────────────────────────────────────────

TEST_CASE("IsTableRow: leading pipe is a table row") {
	CHECK(IsTableRow("| a | b |"));
	CHECK(IsTableRow("  | a | b |"));   // leading whitespace tolerated
	CHECK(IsTableRow("\t| a |"));       // leading tab
}

TEST_CASE("IsTableRow: embedded ' | ' counts even without a leading pipe") {
	CHECK(IsTableRow("a | b"));
	CHECK(IsTableRow("col1 | col2 | col3"));
}

TEST_CASE("IsTableRow: no pipe is not a table row") {
	CHECK_FALSE(IsTableRow("just a sentence"));
	CHECK_FALSE(IsTableRow(""));
}

TEST_CASE("IsTableRow: a bare pipe with no spacing and no leading pipe is rejected") {
	// "a|b" has a pipe but no leading '|' and no ' | ' — current behaviour
	// treats this as NOT a table row. This pins the documented contract.
	CHECK_FALSE(IsTableRow("a|b"));
}

// ── IsSeparatorRow ──────────────────────────────────────────────────────────

TEST_CASE("IsSeparatorRow: classic separators") {
	CHECK(IsSeparatorRow("|---|---|"));
	CHECK(IsSeparatorRow("| --- | --- |"));
	CHECK(IsSeparatorRow("---"));
}

TEST_CASE("IsSeparatorRow: alignment markers are allowed") {
	CHECK(IsSeparatorRow("|:---|---:|:--:|"));
	CHECK(IsSeparatorRow(":---: | :---:"));
}

TEST_CASE("IsSeparatorRow: must contain at least one dash") {
	CHECK_FALSE(IsSeparatorRow("|   |   |"));  // pipes/space only, no dash
	CHECK_FALSE(IsSeparatorRow(""));
	CHECK_FALSE(IsSeparatorRow("::::"));
}

TEST_CASE("IsSeparatorRow: any other glyph disqualifies") {
	CHECK_FALSE(IsSeparatorRow("| a- |"));      // letter present
	CHECK_FALSE(IsSeparatorRow("|--- x ---|")); // stray letter
}

// ── SplitCells ──────────────────────────────────────────────────────────────

TEST_CASE("SplitCells: strips outer pipes and trims each cell") {
	auto cells = SplitCells("| a | b | c |");
	REQUIRE(cells.size() == 3);
	CHECK(cells[0] == "a");
	CHECK(cells[1] == "b");
	CHECK(cells[2] == "c");
}

TEST_CASE("SplitCells: works without outer pipes") {
	auto cells = SplitCells("a | b");
	REQUIRE(cells.size() == 2);
	CHECK(cells[0] == "a");
	CHECK(cells[1] == "b");
}

TEST_CASE("SplitCells: preserves empty interior cells") {
	auto cells = SplitCells("| a |  | c |");
	REQUIRE(cells.size() == 3);
	CHECK(cells[0] == "a");
	CHECK(cells[1] == "");
	CHECK(cells[2] == "c");
}

TEST_CASE("SplitCells: trims surrounding whitespace on the whole row") {
	auto cells = SplitCells("   |  x  |  y  |   ");
	REQUIRE(cells.size() == 2);
	CHECK(cells[0] == "x");
	CHECK(cells[1] == "y");
}

// ── StripHtml ───────────────────────────────────────────────────────────────

TEST_CASE("StripHtml: removes tags, keeps text") {
	CHECK(StripHtml("<b>hello</b>") == "hello");
	CHECK(StripHtml("<span class=\"x\">hi</span>") == "hi");
}

TEST_CASE("StripHtml: decodes named entities") {
	CHECK(StripHtml("a &amp; b") == "a & b");
	CHECK(StripHtml("&lt;tag&gt;") == "<tag>");
	CHECK(StripHtml("&quot;q&quot; &apos;a&apos;") == "\"q\" 'a'");
	CHECK(StripHtml("a&nbsp;b") == "a b");
}

TEST_CASE("StripHtml: decodes numeric entities (decimal and hex)") {
	CHECK(StripHtml("&#65;&#66;&#67;") == "ABC");   // decimal
	CHECK(StripHtml("&#x41;&#x42;") == "AB");        // hex
}

TEST_CASE("StripHtml: out-of-range numeric entities become '?'") {
	// Code points >= 128 are not emitted as raw bytes; they map to '?'.
	CHECK(StripHtml("&#256;") == "?");
}

TEST_CASE("StripHtml: unknown entity passes through verbatim") {
	CHECK(StripHtml("100&percnt; done") == "100&percnt; done");
}

TEST_CASE("StripHtml: block tags insert a newline before content") {
	CHECK(StripHtml("one<p>two") == "one\ntwo");
	CHECK(StripHtml("a<br>b") == "a\nb");
	CHECK(StripHtml("<li>x<li>y") == "x\ny");
}

TEST_CASE("StripHtml: collapses 3+ consecutive newlines to at most 2") {
	CHECK(StripHtml("a\n\n\n\n\nb") == "a\n\nb");
}

TEST_CASE("StripHtml: empty input yields empty output") {
	CHECK(StripHtml("") == "");
}
