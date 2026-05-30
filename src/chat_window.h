#ifndef HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
#define HAIKU_CLAUDE_CLI_CHAT_WINDOW_H

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <Button.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <nlohmann/json.hpp>

#include "code_styler.h"
#include "config.h"
#include "gui_sink.h"

// ChatWindow — the single chat surface. Owns all BViews on the main thread.
//
// Code-block streaming model (Step 6):
//   While fInCodeBlock == false: text goes to fOutput (BTextView).
//   When "```lang" arrives: fInCodeBlock = true, fCodeLang = "lang".
//   While fInCodeBlock: text accumulates in fCodeBuffer.
//   When closing "```" arrives: render fCodeBuffer in an embedded
//     BScintillaView with CodeStyler applied, then resume BTextView output.
class ChatWindow : public BWindow {
public:
	ChatWindow(const config::Auth& auth, const std::string& model,
	           int maxTokens, const std::string& systemPrompt);
	~ChatWindow() override;

	void MessageReceived(BMessage* msg) override;
	bool QuitRequested() override;

private:
	void _BuildLayout();

	// Append plain text to the BTextView scrollback (main thread only).
	void _AppendText(const std::string& text);
	void _AppendToolLine(const std::string& text);

	// Process a chunk of streamed text, detecting fenced code blocks.
	// Plain text goes to BTextView; code blocks are buffered until closed.
	void _ProcessChunk(const std::string& chunk);

	// Flush fCodeBuffer as a BScintillaView embedded below the BTextView.
	void _FlushCodeBlock();

	void _SendTurn();
	void _LaunchWorker(const std::string& userText);
	void _HandlePermRequest(BMessage* msg);

	// ── Widgets ────────────────────────────────────────────────────────────
	BTextView*    fOutput    = nullptr;
	BScrollView*  fScroll    = nullptr;
	BTextControl* fInput     = nullptr;
	BButton*      fSend      = nullptr;
	BStringView*  fStatus    = nullptr;

	// ── Conversation state ─────────────────────────────────────────────────
	config::Auth       fAuth;
	std::string        fModel;
	int                fMaxTokens;
	std::string        fSystemPrompt;
	nlohmann::json     fMessages;
	int                fTurnCount = 0;

	// ── Code-block streaming state ─────────────────────────────────────────
	// Two-phase: buffer during streaming, render on closing fence.
	bool               fInCodeBlock  = false;
	std::string        fCodeLang;    // fence tag, e.g. "cpp"
	std::string        fCodeBuffer;  // accumulates code lines
	std::string        fLineBuffer;  // partial line for fence detection

	// CodeStyler loaded once at startup; nullptr if no theme/languages found.
	styling::Theme       fTheme;
	styling::LanguageSet fLangSet;
	styling::CodeStyler* fStyler = nullptr;

	// List of BScintillaView* we've embedded; kept so they can be
	// destroyed with the window (they are not part of fOutput's view
	// hierarchy in the same way as regular child views).
	std::vector<BView*>  fCodeViews;

	// ── Worker thread ──────────────────────────────────────────────────────
	std::thread           fWorker;
	std::atomic<bool>     fWorkerRunning { false };
	gui::GuiSink*         fSink = nullptr;
	std::string           fPendingUserText;
	std::string           fPendingAssistantText;
};

#endif // HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
