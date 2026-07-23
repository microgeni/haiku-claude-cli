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

} // namespace config

#endif // HAIKU_CLAUDE_CLI_HISTORY_UTIL_H
