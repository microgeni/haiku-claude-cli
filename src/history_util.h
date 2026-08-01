#ifndef HAIKU_CLAUDE_CLI_HISTORY_UTIL_H
#define HAIKU_CLAUDE_CLI_HISTORY_UTIL_H

#include <cstddef>

#include <nlohmann/json.hpp>

// history_util — pure helpers for the rolling conversation history file.
// No BeAPI, no network, no filesystem: just JSON transforms, so they are
// unit-tested directly (tests/unit/history_util_test.cpp) and link on
// every target. config.cpp forwards to these from LoadHistory/SaveHistory.

namespace config {

using json = nlohmann::json;

// Cap a messages array to its last `cap` entries, dropping the oldest so a
// saved history file can't grow without bound. A leading orphaned
// tool_result — a user turn whose content leads with a tool_result whose
// matching tool_use was dropped by the cut — makes the Messages API reject
// a resume with a 400, so we advance past any such leading turn until the
// array starts on a clean boundary. Arrays already within `cap` (and
// non-array input) are returned unchanged.
json CapHistoryMessages(const json& messages, size_t cap);

// Repair content blocks that the Messages API would reject with
// "text content blocks must contain non-whitespace text" (HTTP 400).
// Empty or whitespace-only blocks reach us from several directions: an
// assistant turn that opened a text block and went straight to tool use,
// a tool (or MCP server, or sub-agent) that returned no output, and old
// session files that already contain such a block — those fail on every
// resume until repaired. The transform is:
//
//   - a whitespace-only string content becomes a "(no content)" marker;
//   - a whitespace-only `text` block inside a content array is dropped;
//   - a `tool_result` whose content is empty, null, or whitespace-only
//     becomes "(no output)";
//   - an array left empty by the above gets a single "(no content)"
//     text block, since a message may not have empty content.
//
// tool_use blocks are never dropped, so tool_use/tool_result pairing is
// preserved. Non-array input is returned unchanged.
json RepairEmptyTextBlocks(const json& messages);

} // namespace config

#endif // HAIKU_CLAUDE_CLI_HISTORY_UTIL_H
