// chat_window.cpp — Claude GUI main window implementation.
//
// Three native widget classes are defined here before ChatWindow:
//   InputView    — multi-line input with history, placeholder, Shift+Enter.
//   TokenBar     — thin coloured fill bar showing context usage.
//   SettingsPanel — slide-in panel for system prompt / model config.
//   SpinnerView  — animated thinking indicator.
//
// All view mutations happen on the main thread inside MessageReceived or
// layout callbacks. The worker thread communicates exclusively via BMessages.

#include "chat_window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <Alert.h>
#include <Application.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <File.h>
#include <Font.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <ScrollBar.h>
#include <SeparatorView.h>
#include <SpaceLayoutItem.h>
#include <StringItem.h>
#include <TextView.h>
#include <Window.h>

#include "api.h"
#include "code_styler.h"
#include "config.h"
#include "gui_sink.h"
#include "md_renderer.h"
#include "scintilla_view.h"

// ---------------------------------------------------------------------------
// Colour helpers — prefer ui_color() for theme-aware values.
// Hard-coded values are used only for chat-content colours that intentionally
// stay dark regardless of the system theme (the output area is always dark).
// ---------------------------------------------------------------------------
namespace {

// Output area colours (always dark regardless of system theme).
const rgb_color kColorChatBg      = {  24,  24,  28, 255 };
const rgb_color kColorText        = { 215, 215, 220, 255 };
const rgb_color kColorUserLabel   = {  86, 180, 233, 255 };
const rgb_color kColorModelLabel  = { 204, 121,  90, 255 };
const rgb_color kColorToolLine    = { 130, 130, 140, 255 };
const rgb_color kColorError       = { 230,  75,  75, 255 };

// Known Anthropic models listed in the model picker.
const char* kKnownModels[] = {
	"claude-opus-4-5",
	"claude-sonnet-4-5",
	"claude-haiku-4-5",
	"claude-opus-4",
	"claude-sonnet-4",
	"claude-3-7-sonnet-20250219",
	"claude-3-5-sonnet-20241022",
	"claude-3-5-haiku-20241022",
	"claude-3-opus-20240229",
	nullptr
};

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
	tra->count          = 1;
	tra->runs[0].offset = 0;
	tra->runs[0].font   = font;
	tra->runs[0].color  = color;
	const int32 start   = view->TextLength();
	view->Insert(start, text.c_str(), static_cast<int32>(text.size()), tra);
	free(tra);
}

int CountLines(const std::string& s)
{
	int n = 0;
	for (char c : s) if (c == '\n') ++n;
	return n;
}

} // namespace


// ===========================================================================
// InputView
// ===========================================================================

InputView::InputView(const char* name)
	: BTextView(name, B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS)
{
	SetWordWrap(true);
	SetStylable(false);
}

void InputView::AttachedToWindow()
{
	BTextView::AttachedToWindow();
	// Use system document colours for the input area.
	SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
	// A slightly larger font than the default for comfortable typing.
	BFont f(be_plain_font);
	f.SetSize(f.Size() + 1.0f);
	SetFontAndColor(&f);
}

void InputView::Draw(BRect updateRect)
{
	BTextView::Draw(updateRect);
	// Draw placeholder text when empty and unfocused.
	if (TextLength() == 0 && !fFocused)
		_DrawPlaceholder();
	// Draw focus ring.
	if (fFocused) {
		SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
		StrokeRect(Bounds().InsetByCopy(-1, -1));
	}
}

void InputView::_DrawPlaceholder()
{
	const char* placeholder = "Message Claude\xE2\x80\xA6"; // ellipsis
	BFont f;
	GetFont(&f);
	rgb_color dim = tint_color(ui_color(B_DOCUMENT_TEXT_COLOR), B_LIGHTEN_2_TINT);
	SetHighColor(dim);
	BPoint pen(TextRect().left, TextRect().top + f.Size());
	DrawString(placeholder, pen);
	// Restore.
	SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
}

void InputView::MakeFocus(bool focused)
{
	BTextView::MakeFocus(focused);
	fFocused = focused;
	Invalidate();
}

void InputView::KeyDown(const char* bytes, int32 numBytes)
{
	if (numBytes == 1) {
		// Enter: send unless Shift is held.
		if (bytes[0] == B_ENTER || bytes[0] == B_RETURN) {
			if (modifiers() & B_SHIFT_KEY) {
				// Shift+Enter → insert newline.
				BTextView::KeyDown(bytes, numBytes);
				if (Window()) Window()->PostMessage(gui::MSG_JUMP_BOTTOM);
			} else {
				// Plain Enter → send.
				if (Window()) Window()->PostMessage(gui::MSG_SEND);
			}
			return;
		}
		// Escape → cancel.
		if (bytes[0] == B_ESCAPE) {
			if (Window()) Window()->PostMessage(gui::MSG_CANCEL);
			return;
		}
	}

	// Up/Down when the caret is on the first/last line → history.
	if (numBytes == 3 && bytes[0] == '\x1B') {
		if (bytes[2] == 'A') { _HistoryUp();   return; }
		if (bytes[2] == 'B') { _HistoryDown(); return; }
	}

	BTextView::KeyDown(bytes, numBytes);
	Invalidate(); // repaint placeholder if text becomes empty
}

void InputView::FrameResized(float w, float h)
{
	BTextView::FrameResized(w, h);
	SetTextRect(Bounds().InsetByCopy(4.0f, 4.0f));
}

float InputView::PreferredHeight() const
{
	BFont f;
	GetFont(&f);
	font_height fh;
	f.GetHeight(&fh);
	const float lineH = ceilf(fh.ascent + fh.descent + fh.leading) + 1.0f;
	const std::string content(Text(), static_cast<size_t>(TextLength()));
	const int   lines = std::max(kMinLines,
	                             std::min(kMaxLines,
	                                      ::CountLines(content) + 1));
	return lineH * static_cast<float>(lines) + 8.0f;
}

void InputView::SetEnabled(bool enabled)
{
	fEnabled = enabled;
	MakeEditable(enabled);
	SetViewUIColor(enabled ? B_DOCUMENT_BACKGROUND_COLOR : B_PANEL_BACKGROUND_COLOR);
	Invalidate();
}

void InputView::PushHistory(const std::string& text)
{
	if (text.empty()) return;
	// Avoid duplicating the most-recent entry.
	if (!fHistory.empty() && fHistory.back() == text) return;
	fHistory.push_back(text);
	fHistIdx = -1;
}

void InputView::LoadHistory(const std::string& path)
{
	std::ifstream f(path);
	if (!f.is_open()) return;
	std::string line;
	while (std::getline(f, line))
		if (!line.empty()) fHistory.push_back(line);
}

void InputView::SaveHistory(const std::string& path) const
{
	std::ofstream f(path, std::ios::trunc);
	if (!f.is_open()) return;
	// Write at most the last 200 entries.
	const size_t start = fHistory.size() > 200 ? fHistory.size() - 200 : 0;
	for (size_t i = start; i < fHistory.size(); ++i)
		f << fHistory[i] << '\n';
}

void InputView::_HistoryUp()
{
	if (fHistory.empty()) return;
	if (fHistIdx == -1) {
		// Save current draft.
		fDraft   = std::string(Text(), static_cast<size_t>(TextLength()));
		fHistIdx = static_cast<int>(fHistory.size()) - 1;
	} else if (fHistIdx > 0) {
		--fHistIdx;
	}
	SetText(fHistory[static_cast<size_t>(fHistIdx)].c_str());
	Select(TextLength(), TextLength());
}

void InputView::_HistoryDown()
{
	if (fHistIdx == -1) return;
	++fHistIdx;
	if (fHistIdx >= static_cast<int>(fHistory.size())) {
		fHistIdx = -1;
		SetText(fDraft.c_str());
	} else {
		SetText(fHistory[static_cast<size_t>(fHistIdx)].c_str());
	}
	Select(TextLength(), TextLength());
}


// ===========================================================================
// TokenBar
// ===========================================================================

TokenBar::TokenBar()
	: BView("tokenbar", B_WILL_DRAW)
{
	SetExplicitMinSize(BSize(B_SIZE_UNSET, static_cast<float>(kBarHeight)));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, static_cast<float>(kBarHeight)));
}

void TokenBar::Draw(BRect /*updateRect*/)
{
	const BRect r      = Bounds();
	const float filled = (fMax > 0)
		? std::min(1.0f, static_cast<float>(fUsed) / static_cast<float>(fMax))
		: 0.0f;

	// Background track.
	SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
	FillRect(r);

	// Filled bar — colour transitions green → amber → red.
	rgb_color barColor;
	if (filled < 0.6f)      barColor = {  80, 200, 120, 255 }; // green
	else if (filled < 0.85f) barColor = { 230, 170,  50, 255 }; // amber
	else                     barColor = { 220,  60,  60, 255 }; // red

	BRect fill = r;
	fill.right = r.left + r.Width() * filled;
	SetHighColor(barColor);
	FillRect(fill);

	// Divider line at top.
	SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_3_TINT));
	StrokeLine(r.LeftTop(), r.RightTop());

	// Label "42,100 / 200k  (21%)".
	auto commaNum = [](int v) -> std::string {
		std::string s   = std::to_string(v);
		const int  len  = static_cast<int>(s.size());
		int        ins  = len - 3;
		while (ins > 0) { s.insert(static_cast<size_t>(ins), ","); ins -= 3; }
		return s;
	};
	auto shortK = [](int v) -> std::string {
		if (v >= 1000) return std::to_string(v / 1000) + "k";
		return std::to_string(v);
	};

	const std::string pct  = std::to_string(static_cast<int>(filled * 100.0f + 0.5f)) + "%";
	const std::string lbl  = commaNum(fUsed) + " / " + shortK(fMax) + "  (" + pct + ")";

	BFont f(be_plain_font);
	f.SetSize(10.0f);
	SetFont(&f);
	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	float strW = f.StringWidth(lbl.c_str());
	MovePenTo(r.right - strW - 4.0f, r.bottom - 3.0f);
	DrawString(lbl.c_str());
}

void TokenBar::SetTokens(int used, int maxCtx)
{
	fUsed = used;
	if (maxCtx > 0) fMax = maxCtx;
	Invalidate();
}


// ===========================================================================
// SettingsPanel
// ===========================================================================

SettingsPanel::SettingsPanel(const std::string& systemPrompt, int maxTokens)
	: BView("settings", B_WILL_DRAW | B_SUPPORTS_LAYOUT)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	_BuildLayout(systemPrompt, maxTokens);
	// Start hidden.
	Hide();
}

void SettingsPanel::_BuildLayout(const std::string& systemPrompt, int maxTokens)
{
	// System-prompt label + editor.
	BStringView* sysLabel = new BStringView("syslbl", "System Prompt:");
	fSysPromptView        = new BTextView("sysprompt", B_WILL_DRAW | B_FRAME_EVENTS);
	fSysPromptView->SetWordWrap(true);
	fSysPromptView->SetText(systemPrompt.c_str());
	BScrollView* sysScroll = new BScrollView("sysscroll", fSysPromptView,
	                                          0, false, true, B_FANCY_BORDER);

	// Max tokens field.
	fMaxTokensCtl = new BTextControl("maxtokens", "Max tokens:",
	                                  std::to_string(maxTokens).c_str(), nullptr);

	// Close button.
	BButton* closeBtn = new BButton("closesettings", "Close",
	                                 new BMessage(gui::MSG_SETTINGS));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(sysLabel)
		.Add(sysScroll, 1.0f)
		.Add(fMaxTokensCtl)
		.Add(closeBtn)
	.End();
}

void SettingsPanel::SetValues(const std::string& systemPrompt, int maxTokens)
{
	if (fSysPromptView)
		fSysPromptView->SetText(systemPrompt.c_str());
	if (fMaxTokensCtl)
		fMaxTokensCtl->SetText(std::to_string(maxTokens).c_str());
}

std::string SettingsPanel::SystemPrompt() const
{
	if (!fSysPromptView) return {};
	const char* t = fSysPromptView->Text();
	return t ? std::string(t) : std::string();
}

int SettingsPanel::MaxTokens() const
{
	if (!fMaxTokensCtl) return 8192;
	const char* t = fMaxTokensCtl->Text();
	if (!t || t[0] == '\0') return 8192;
	int v = std::atoi(t);
	return (v > 0) ? v : 8192;
}

void SettingsPanel::Toggle()
{
	fOpen = !fOpen;
	if (fOpen)
		Show();
	else
		Hide();
}


// ===========================================================================
// SpinnerView
// ===========================================================================

SpinnerView::SpinnerView()
	: BView("spinner", B_WILL_DRAW)
{
	SetExplicitMinSize(BSize(kSize, kSize));
	SetExplicitMaxSize(BSize(kSize, kSize));
	Hide(); // starts hidden
}

void SpinnerView::Draw(BRect /*updateRect*/)
{
	if (!fVisible) return;

	const float cx = Bounds().Width() / 2.0f;
	const float cy = Bounds().Height() / 2.0f;
	const float r  = (cx < cy ? cx : cy) - 2.0f;

	SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
	StrokeArc(BPoint(cx, cy), r, r, 0.0f, 360.0f);

	// Draw a 120° arc that rotates with each tick.
	const float startAngle = static_cast<float>(fStep * 30);
	SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
	SetPenSize(2.0f);
	StrokeArc(BPoint(cx, cy), r, r, startAngle, 120.0f);
	SetPenSize(1.0f);
}

void SpinnerView::Tick()
{
	fStep = (fStep + 1) % 12;
	if (fVisible) Invalidate();
}

void SpinnerView::SetVisible(bool v)
{
	fVisible = v;
	if (v)
		Show();
	else
		Hide();
}


// ===========================================================================
// ChatWindow
// ===========================================================================

ChatWindow::ChatWindow(const config::Auth& auth, const std::string& model,
                        int maxTokens, const std::string& systemPrompt)
	: BWindow(BRect(100, 100, 900, 680), "Claude",
	           B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS)
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

	// Markdown renderer (needs fOutput to exist).
	fMdRenderer = new md::MdRenderer(fOutput);

	// Update title with model name.
	_UpdateTitle();

	// Register keyboard shortcuts.
	AddShortcut('N', B_COMMAND_KEY, new BMessage(gui::MSG_NEW_CHAT));
	AddShortcut('L', B_COMMAND_KEY, new BMessage(gui::MSG_CLEAR_OUTPUT));
	AddShortcut(',', B_COMMAND_KEY, new BMessage(gui::MSG_SETTINGS));
}

ChatWindow::~ChatWindow()
{
	delete fStyler;
	delete fMdRenderer;
	if (fWorker.joinable()) fWorker.detach();
	delete fSink;
	delete fSpinnerTimer;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ChatWindow::_BuildLayout()
{
	// ── Output BTextView (always-dark chat area) ─────────────────────────────
	fOutput = new BTextView("output", B_WILL_DRAW | B_FRAME_EVENTS);
	fOutput->MakeEditable(false);
	fOutput->MakeSelectable(true);
	fOutput->SetWordWrap(true);
	fOutput->SetStylable(true);
	fOutput->SetViewColor(kColorChatBg);
	fOutput->SetLowColor(kColorChatBg);
	fOutput->SetHighColor(kColorText);
	fOutput->SetFontAndColor(be_fixed_font, B_FONT_ALL, &kColorText);

	fScroll = new BScrollView("scroll", fOutput,
	                          B_FOLLOW_ALL, 0, false, true, B_FANCY_BORDER);

	// Floating jump-to-bottom button (overlaid, repositioned in FrameResized).
	fJumpBtn = new BButton("jumpbtn", "\xE2\x86\x93 New", // ↓
	                        new BMessage(gui::MSG_JUMP_BOTTOM));
	fJumpBtn->SetExplicitSize(BSize(80, 26));
	fJumpBtn->Hide();
	AddChild(fJumpBtn); // added directly to window, not layout

	// ── Token bar ────────────────────────────────────────────────────────────
	fTokenBar = new TokenBar();

	// ── Spinner ──────────────────────────────────────────────────────────────
	fSpinner = new SpinnerView();

	// ── Input area ───────────────────────────────────────────────────────────
	fInput = new InputView("input");
	fInputScroll = new BScrollView("inputscroll", fInput,
	                               B_FOLLOW_LEFT_RIGHT | B_FOLLOW_BOTTOM,
	                               0, false, true, B_FANCY_BORDER);
	fInputScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 54));
	fInputScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 180));

	// ── Buttons ──────────────────────────────────────────────────────────────
	fSend = new BButton("send", "Send", new BMessage(gui::MSG_SEND));
	fSend->MakeDefault(true);

	fStop = new BButton("stop", "Stop", new BMessage(gui::MSG_CANCEL));
	fStop->Hide(); // hidden until busy

	// ── Model picker ─────────────────────────────────────────────────────────
	fModelMenu  = new BPopUpMenu(fModel.c_str());
	fModelField = new BMenuField("modelpicker", "Model:", fModelMenu);
	_PopulateModelMenu();

	// ── Secondary toolbar buttons ─────────────────────────────────────────────
	fNewBtn      = new BButton("newbtn",      "New",      new BMessage(gui::MSG_NEW_CHAT));
	fClearBtn    = new BButton("clearbtn",    "Clear",    new BMessage(gui::MSG_CLEAR_OUTPUT));
	fSettingsBtn = new BButton("settingsbtn", "\xE2\x9A\x99", // ⚙
	                            new BMessage(gui::MSG_SETTINGS));
	fSettingsBtn->SetToolTip("Settings (Cmd+,)");

	// ── Settings panel ────────────────────────────────────────────────────────
	fSettings = new SettingsPanel(fSystemPrompt, fMaxTokens);

	// ── Layout ────────────────────────────────────────────────────────────────
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fScroll, 1.0f)
			.Add(fSettings, 0.0f)
		.End()
		.Add(fTokenBar, 0.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_SMALL_INSETS, 4, B_USE_SMALL_INSETS, 4)
			.Add(fSpinner, 0.0f)
			.Add(fInputScroll, 1.0f)
			.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
				.Add(fSend)
				.Add(fStop)
			.End()
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_SMALL_INSETS, 0, B_USE_SMALL_INSETS, B_USE_SMALL_INSETS)
			.Add(fModelField, 1.0f)
			.Add(fNewBtn)
			.Add(fClearBtn)
			.Add(fSettingsBtn)
		.End()
	.End();

	SetSizeLimits(420, 32767, 300, 32767);

	// Give input focus on startup.
	fInput->MakeFocus(true);
}

void ChatWindow::_PopulateModelMenu()
{
	// Add known models.
	for (int i = 0; kKnownModels[i] != nullptr; ++i) {
		BMessage* msg = new BMessage(gui::MSG_MODEL_PICK);
		msg->AddString("model", kKnownModels[i]);
		BMenuItem* item = new BMenuItem(kKnownModels[i], msg);
		// Check the current model.
		if (fModel == kKnownModels[i])
			item->SetMarked(true);
		fModelMenu->AddItem(item);
	}
	// If the current model is not in the list, add it at the top.
	bool found = false;
	for (int i = 0; kKnownModels[i] != nullptr; ++i)
		if (fModel == kKnownModels[i]) { found = true; break; }
	if (!found) {
		BMessage* msg = new BMessage(gui::MSG_MODEL_PICK);
		msg->AddString("model", fModel.c_str());
		BMenuItem* item = new BMenuItem(fModel.c_str(), msg);
		item->SetMarked(true);
		fModelMenu->AddItem(item, 0);
	}
	fModelMenu->SetRadioMode(true);
}

void ChatWindow::_RepositionOverlays()
{
	if (!fJumpBtn || !fScroll) return;
	const BRect sb = fScroll->Frame();
	const float bw = fJumpBtn->Frame().Width();
	const float bh = fJumpBtn->Frame().Height();
	fJumpBtn->MoveTo(sb.right - bw - 12.0f, sb.bottom - bh - 12.0f);
}

void ChatWindow::FrameResized(float w, float h)
{
	BWindow::FrameResized(w, h);
	_RepositionOverlays();
}

// ---------------------------------------------------------------------------
// MessageReceived
// ---------------------------------------------------------------------------

void ChatWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {

	// ── User initiated actions ───────────────────────────────────────────────
	case gui::MSG_SEND:
		if (!fWorkerRunning.load()) _SendTurn();
		break;

	case gui::MSG_CANCEL:
		_CancelWorker();
		break;

	case gui::MSG_NEW_CHAT:
		_NewChat();
		break;

	case gui::MSG_CLEAR_OUTPUT:
		_ClearOutput();
		break;

	case gui::MSG_JUMP_BOTTOM:
		_ScrollToBottom();
		fJumpBtn->Hide();
		fUserScrolled = false;
		break;

	case gui::MSG_SETTINGS:
		fSettings->Toggle();
		break;

	case gui::MSG_MODEL_PICK: {
		const char* model = nullptr;
		if (msg->FindString("model", &model) == B_OK && model) {
			fModel = model;
			_UpdateTitle();
		}
		break;
	}

	// ── Spinner tick ─────────────────────────────────────────────────────────
	case gui::MSG_TICK:
		if (fSpinner) fSpinner->Tick();
		break;

	// ── Worker → window messages ─────────────────────────────────────────────
	case gui::MSG_CHUNK: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			if (fInWebFetch) {
				fWebFetchBuf += text;
				// If clearly not HTML after 40 chars, emit directly.
				if (fWebFetchBuf.find("text/html") == std::string::npos &&
				    fWebFetchBuf.find("<html")     == std::string::npos &&
				    fWebFetchBuf.find("<HTML")     == std::string::npos &&
				    fWebFetchBuf.size() > 40) {
					fInWebFetch = false;
					if (fMdRenderer) fMdRenderer->Write(fWebFetchBuf);
					else             _AppendText(fWebFetchBuf);
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
		if (fInCodeBlock) { fInCodeBlock = false; _FlushCodeBlock(); }
		if (fMdRenderer)  fMdRenderer->Flush();
		fLineBuffer.clear();
		fInWebFetch = false;
		fWebFetchBuf.clear();
		break;

	case gui::MSG_TOOL_START: {
		const char* name    = nullptr;
		const char* summary = nullptr;
		msg->FindString("name",    &name);
		msg->FindString("summary", &summary);
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
		if (fInWebFetch) {
			fInWebFetch = false;
			if (!fWebFetchBuf.empty()) {
				const std::string stripped = md::StripHtml(fWebFetchBuf);
				if (fMdRenderer) fMdRenderer->Write(stripped + "\n");
				else             _AppendText(stripped + "\n");
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
				SetTitle("Claude \xE2\x80\x94 thinking\xE2\x80\xA6");
				break;
			case sink::StatusKind::kCallingTool:
				SetTitle("Claude \xE2\x80\x94 running tool\xE2\x80\xA6");
				break;
			case sink::StatusKind::kIdle:
				_UpdateTitle();
				break;
		}
		break;
	}

	case gui::MSG_ERR: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			AppendWithColor(fOutput,
			    std::string("\n\xE2\x9A\xA0 ") + text + "\n", // ⚠
			    kColorError);
			_ScrollToBottom();
		}
		break;
	}

	case gui::MSG_TOKENS: {
		int32 input  = 0;
		int32 output = 0;
		int32 maxCtx = 0;
		msg->FindInt32("input",  &input);
		msg->FindInt32("output", &output);
		msg->FindInt32("max",    &maxCtx);
		fSessionInputTokens  += input;
		fSessionOutputTokens += output;
		if (fTokenBar) fTokenBar->SetTokens(fSessionInputTokens, maxCtx > 0 ? maxCtx : fMaxTokens);
		break;
	}

	case gui::MSG_WORKER_DONE: {
		fWorkerRunning.store(false);
		if (fWorker.joinable()) fWorker.join();
		delete fSink;
		fSink = nullptr;

		// Stop spinner.
		delete fSpinnerTimer;
		fSpinnerTimer = nullptr;
		if (fSpinner) fSpinner->SetVisible(false);

		// Flush any open code block.
		if (fInCodeBlock) { fInCodeBlock = false; _FlushCodeBlock(); }
		if (fMdRenderer)  fMdRenderer->Flush();
		fLineBuffer.clear();
		fInWebFetch = false;
		fWebFetchBuf.clear();

		// Commit turn to history.
		if (!fPendingUserText.empty())
			fMessages.push_back({{"role", "user"},
			                     {"content", fPendingUserText}});
		if (!fPendingAssistantText.empty())
			fMessages.push_back({{"role", "assistant"},
			                     {"content", fPendingAssistantText}});
		fPendingUserText.clear();
		fPendingAssistantText.clear();

		++fTurnCount;
		_SetBusy(false);
		_UpdateTitle();

		// Desktop notification when the window is not active.
		if (!IsActive()) {
			BNotification notif(B_INFORMATION_NOTIFICATION);
			notif.SetGroup("Claude");
			notif.SetTitle("Response ready");
			notif.SetMessageID("claude-response");
			notif.Send();
		}
		break;
	}

	default:
		BWindow::MessageReceived(msg);
	}
}

bool ChatWindow::QuitRequested()
{
	if (fWorkerRunning.load()) {
		g_interrupted = 1;
		if (fWorker.joinable()) fWorker.join();
		delete fSink;
		fSink = nullptr;
	}
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

void ChatWindow::_AppendText(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorText);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_AppendToolLine(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorToolLine);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_ScrollToBottom()
{
	fOutput->ScrollToOffset(fOutput->TextLength());
}

bool ChatWindow::_IsNearBottom() const
{
	const BScrollBar* sb = fScroll->ScrollBar(B_VERTICAL);
	if (!sb) return true;
	float sbMin = 0.0f, sbMax = 0.0f;
	sb->GetRange(&sbMin, &sbMax);
	return (sbMax - sb->Value()) < 24.0f;
}

// ---------------------------------------------------------------------------
// _ProcessChunk — fenced-code-block detection
// ---------------------------------------------------------------------------

static bool IsFence(const std::string& line, std::string& lang)
{
	if (line.size() < 3) return false;
	if (line[0] != '`' || line[1] != '`' || line[2] != '`') return false;
	lang.clear();
	for (size_t i = 3; i < line.size(); ++i) {
		const char c = line[i];
		if (c == '\n' || c == '\r' || c == ' ') break;
		lang += c;
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
				if (fMdRenderer) fMdRenderer->Flush();
				fInCodeBlock = true;
				fCodeLang    = lang;
				fCodeBuffer.clear();
			} else {
				if (fMdRenderer) fMdRenderer->Write(line);
				else             _AppendText(line);
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

	// Partial line — pass to markdown renderer (buffers until '\n').
	if (!fLineBuffer.empty() && !fInCodeBlock) {
		if (fMdRenderer) {
			fMdRenderer->Write(fLineBuffer);
			fLineBuffer.clear();
		}
	}
}

// ---------------------------------------------------------------------------
// _FlushCodeBlock — render accumulated code in a BScintillaView
// ---------------------------------------------------------------------------

void ChatWindow::_FlushCodeBlock()
{
	if (fCodeBuffer.empty()) return;

	const int   lines  = std::max(1, CountLines(fCodeBuffer) + 1);
	const int   height = std::min(400, std::max(60, lines * 16 + 8));
	const float viewW  = fOutput->Bounds().Width() - 16.0f;
	const float yPos   = fOutput->TextRect().bottom + 4.0f;

	BScintillaView* sci = new BScintillaView(
		"code", B_WILL_DRAW | B_NAVIGABLE, false, true, B_FANCY_BORDER);
	sci->MoveTo(8.0f, yPos);
	sci->ResizeTo(viewW, static_cast<float>(height));
	fOutput->AddChild(sci);
	fCodeViews.push_back(sci);

	auto send = [sci](unsigned int m, unsigned long w, long l) -> long {
		return sci->SendMessage(m, w, l);
	};

	if (fStyler) {
		fStyler->Apply(send, fCodeLang);
	} else {
		send(2051, 32, static_cast<long>(styling::CodeStyler::ParseColor("#DCDCDC")));
		send(2052, 32, static_cast<long>(styling::CodeStyler::ParseColor("#1E1E1E")));
		send(2050, 0, 0);
	}
	send(2056 + 1, 32, reinterpret_cast<long>("Noto Mono"));
	sci->SetText(fCodeBuffer.c_str());
	send(2171, 1, 0); // SCI_SETREADONLY

	BRect tr = fOutput->TextRect();
	tr.bottom += static_cast<float>(height) + 8.0f;
	fOutput->SetTextRect(tr);

	if (!fUserScrolled) _ScrollToBottom();
	fCodeBuffer.clear();
	fCodeLang.clear();
}

// ---------------------------------------------------------------------------
// Turn lifecycle
// ---------------------------------------------------------------------------

void ChatWindow::_SendTurn()
{
	const char* raw = fInput->Text();
	if (!raw || raw[0] == '\0') return;
	const std::string userText(raw);
	fInput->SetText("");
	fInput->PushHistory(userText);

	// If this is the first turn, set the conversation topic for the title.
	if (fConvTopic.empty()) {
		fConvTopic = userText.size() > 60
		    ? userText.substr(0, 57) + "\xE2\x80\xA6"
		    : userText;
	}

	// Emit user label + text into the output.
	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel);   // ▸
	_AppendText(userText + "\n");
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel);

	_LaunchWorker(userText);
}

void ChatWindow::_LaunchWorker(const std::string& userText)
{
	fPendingUserText.clear();
	fPendingAssistantText.clear();
	fPendingUserText = userText;
	fInCodeBlock     = false;
	fCodeBuffer.clear();
	fLineBuffer.clear();
	fInWebFetch      = false;
	fWebFetchBuf.clear();

	_SetBusy(true);

	// Start spinner timer (80ms ticks).
	if (fSpinner) fSpinner->SetVisible(true);
	BMessage tickMsg(gui::MSG_TICK);
	fSpinnerTimer = new BMessageRunner(BMessenger(this), &tickMsg, 80000LL);

	const config::Auth   auth         = fAuth;
	const std::string    model        = fModel;
	const int            maxTokens    = fMaxTokens;
	const std::string    systemPrompt = config::ComposeSystem(fSystemPrompt);
	nlohmann::json       messages     = fMessages;
	messages.push_back({{"role", "user"}, {"content", userText}});

	fSink = new gui::GuiSink(BMessenger(this));
	gui::GuiSink* sink = fSink;
	sink->BeginMessage("assistant");

	fWorkerRunning.store(true);
	fWorker = std::thread([=]() {
		api::SendWithTools(auth, model, maxTokens,
		                   const_cast<nlohmann::json&>(messages),
		                   systemPrompt, sink);
		sink->EndMessage();
		BMessenger(this).SendMessage(gui::MSG_WORKER_DONE);
	});
	fWorker.detach();
}

void ChatWindow::_CancelWorker()
{
	if (!fWorkerRunning.load()) return;
	g_interrupted = 1;
	// _SetBusy(false) will be called when MSG_WORKER_DONE arrives.
}

void ChatWindow::_NewChat()
{
	_CancelWorker();
	_ClearOutput();
	fMessages    = nlohmann::json::array();
	fTurnCount   = 0;
	fConvTopic.clear();
	fSessionInputTokens  = 0;
	fSessionOutputTokens = 0;
	if (fTokenBar) fTokenBar->SetTokens(0, fMaxTokens);
	_UpdateTitle();
}

void ChatWindow::_ClearOutput()
{
	fOutput->SetText("");
	// Remove all embedded code views.
	for (BView* v : fCodeViews) {
		fOutput->RemoveChild(v);
		delete v;
	}
	fCodeViews.clear();
	// Reset the TextRect to fit the now-empty view.
	fOutput->SetTextRect(fOutput->Bounds().InsetByCopy(4, 4));
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
	const int32 choice = alert->Go();
	if (fSink) fSink->DeliverPermissionReply(choice == 1);
}

// ---------------------------------------------------------------------------
// Toolbar / UI state
// ---------------------------------------------------------------------------

void ChatWindow::_SetBusy(bool busy)
{
	if (busy) {
		fSend->Hide();
		fStop->Show();
		fInput->SetEnabled(false);
	} else {
		fStop->Hide();
		fSend->Show();
		fInput->SetEnabled(true);
		fInput->MakeFocus(true);
	}
}

void ChatWindow::_UpdateTitle()
{
	std::string title = "Claude \xE2\x80\x94 " + fModel; // em dash
	if (!fConvTopic.empty())
		title += " \xE2\x80\x94 " + fConvTopic;
	SetTitle(title.c_str());
}
