#ifndef HAIKU_CLAUDE_CLI_OUTPUT_SINK_H
#define HAIKU_CLAUDE_CLI_OUTPUT_SINK_H

#include <string>
#include <nlohmann/json.hpp>

// Forward-declare api::Permission so OutputSink can reference it without
// pulling in api.h (which would create a circular dependency since api.h
// forward-declares OutputSink).
namespace api { enum class Permission { Allow, Deny }; }

// OutputSink — the UI seam between the API/tool-use core and any
// front-end (terminal, GUI, Telegram). The core emits semantic events
// through this interface; each front-end implements it independently.
// The terminal front-end (TerminalSink) wraps the existing tui:: calls
// so the CLI behaves identically after the seam is introduced.
//
// All methods are called from the worker thread that runs SendWithTools.
// Implementations that touch UI widgets must marshal to the appropriate
// UI thread themselves.
struct OutputSink {
	virtual ~OutputSink() = default;

	// A chunk of streamed assistant text arrived (SSE text_delta).
	virtual void OnText(const std::string& chunk) = 0;

	// A meta/status notice should be displayed (tool notices,
	// "[interrupted]", etc.).
	virtual void OnMeta(const std::string& text) = 0;

	// A non-fatal diagnostic to stderr (retry notices, token-refresh
	// messages, etc.). Dim in the terminal; suppressed in Telegram.
	virtual void OnDiag(const std::string& text) = 0;

	// A hard error occurred (HTTP error, stream error, curl failure).
	// The terminal sink prints tui::ErrorLabel() + text to stderr.
	virtual void OnError(const std::string& text) = 0;

	// A tool is starting or finishing. Empty `phase` = done.
	virtual void OnToolStatus(const std::string& phase) = 0;

	// Request permission to run a destructive tool. Blocks until the
	// user responds. Returns Allow or Deny. `denial_reason` may be null.
	virtual api::Permission AskPermission(const std::string& tool_name,
	                                      const nlohmann::json& input,
	                                      std::string* denial_reason) = 0;
};

#endif // HAIKU_CLAUDE_CLI_OUTPUT_SINK_H
