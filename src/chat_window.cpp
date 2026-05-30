#include "chat_window.h"

#include <cstdio>
#include <string>

#include <Alert.h>
#include <Application.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <ScrollBar.h>
#include <TextView.h>

#include "api.h"
#include "config.h"
#include "gui_sink.h"

// Thin wrapper around tui-free plain-text rendering colours.
// We use rgb_color directly rather than pulling in tui.h (which
// would drag in ANSI/libedit headers the GUI doesn't need).
namespace {

const rgb_color kColorBackground  = { 30,  30,  30, 255 };
const rgb_color kColorText        = { 220, 220, 220, 255 };
const rgb_color kColorUserLabel   = {  86, 180, 233, 255 }; // blue
const rgb_color kColorModelLabel  = { 204, 121,  90, 255 }; // orange (Haiku accent)
const rgb_color kColorDim         = { 120, 120, 120, 255 };
const rgb_color kColorError       = { 230,  75,  75, 255 };

// Append text at a given colour to fOutput. Must be called on the main thread.
void AppendWithColor(BTextView* view, const std::string& text, rgb_color color)
{
	if (text.empty()) return;
	BFont font;
	view->GetFont(&font);
	text_run_array* tra = (text_run_array*)malloc(
		sizeof(text_run_array) + sizeof(text_run) * 2);
	if (!tra) {
		view->Insert(text.c_str(), text.size());
		return;
	}
	tra->count = 1;
	tra->runs[0].offset = 0;
	tra->runs[0].font   = font;
	tra->runs[0].color  = color;
	const int32 start = view->TextLength();
	view->Insert(start, text.c_str(), static_cast<int32>(text.size()), tra);
	free(tra);
}

// Scroll fOutput to the very end.
void ScrollToBottom(BTextView* view)
{
	view->ScrollToOffset(view->TextLength());
}

} // namespace

// ── ChatWindow ───────────────────────────────────────────────────────────────

ChatWindow::ChatWindow(const config::Auth& auth, const std::string& model,
                        int maxTokens, const std::string& systemPrompt)
	: BWindow(BRect(100, 100, 820, 620), "Claude",
	           B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	, fAuth(auth)
	, fModel(model)
	, fMaxTokens(maxTokens)
	, fSystemPrompt(systemPrompt)
	, fMessages(nlohmann::json::array())
{
	_BuildLayout();
}

ChatWindow::~ChatWindow()
{
	// If a worker is somehow still alive (shouldn't happen after QuitRequested
	// waits for it), detach to avoid terminate-on-destroy.
	if (fWorker.joinable()) fWorker.detach();
	delete fSink;
}

void ChatWindow::_BuildLayout()
{
	// Output area: stylable, read-only, word-wrapped BTextView.
	BRect dummy(0, 0, 100, 100);
	fOutput = new BTextView(dummy, "output", dummy.InsetByCopy(4, 4),
	                        B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS);
	fOutput->MakeEditable(false);
	fOutput->MakeSelectable(true);
	fOutput->SetWordWrap(true);
	fOutput->SetStylable(true);
	fOutput->SetViewColor(kColorBackground);
	fOutput->SetLowColor(kColorBackground);
	fOutput->SetHighColor(kColorText);
	fOutput->SetFontAndColor(be_fixed_font, B_FONT_ALL, &kColorText);

	fScroll = new BScrollView("scroll", fOutput,
	                          B_FOLLOW_ALL, 0, false, true,
	                          B_FANCY_BORDER);

	// Input row.
	fInput = new BTextControl("input", nullptr, "",
	                           new BMessage(gui::MSG_SEND));
	fInput->SetAlignment(B_ALIGN_LEFT, B_ALIGN_LEFT);

	fSend = new BButton("send", "Send", new BMessage(gui::MSG_SEND));
	fSend->MakeDefault(true);

	// Status bar (one line, left-aligned).
	fStatus = new BStringView("status", "");
	fStatus->SetAlignment(B_ALIGN_LEFT);
	fStatus->SetFont(be_plain_font);

	// Wire the layout: scroll area fills available space, input row at
	// bottom, status bar below that.
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fScroll, 10.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_SMALL_INSETS, 0, B_USE_SMALL_INSETS, 0)
			.Add(fInput,  1.0f)
			.Add(fSend,   0.0f)
		.End()
		.Add(fStatus, 0.0f)
		.SetInsets(0, 0, 0, B_USE_SMALL_INSETS)
	.End();

	// Initial status.
	const std::string status = "model: " + fModel + "  |  turns: 0";
	fStatus->SetText(status.c_str());

	SetSizeLimits(400, 30000, 300, 30000);
}

// ── MessageReceived ──────────────────────────────────────────────────────────

void ChatWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {

	// ── User input ────────────────────────────────────────────────────────
	case gui::MSG_SEND: {
		if (fWorkerRunning.load()) break; // ignore while a turn is in flight
		const char* txt = fInput->Text();
		if (!txt || txt[0] == '\0') break;
		_SendTurn();
		break;
	}

	// ── Streamed assistant text ───────────────────────────────────────────
	case gui::MSG_CHUNK: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			_AppendText(text);
			fPendingAssistantText += text;
		}
		break;
	}

	// ── Turn complete ─────────────────────────────────────────────────────
	case gui::MSG_DONE:
		// Nothing to do here visually — the final newline already came
		// via MSG_CHUNK in GuiSink::EndMessage(). Worker done arrives
		// separately as MSG_WORKER_DONE.
		break;

	// ── Tool lifecycle ────────────────────────────────────────────────────
	case gui::MSG_TOOL_START: {
		const char* name    = nullptr;
		const char* summary = nullptr;
		msg->FindString("name",    &name);
		msg->FindString("summary", &summary);
		std::string line = "\n\xE2\x9A\x99 "; // ⚙
		if (name)    { line += name; line += ": "; }
		if (summary) { line += summary; }
		line += "\xE2\x80\xA6\n"; // …
		_AppendToolLine(line);
		break;
	}
	case gui::MSG_TOOL_DONE: {
		const char* name = nullptr;
		bool        ok   = true;
		msg->FindString("name", &name);
		msg->FindBool("ok",     &ok);
		std::string line = ok ? "\xE2\x9C\x85 " : "\xE2\x9D\x8C "; // ✅ / ❌
		if (name) line += name;
		line += '\n';
		_AppendToolLine(line);
		break;
	}

	// ── Permission request ────────────────────────────────────────────────
	case gui::MSG_ASK_PERM:
		_HandlePermRequest(msg);
		break;

	// ── Status update ─────────────────────────────────────────────────────
	case gui::MSG_STATUS: {
		int32 kind = 0;
		msg->FindInt32("kind", &kind);
		const char* label = nullptr;
		switch (static_cast<sink::StatusKind>(kind)) {
			case sink::StatusKind::kThinking:   label = "thinking…"; break;
			case sink::StatusKind::kCallingTool: label = "running tool…"; break;
			case sink::StatusKind::kIdle:        label = nullptr;     break;
		}
		if (label) {
			fStatus->SetText(label);
		} else {
			const std::string s = "model: " + fModel
			    + "  |  turns: " + std::to_string(fTurnCount);
			fStatus->SetText(s.c_str());
		}
		break;
	}

	// ── Error from worker ─────────────────────────────────────────────────
	case gui::MSG_ERR: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			const std::string line =
				std::string("\n\xE2\x9A\xA0 ") + text + "\n"; // ⚠
			AppendWithColor(fOutput, line, kColorError);
			ScrollToBottom(fOutput);
		}
		break;
	}

	// ── Worker thread finished ────────────────────────────────────────────
	case gui::MSG_WORKER_DONE: {
		fWorkerRunning.store(false);
		if (fWorker.joinable()) fWorker.join();
		delete fSink;
		fSink = nullptr;

		// Commit the completed turn to conversation history.
		if (!fPendingUserText.empty()) {
			fMessages.push_back({{"role", "user"},
			                     {"content", fPendingUserText}});
		}
		if (!fPendingAssistantText.empty()) {
			fMessages.push_back({{"role", "assistant"},
			                     {"content", fPendingAssistantText}});
		}
		fPendingUserText.clear();
		fPendingAssistantText.clear();

		++fTurnCount;
		const std::string s = "model: " + fModel
		    + "  |  turns: " + std::to_string(fTurnCount);
		fStatus->SetText(s.c_str());
		fSend->SetEnabled(true);
		fInput->SetEnabled(true);
		fInput->MakeFocus(true);
		break;
	}

	default:
		BWindow::MessageReceived(msg);
		break;
	}
}

bool ChatWindow::QuitRequested()
{
	// Wait for any in-flight worker to finish before the window tears down,
	// so we don't destroy the GuiSink (which holds a live BMessenger and
	// sem_t) while the worker is still using it.
	if (fWorkerRunning.load() && fWorker.joinable()) {
		// Signal cancellation via g_interrupted so curl aborts quickly.
		g_interrupted = 1;
		fWorker.join();
		delete fSink;
		fSink = nullptr;
	}
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ChatWindow::_AppendText(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorText);
	ScrollToBottom(fOutput);
}

void ChatWindow::_AppendToolLine(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorDim);
	ScrollToBottom(fOutput);
}

void ChatWindow::_SendTurn()
{
	const char* raw = fInput->Text();
	if (!raw || raw[0] == '\0') return;
	const std::string userText(raw);
	fInput->SetText("");

	// Echo the user line into the scrollback.
	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel); // ▸
	_AppendText(userText + "\n");
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 ", kColorModelLabel);

	_LaunchWorker(userText);
}

void ChatWindow::_LaunchWorker(const std::string& userText)
{
	fPendingUserText      = userText;
	fPendingAssistantText.clear();

	fSend->SetEnabled(false);
	fInput->SetEnabled(false);
	fStatus->SetText("thinking\xE2\x80\xA6"); // "thinking…"
	fWorkerRunning.store(true);

	// Capture everything the worker needs by value so no shared state is
	// accessed without the window lock. The GuiSink is the only shared
	// object; it is valid for the entire thread lifetime.
	const config::Auth   auth          = fAuth;
	const std::string    model         = fModel;
	const int            maxTokens     = fMaxTokens;
	const std::string    systemPrompt  = config::ComposeSystem(fSystemPrompt);
	nlohmann::json       messages      = fMessages; // snapshot

	// Append the new user turn to the snapshot so SendWithTools sees it.
	messages.push_back({{"role", "user"}, {"content", userText}});

	fSink = new gui::GuiSink(BMessenger(this));
	gui::GuiSink* sink = fSink; // raw pointer for the lambda

	// Signal BeginMessage so the role header appears before the first chunk.
	sink->BeginMessage("assistant");

	fWorker = std::thread([=]() {
		// Worker thread — no BView calls. Only BMessenger::SendMessage
		// (via GuiSink) and api:: calls are safe here.
		api::SendWithTools(auth, model, maxTokens,
		                   const_cast<nlohmann::json&>(messages),
		                   systemPrompt, sink);
		sink->EndMessage();
		BMessenger(this).SendMessage(gui::MSG_WORKER_DONE);
	});
	fWorker.detach(); // joined in MSG_WORKER_DONE
}

void ChatWindow::_HandlePermRequest(BMessage* msg)
{
	const char* tool    = nullptr;
	const char* preview = nullptr;
	msg->FindString("tool",    &tool);
	msg->FindString("preview", &preview);

	// Build the alert text.
	std::string body = "Allow tool: ";
	body += tool ? tool : "(unknown)";
	if (preview && preview[0]) {
		body += "\n\n";
		// Cap preview at 400 chars so the alert stays readable.
		const std::string pv(preview);
		body += (pv.size() > 400) ? pv.substr(0, 400) + "…" : pv;
	}

	BAlert* alert = new BAlert("Tool Permission",
	    body.c_str(),
	    "Deny", "Allow",
	    nullptr,
	    B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE); // Esc = Deny

	// Go() runs a nested event loop — safe on the BLooper thread.
	// The worker is blocked on sem_wait() throughout.
	const int32 choice = alert->Go();
	const bool  granted = (choice == 1);

	if (fSink) fSink->DeliverPermissionReply(granted);
}
