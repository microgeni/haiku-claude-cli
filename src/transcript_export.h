#ifndef HAIKU_CLAUDE_CLI_TRANSCRIPT_EXPORT_H
#define HAIKU_CLAUDE_CLI_TRANSCRIPT_EXPORT_H

#include <string>

#include <nlohmann/json.hpp>

// transcript_export — pure Markdown serialization of a conversation.
//
// Extracted from ChatWindow::_ExportTranscript so the serialization logic
// (which has no BeAPI dependency) can be unit-tested in isolation. The
// window keeps only the thin BFile write + Tracker MIME stamp around a call
// to transcript::ToMarkdown.

namespace transcript {

// Serialize a conversation to a Markdown document. `messages` is the raw
// history array (the same shape sent to the Messages API): each element has
// a "role" and a "content" that is either a plain string or an array of
// content blocks (text / image / tool_use / tool_result).
//
//   - A user turn whose content is entirely tool_result blocks is labelled
//     "Tool result" (it's the automated half of a tool round-trip, not
//     something the human typed).
//   - Image blocks become a placeholder noting the media type.
//   - tool_use / tool_result blocks render as blockquoted call/result,
//     with results truncated to 1000 characters.
//   - Turns whose rendered body is empty are skipped.
//
// `topic` may be empty (the "**Topic:**" line is then omitted).
std::string ToMarkdown(const std::string& topic,
                       const std::string& model,
                       int turns,
                       const nlohmann::json& messages);

} // namespace transcript

#endif // HAIKU_CLAUDE_CLI_TRANSCRIPT_EXPORT_H
