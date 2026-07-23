#ifndef HAIKU_CLAUDE_CLI_MD_TEXT_H
#define HAIKU_CLAUDE_CLI_MD_TEXT_H

#include <string>
#include <vector>

// md_text — pure string transforms shared by the markdown renderers.
//
// These functions have no BeAPI / BTextView dependency, so they compile
// and link on every target (Haiku, macOS/nix) and are exercised directly
// by the unit tests in tests/unit/. MdRenderer (GUI) forwards to them so
// the table-parsing and HTML-stripping logic lives in exactly one place.

namespace md {

// Returns true if `line` looks like a markdown table row: it contains at
// least one '|' and, when trimmed, either starts with '|' or has an
// embedded " | ".
bool IsTableRow(const std::string& line);

// Returns true if `line` is a table separator row (|---|:--:|---|): it
// contains only '|', '-', ':', and whitespace, with at least one '-'.
bool IsSeparatorRow(const std::string& line);

// Split a pipe-delimited table row into trimmed cell strings, stripping
// the outer '|' characters first.
std::vector<std::string> SplitCells(const std::string& line);

// Strip HTML tags and decode common entities from a string, inserting
// newlines before block-level tags. Used to make WebFetch output readable.
std::string StripHtml(const std::string& html);

} // namespace md

#endif // HAIKU_CLAUDE_CLI_MD_TEXT_H
