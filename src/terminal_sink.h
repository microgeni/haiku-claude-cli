#ifndef HAIKU_CLAUDE_CLI_TERMINAL_SINK_H
#define HAIKU_CLAUDE_CLI_TERMINAL_SINK_H

#include <atomic>
#include <string>

#include <nlohmann/json.hpp>

#include "output_sink.h"
#include "tui.h"

// TerminalSink — the terminal implementation of OutputSink.
// Wraps all tui:: calls that were previously inlined in api.cpp so
// the CLI behaves identically after the seam is introduced. The GUI
// and Telegram front-ends implement OutputSink independently with
// their own rendering (Steps 4 and 5).
//
// Owns the MarkdownRenderer and the thinking Spinner for the current
// turn. Created once per SendConversation call by SendWithTools.
class TerminalSink : public OutputSink {
public:
	TerminalSink();
	~TerminalSink() override;

	// Non-copyable; the spinner holds mutable state.
	TerminalSink(const TerminalSink&)            = delete;
	TerminalSink& operator=(const TerminalSink&) = delete;

	void OnText(const std::string& chunk)      override;
	void OnThinking(const std::string& chunk)  override;
	void OnMeta(const std::string& text)       override;
	void OnDiag(const std::string& text)       override;
	void OnError(const std::string& text)      override;
	void OnToolStatus(const std::string& phase) override;

	api::Permission AskPermission(const std::string& tool_name,
	                              const nlohmann::json& input,
	                              std::string* denial_reason) override;

	// Called by SendConversation after the stream ends to flush any
	// buffered markdown (e.g. a final line without a trailing newline).
	void Flush();

	// Pointer to the live input-token counter so the spinner can show
	// "↑ N tokens" while the model is processing.
	void SetLiveInputTokens(std::atomic<int>* counter);

private:
	tui::MarkdownRenderer fRenderer;
	tui::Spinner*         fSpinner    = nullptr;
	// True once a thinking chunk has been emitted this turn, so OnText can
	// close the dim "thinking" region before the real reply begins.
	bool                  fInThinking = false;
};

#endif // HAIKU_CLAUDE_CLI_TERMINAL_SINK_H
