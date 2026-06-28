#ifndef HAIKU_CLAUDE_CLI_TEE_SINK_H
#define HAIKU_CLAUDE_CLI_TEE_SINK_H

#include "output_sink.h"

// TeeSink — an OutputSink that fans every event out to two underlying
// sinks. Used by the GUI to stream a locally-typed turn to both the
// chat window (fPrimary, a GuiSink) and the phone (fSecondary, a
// TelegramSink) while remote control is active.
//
// Both sinks must outlive the TeeSink. The TeeSink does not own them.
//
// Permission prompts go to the primary sink ONLY: the user is sitting
// at the GUI, so the GUI owns the decision. Routing AskPermission to
// both would pop a Telegram inline keyboard for a turn the local user
// is already answering, and the two replies would race. The secondary
// sink still sees the tool start/finish notices so the phone transcript
// stays coherent.
class TeeSink : public OutputSink {
public:
	// `primary` owns user interaction (permission prompts); `secondary`
	// is a passive mirror. Neither pointer is owned by the TeeSink.
	TeeSink(OutputSink* primary, OutputSink* secondary)
		: fPrimary(primary), fSecondary(secondary) {}

	~TeeSink() override = default;

	TeeSink(const TeeSink&)            = delete;
	TeeSink& operator=(const TeeSink&) = delete;

	void OnText(const std::string& chunk) override {
		if (fPrimary)   fPrimary->OnText(chunk);
		if (fSecondary) fSecondary->OnText(chunk);
	}

	void OnMeta(const std::string& text) override {
		if (fPrimary)   fPrimary->OnMeta(text);
		if (fSecondary) fSecondary->OnMeta(text);
	}

	void OnDiag(const std::string& text) override {
		if (fPrimary)   fPrimary->OnDiag(text);
		if (fSecondary) fSecondary->OnDiag(text);
	}

	void OnError(const std::string& text) override {
		if (fPrimary)   fPrimary->OnError(text);
		if (fSecondary) fSecondary->OnError(text);
	}

	void OnToolStatus(const std::string& phase) override {
		if (fPrimary)   fPrimary->OnToolStatus(phase);
		if (fSecondary) fSecondary->OnToolStatus(phase);
	}

	// Permission is a single-owner decision: route it to the primary
	// sink only. The secondary never blocks waiting for an answer.
	api::Permission AskPermission(const std::string& tool_name,
	                              const nlohmann::json& input,
	                              std::string* denial_reason) override {
		if (fPrimary)
			return fPrimary->AskPermission(tool_name, input, denial_reason);
		if (fSecondary)
			return fSecondary->AskPermission(tool_name, input, denial_reason);
		return api::Permission::Deny;
	}

private:
	OutputSink* fPrimary;
	OutputSink* fSecondary;
};

#endif // HAIKU_CLAUDE_CLI_TEE_SINK_H
