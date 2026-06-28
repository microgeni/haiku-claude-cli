#ifndef HAIKU_CLAUDE_CLI_STRUCTURED_SINK_H
#define HAIKU_CLAUDE_CLI_STRUCTURED_SINK_H

#include <string>
#include <vector>

// StructuredSink — the shared semantic event model for message-and-widget
// surfaces (GUI and Telegram). Both surfaces share the same event contract;
// each implements its own rendering independently.
//
// This is the "lean" model that replaces the stream-model-forced-into-chat
// that makes the current Telegram bridge noisy:
//
//   NO meta notices / status lines as chat messages
//   NO token/timing stats by default
//   NO scroll-region / cursor artifacts
//   Tool calls COLLAPSED — "ran Bash ✓" line, expandable detail hidden
//   One reply = one settled message (throttled live-edit during streaming)
//
// TerminalSink (Step 4) will also implement this interface. The terminal
// can render all the extra detail it wants — in a scrollback it is free.
// Telegram/GUI suppress it structurally, not via flags.

namespace sink {

// Transient activity status shown as a typing indicator or spinner.
// Never transmitted as a persistent chat message.
enum class StatusKind {
	kThinking,   // model is generating
	kCallingTool, // executing a tool
	kIdle,       // turn complete
};

// Permission decision returned by AskPermission.
enum class Permission {
	kAllow,       // run once
	kAllowAlways, // add to session allowlist
	kDeny,        // block this tool call
};

class StructuredSink {
public:
	virtual ~StructuredSink() = default;

	// ── Message lifecycle ─────────────────────────────────────────────────
	// One BeginMessage…EndMessage bracket is one discrete reply unit.
	// The implementation may send a placeholder on BeginMessage and settle
	// it on EndMessage, or buffer everything and send at EndMessage.
	// `role` is "assistant" | "tool" | "system".
	virtual void BeginMessage(const std::string& role) = 0;

	// A chunk of streamed assistant text arrived. Implementations batch
	// and throttle edits to respect rate limits (Telegram: ~20 edits/min).
	virtual void AppendText(const std::string& chunk) = 0;

	// The reply is complete. Perform the final, authoritative render.
	// After this call no further AppendText will arrive for this message.
	virtual void EndMessage() = 0;

	// ── Tool visibility ───────────────────────────────────────────────────
	// Tool calls are COLLAPSED by default: one line per call, detail hidden.
	// `summary` is a brief human label ("reading src/api.cpp").
	// `detail` is the full output — stored but not shown until requested.
	virtual void ToolStarted(const std::string& name,
	                         const std::string& summary) = 0;
	virtual void ToolFinished(const std::string& name,
	                          bool ok,
	                          const std::string& detail) = 0;

	// ── Interactive affordances ───────────────────────────────────────────
	// Rendered as buttons / inline-keyboard affordances, NOT "type 1-4:".
	// AskChoice returns the 0-based index of the chosen option, or -1 if
	// cancelled / timed out. AskPermission blocks until the user responds.
	virtual int       AskChoice(const std::string& prompt,
	                            const std::vector<std::string>& options) = 0;
	virtual Permission AskPermission(const std::string& tool,
	                                 const std::string& preview) = 0;

	// ── Transient status ──────────────────────────────────────────────────
	// Shown as a typing indicator, spinner badge, or status bar element.
	// Never persisted as a chat message.
	virtual void SetStatus(StatusKind kind) = 0;

	// ── Errors ────────────────────────────────────────────────────────────
	// A hard error that the user must see. Implementations may render this
	// as a chat message, alert dialog, or error badge.
	virtual void OnError(const std::string& message) = 0;
};

} // namespace sink

#endif // HAIKU_CLAUDE_CLI_STRUCTURED_SINK_H
