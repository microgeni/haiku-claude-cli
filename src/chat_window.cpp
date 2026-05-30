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
#include "code_styler.h"
#include "config.h"
#include "gui_sink.h"
#include "md_renderer.h"
#include "scintilla_view.h"

// ---------------------------------------------------------------------------
// Internal colour helpers (no tui:: dependency)
// ---------------------------------------------------------------------------
namespace {

const rgb_color kColorBackground  = {  30,  30,  30, 255 };
const rgb_color kColorText        = { 220, 220, 220, 255 };
const rgb_color kColorUserLabel   = {  86, 180, 233, 255 };
const rgb_color kColorModelLabel  = { 204, 121,  90, 255 };
const rgb_color kColorDim         = { 120, 120, 120, 255 };
const rgb_color kColorError       = { 230,  75,  75, 255 };

void AppendWithColor(BTextView* view, const std::string& text, rgb_color color)
{
	if (text.empty()) return;
	BFont font;
	view->GetFont(&font);
	text_run_array* tra = static_cast<text_run_array*>(
		malloc(sizeof(text_run_array) + sizeof(text_run)));
	if (!tra) {
		view->Insert(text.c_str(), static_cast<int32>(text.size()));
		return;
	}
	tra->count       = 1;
	tra->runs[0].offset = 0;
	tra->runs[0].font   = font;
	tra->runs[0].color  = color;
	const int32 start = view->TextLength();
	view->Insert(start, text.c_str(), static_cast<int32>(text.size()), tra);
	free(tra);
}

void ScrollToBottom(BTextView* view)
{
	view->ScrollToOffset(view->TextLength());
}

// Count lines in a string.
int CountLines(const std::string& s)
{
	int n = 0;
	for (char c : s) if (c == '\n') ++n;
	return n;
}

} // namespace

// ---------------------------------------------------------------------------
// ChatWindow
// ---------------------------------------------------------------------------

ChatWindow::ChatWindow(const config::Auth& auth, const std::string& model,
                        int maxTokens, const std::string& systemPrompt)
	: BWindow(BRect(100, 100, 820, 640), "Claude",
	           B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	, fAuth(auth)
	, fModel(model)
	, fMaxTokens(maxTokens)
	, fSystemPrompt(systemPrompt)
	, fMessages(nlohmann::json::array())
{
	// Try to load the Genio theme and language set.
	const std::string themePath = styling::FindDefaultTheme();
	const std::string langsDir  = styling::FindLanguagesDir();
	if (!themePath.empty() && fTheme.LoadFile(themePath)
	    && !langsDir.empty()  && fLangSet.LoadDir(langsDir)) {
		fStyler = new styling::CodeStyler(fTheme, fLangSet);
	}

	_BuildLayout();

	// Create the markdown renderer now that fOutput exists.
	fMdRenderer = new md::MdRenderer(fOutput);
}

ChatWindow::~ChatWindow()
{
	delete fStyler;
	delete fMdRenderer;
	if (fWorker.joinable()) fWorker.detach();
	delete fSink;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ChatWindow::_BuildLayout()
{
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
	                          B_FOLLOW_ALL, 0, false, true, B_FANCY_BORDER);

	fInput = new BTextControl("input", nullptr, "",
	                           new BMessage(gui::MSG_SEND));
	fSend  = new BButton("send", "Send", new BMessage(gui::MSG_SEND));
	fSend->MakeDefault(true);
	fStatus = new BStringView("status", "");
	fStatus->SetAlignment(B_ALIGN_LEFT);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fScroll, 10.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_SMALL_INSETS, 0, B_USE_SMALL_INSETS, 0)
			.Add(fInput, 1.0f)
			.Add(fSend,  0.0f)
		.End()
		.Add(fStatus, 0.0f)
		.SetInsets(0, 0, 0, B_USE_SMALL_INSETS)
	.End();

	const std::string s = "model: " + fModel + "  |  turns: 0";
	fStatus->SetText(s.c_str());
	SetSizeLimits(400, 30000, 300, 30000);
}

// ---------------------------------------------------------------------------
// MessageReceived
// ---------------------------------------------------------------------------

void ChatWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {

	case gui::MSG_SEND: {
		if (fWorkerRunning.load()) break;
		const char* txt = fInput->Text();
		if (!txt || txt[0] == '\0') break;
		_SendTurn();
		break;
	}

	case gui::MSG_CHUNK: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			// WebFetch HTML is buffered; everything else goes through
			// the two-phase markdown/_ProcessChunk pipeline.
			// Detection: tool_result text starting with "HTTP NNN" where
			// the content-type contains "html". We check fInWebFetch which
			// was set by MSG_TOOL_START("WebFetch").
			if (fInWebFetch) {
				fWebFetchBuf += text;
				// Check if this looks like plain text (not HTML) — if the
				// first line is "HTTP 200 (text/plain..." just pass through.
				if (fWebFetchBuf.find("text/html") == std::string::npos &&
				    fWebFetchBuf.find("<html") == std::string::npos &&
				    fWebFetchBuf.find("<HTML") == std::string::npos &&
				    fWebFetchBuf.size() > 40) {
					// Not HTML — emit directly.
					fInWebFetch = false;
					if (fMdRenderer)
						fMdRenderer->Write(fWebFetchBuf);
					else
						_AppendText(fWebFetchBuf);
					fWebFetchBuf.clear();
				}
			} else {
				_ProcessChunk(text);
			}
			fPendingAssistantText += text;
		}
		break;
	}

	case gui::MSG_DONE:
		// Flush any open code block when the turn ends.
		if (fInCodeBlock) {
			fInCodeBlock = false;
			_FlushCodeBlock();
		}
		// Flush any partial markdown line.
		if (fMdRenderer) fMdRenderer->Flush();
		fLineBuffer.clear();
		fInWebFetch = false;
		fWebFetchBuf.clear();
		break;

	case gui::MSG_TOOL_START: {
		const char* name    = nullptr;
		const char* summary = nullptr;
		msg->FindString("name",    &name);
		msg->FindString("summary", &summary);
		// Detect WebFetch so we can strip HTML from its output.
		if (name && std::string(name) == "WebFetch") {
			fInWebFetch = true;
			fWebFetchBuf.clear();
		}
		std::string line = "\n\xE2\x9A\x99 "; // ⚙
		if (name)    { line += name;    line += ": "; }
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
		// If this was a WebFetch, flush the stripped HTML now.
		if (fInWebFetch) {
			fInWebFetch = false;
			if (!fWebFetchBuf.empty()) {
				const std::string stripped = md::StripHtml(fWebFetchBuf);
				if (fMdRenderer)
					fMdRenderer->Write(stripped + "\n");
				else
					_AppendText(stripped + "\n");
				fWebFetchBuf.clear();
			}
		}
		std::string line = ok ? "\xE2\x9C\x85 " : "\xE2\x9D\x8C "; // ✅/❌
		if (name) line += name;
		line += '\n';
		_AppendToolLine(line);
		break;
	}

	case gui::MSG_ASK_PERM:
		_HandlePermRequest(msg);
		break;

	case gui::MSG_STATUS: {
		int32 kind = 0;
		msg->FindInt32("kind", &kind);
		switch (static_cast<sink::StatusKind>(kind)) {
			case sink::StatusKind::kThinking:
				fStatus->SetText("thinking\xE2\x80\xA6");
				break;
			case sink::StatusKind::kCallingTool:
				fStatus->SetText("running tool\xE2\x80\xA6");
				break;
			case sink::StatusKind::kIdle: {
				const std::string s = "model: " + fModel
				    + "  |  turns: " + std::to_string(fTurnCount);
				fStatus->SetText(s.c_str());
				break;
			}
		}
		break;
	}

	case gui::MSG_ERR: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			AppendWithColor(fOutput,
				std::string("\n\xE2\x9A\xA0 ") + text + "\n", // ⚠
				kColorError);
			ScrollToBottom(fOutput);
			fStatus->SetText("Error");
		}
		break;
	}

	case gui::MSG_WORKER_DONE: {
		fWorkerRunning.store(false);
		if (fWorker.joinable()) fWorker.join();
		delete fSink;
		fSink = nullptr;
		fInCodeBlock = false;
		fCodeBuffer.clear();
		fLineBuffer.clear();
		fInWebFetch = false;
		fWebFetchBuf.clear();
		if (fMdRenderer) fMdRenderer->Flush();

		if (!fPendingUserText.empty())
			fMessages.push_back({{"role", "user"},
			                     {"content", fPendingUserText}});
		if (!fPendingAssistantText.empty())
			fMessages.push_back({{"role", "assistant"},
			                     {"content", fPendingAssistantText}});
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
	}
}

bool ChatWindow::QuitRequested()
{
	if (fWorkerRunning.load() && fWorker.joinable()) {
		g_interrupted = 1;
		fWorker.join();
		delete fSink;
		fSink = nullptr;
	}
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// _ProcessChunk — fenced-code-block detection
//
// The streaming arrives token-by-token. We append each character to
// fLineBuffer. On newline we check whether the complete line is a fence
// (starts with "```"). When we find an opening fence we switch to
// buffering mode; on the closing fence we flush to a BScintillaView.
// ---------------------------------------------------------------------------

static bool IsFence(const std::string& line, std::string& lang)
{
	if (line.size() < 3) return false;
	if (line[0] != '`' || line[1] != '`' || line[2] != '`') return false;
	// Closing fence: exactly "```" (possibly with trailing whitespace).
	lang.clear();
	for (size_t i = 3; i < line.size(); ++i) {
		if (line[i] == '\n' || line[i] == '\r' || line[i] == ' ') break;
		lang += line[i];
	}
	return true;
}

void ChatWindow::_ProcessChunk(const std::string& chunk)
{
	for (char c : chunk) {
		fLineBuffer += c;
		if (c != '\n') continue;

		const std::string line = fLineBuffer;
		fLineBuffer.clear();

		std::string lang;
		if (!fInCodeBlock) {
			if (IsFence(line, lang)) {
				// Flush any buffered markdown before entering code block.
				if (fMdRenderer) fMdRenderer->Flush();
				fInCodeBlock = true;
				fCodeLang    = lang;
				fCodeBuffer.clear();
			} else {
				// Route through markdown renderer.
				if (fMdRenderer) {
					fMdRenderer->Write(line);
				} else {
					_AppendText(line);
				}
			}
		} else {
			if (IsFence(line, lang) && lang.empty()) {
				fInCodeBlock = false;
				_FlushCodeBlock();
			} else {
				fCodeBuffer += line;
			}
		}
	}

	// Partial line: pass to markdown renderer (it buffers until '\n').
	if (!fLineBuffer.empty() && !fInCodeBlock) {
		if (fMdRenderer) {
			fMdRenderer->Write(fLineBuffer);
			fLineBuffer.clear();
		}
		// If no renderer, keep in fLineBuffer and emit later.
	}
}

// ---------------------------------------------------------------------------
// _FlushCodeBlock — render the accumulated code in a BScintillaView
// ---------------------------------------------------------------------------

void ChatWindow::_FlushCodeBlock()
{
	if (fCodeBuffer.empty()) return;

	// Calculate an appropriate height: 1 line ≈ 16px, min 60px, max 400px.
	const int lines  = std::max(1, CountLines(fCodeBuffer) + 1);
	const int height = std::min(400, std::max(60, lines * 16 + 8));

	// The BScintillaView is added as a child of the output BTextView.
	// We use a BScrollView wrapper for horizontal scrolling on wide code.
	// Position: appended after the current text, left-aligned, full width.
	const float viewWidth = fOutput->Bounds().Width() - 16.0f;
	const float yPos      = fOutput->TextRect().bottom + 4.0f;

	BScintillaView* sci = new BScintillaView(
		"code", B_WILL_DRAW | B_NAVIGABLE, false, true, B_FANCY_BORDER);
	sci->MoveTo(8.0f, yPos);
	sci->ResizeTo(viewWidth, static_cast<float>(height));

	fOutput->AddChild(sci);
	fCodeViews.push_back(sci);

	// Configure via SCI_* messages.
	auto send = [sci](unsigned int msg, unsigned long w, long l) -> long {
		return sci->SendMessage(msg, w, l);
	};

	if (fStyler) {
		fStyler->Apply(send, fCodeLang);
	} else {
		// No theme: plain dark background, light text.
		// SCI_STYLESETFORE/BACK on STYLE_DEFAULT = 32.
		send(2051, 32, static_cast<long>(styling::CodeStyler::ParseColor("#DCDCDC")));
		send(2052, 32, static_cast<long>(styling::CodeStyler::ParseColor("#1E1E1E")));
		send(2050, 0, 0); // STYLECLEARALL
	}

	// Use fixed-width font (SCI_STYLESETFONT).
	send(2056 + 1, 32, reinterpret_cast<long>("Noto Mono"));

	// Set the code text.
	sci->SetText(fCodeBuffer.c_str());

	// Make read-only.
	send(2171, 1, 0); // SCI_SETREADONLY

	// Grow the BTextView's rect so the Scintilla view doesn't overlap text.
	BRect tr = fOutput->TextRect();
	tr.bottom += static_cast<float>(height) + 8.0f;
	fOutput->SetTextRect(tr);

	ScrollToBottom(fOutput);
	fCodeBuffer.clear();
	fCodeLang.clear();
}

// ---------------------------------------------------------------------------
// _SendTurn / _LaunchWorker / _HandlePermRequest
// ---------------------------------------------------------------------------

void ChatWindow::_SendTurn()
{
	const char* raw = fInput->Text();
	if (!raw || raw[0] == '\0') return;
	const std::string userText(raw);
	fInput->SetText("");

	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel);
	_AppendText(userText + "\n");
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel);

	_LaunchWorker(userText);
}

void ChatWindow::_LaunchWorker(const std::string& userText)
{
	fPendingUserText      = userText;
	fPendingAssistantText.clear();
	fInCodeBlock          = false;
	fCodeBuffer.clear();
	fLineBuffer.clear();
	fInWebFetch           = false;
	fWebFetchBuf.clear();

	fSend->SetEnabled(false);
	fInput->SetEnabled(false);
	fStatus->SetText("thinking\xE2\x80\xA6");
	fWorkerRunning.store(true);

	const config::Auth   auth         = fAuth;
	const std::string    model        = fModel;
	const int            maxTokens    = fMaxTokens;
	const std::string    systemPrompt = config::ComposeSystem(fSystemPrompt);
	nlohmann::json       messages     = fMessages;
	messages.push_back({{"role", "user"}, {"content", userText}});

	fSink = new gui::GuiSink(BMessenger(this));
	gui::GuiSink* sink = fSink;
	sink->BeginMessage("assistant");

	fWorker = std::thread([=]() {
		api::SendWithTools(auth, model, maxTokens,
		                   const_cast<nlohmann::json&>(messages),
		                   systemPrompt, sink);
		sink->EndMessage();
		BMessenger(this).SendMessage(gui::MSG_WORKER_DONE);
	});
	fWorker.detach();
}

void ChatWindow::_HandlePermRequest(BMessage* msg)
{
	const char* tool    = nullptr;
	const char* preview = nullptr;
	msg->FindString("tool",    &tool);
	msg->FindString("preview", &preview);

	std::string body = "Allow tool: ";
	body += tool ? tool : "(unknown)";
	if (preview && preview[0]) {
		body += "\n\n";
		const std::string pv(preview);
		body += (pv.size() > 400) ? pv.substr(0, 400) + "\xE2\x80\xA6" : pv;
	}

	BAlert* alert = new BAlert("Tool Permission", body.c_str(),
	    "Deny", "Allow", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	const int32 choice  = alert->Go();
	if (fSink) fSink->DeliverPermissionReply(choice == 1);
}
