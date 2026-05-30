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

#include "config.h"
#include "gui_sink.h"

// ChatWindow — the single chat surface. Owns all BViews; the only
// thread that mutates them is the BLooper thread (main thread).
//
// A worker std::thread runs api::SendWithTools with a GuiSink whose
// callbacks post BMessages here. MessageReceived handles each code and
// appends text / shows alerts / updates the status bar — all on the
// main thread, safely under the window lock.
class ChatWindow : public BWindow {
public:
	// `auth` and `model` come from config::Load() in ReadyToRun().
	ChatWindow(const config::Auth& auth, const std::string& model,
	           int maxTokens, const std::string& systemPrompt);
	~ChatWindow() override;

	void MessageReceived(BMessage* msg) override;
	bool QuitRequested() override;

private:
	// Build the BLayoutBuilder layout once during construction.
	void _BuildLayout();

	// Append plain text to the scrollback on the main thread.
	// Automatically scrolls to the bottom.
	void _AppendText(const std::string& text);

	// Append a dim "tool" line (⚙ name… / ✓ name).
	void _AppendToolLine(const std::string& text);

	// Submit the current input line as a new user turn.
	void _SendTurn();

	// Spawn the worker thread for one api::SendWithTools call.
	void _LaunchWorker(const std::string& userText);

	// Show a BAlert for a tool permission request and unblock the sink.
	void _HandlePermRequest(BMessage* msg);

	// ── Widgets (owned by the layout, not freed in destructor) ───────────
	BTextView*    fOutput  = nullptr; // scrollback — read-only, stylable
	BScrollView*  fScroll  = nullptr;
	BTextControl* fInput   = nullptr; // single-line prompt
	BButton*      fSend    = nullptr;
	BStringView*  fStatus  = nullptr; // model / turn counter

	// ── Conversation state ────────────────────────────────────────────────
	config::Auth       fAuth;
	std::string        fModel;
	int                fMaxTokens;
	std::string        fSystemPrompt;
	nlohmann::json     fMessages; // rolling history passed to SendWithTools
	int                fTurnCount = 0;

	// ── Worker thread ─────────────────────────────────────────────────────
	// The GuiSink is heap-allocated before the thread starts and deleted
	// in MSG_WORKER_DONE after join() confirms the thread has exited.
	// Both the thread and the sink are valid only while fWorkerRunning.
	std::thread           fWorker;
	std::atomic<bool>     fWorkerRunning { false };
	gui::GuiSink*         fSink = nullptr;

	// The last user text, kept so MSG_WORKER_DONE can push the completed
	// assistant turn into fMessages.
	std::string           fPendingUserText;
	std::string           fPendingAssistantText; // accumulated from MSG_CHUNK
};

#endif // HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
