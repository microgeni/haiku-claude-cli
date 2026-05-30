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
#include <OS.h>
#include <ScrollBar.h>
#include <SeparatorView.h>
#include <SpaceLayoutItem.h>
#include <StringItem.h>
#include <Window.h>

#include <ListView.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StringItem.h>
#include "api.h"
#include "code_styler.h"
#include "commands.h"
#include "config.h"
#include "gui_sink.h"
#include "md_renderer.h"
#include "models.h"

// ---------------------------------------------------------------------------
// Colour helpers — prefer ui_color() for theme-aware values.
// Hard-coded values are used only for chat-content colours that intentionally
// stay dark regardless of the system theme (the output area is always dark).
// ---------------------------------------------------------------------------
namespace {

// Slider for the notification delay. Snaps to 10-second steps and shows a
// plain-English label ("Notify is disabled" / "Notify after 30s") that
// updates live as the thumb moves.
class NotifySlider : public BSlider {
public:
	NotifySlider(const char* name, const char* label, BMessage* message,
	             int32 minValue, int32 maxValue)
		: BSlider(name, label, message, minValue, maxValue, B_HORIZONTAL)
	{
	}

	// Attach a left-aligned label that mirrors the slider value.
	void SetValueLabel(BStringView* label)
	{
		fLabel = label;
		_UpdateLabel();
	}

	// Round every value change to the nearest 10 seconds.
	virtual void SetValue(int32 value) override
	{
		int32 snapped = ((value + 5) / 10) * 10;
		BSlider::SetValue(snapped);
		_UpdateLabel();
	}

	// Suppress the slider's built-in right-aligned value text; the
	// dedicated left-aligned BStringView shows the value instead.
	virtual const char* UpdateText() const override
	{
		return nullptr;
	}

	// Render a notify delay (in seconds) as a friendly phrase:
	//   0   -> "Notify is disabled"
	//   30  -> "Notify after 30s"
	//   60  -> "Notify after 1 minute"
	//   90  -> "Notify after 1 min 30s"
	//   120 -> "Notify after 2 minutes"
	static void FormatNotifyDelay(int32 seconds, char* out, size_t outSize)
	{
		if (seconds <= 0) {
			std::snprintf(out, outSize, "Notify is disabled");
			return;
		}
		const int mins = static_cast<int>(seconds) / 60;
		const int secs = static_cast<int>(seconds) % 60;
		if (mins == 0)
			std::snprintf(out, outSize, "Notify after %ds", secs);
		else if (secs == 0)
			std::snprintf(out, outSize, "Notify after %d %s",
			              mins, mins == 1 ? "minute" : "minutes");
		else
			std::snprintf(out, outSize, "Notify after %d min %ds", mins, secs);
	}

private:
	void _UpdateLabel()
	{
		if (!fLabel) return;
		char text[40];
		FormatNotifyDelay(Value(), text, sizeof(text));
		fLabel->SetText(text);
	}

	BStringView* fLabel = nullptr;
};

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

int CountLines(const std::string& s)
{
	int n = 0;
	for (char c : s) if (c == '\n') ++n;
	return n;
}

} // namespace


// ===========================================================================
// CommandPopup  (BPopUpMenu wrapper)
// ===========================================================================

void CommandPopup::Show(const std::string& prefix, BPoint screenPt)
{
	static const char* kBuiltins[] = {
		"/help", "/clear", "/new", "/model", "/compact",
		"/memory", "/usage", "/version", nullptr
	};

	// Collect matching commands.
	std::vector<std::string> matches;
	for (const auto& name : commands::Names()) {
		if (name.size() >= prefix.size() &&
		    name.substr(0, prefix.size()) == prefix)
			matches.push_back(name);
	}
	for (int i = 0; kBuiltins[i]; ++i) {
		const std::string b(kBuiltins[i]);
		if (b.size() >= prefix.size() &&
		    b.substr(0, prefix.size()) == prefix) {
			bool dup = false;
			for (const auto& m : matches) if (m == b) { dup = true; break; }
			if (!dup) matches.push_back(b);
		}
	}
	std::sort(matches.begin(), matches.end());
	if (matches.empty()) return;

	// Build the BPopUpMenu.
	BPopUpMenu* menu = new BPopUpMenu("commands", false, false);
	for (const auto& m : matches) {
		BMessage* msg = new BMessage(gui::MSG_COMPLETE_CMD);
		msg->AddString("cmd", m.c_str());
		menu->AddItem(new BMenuItem(m.c_str(), msg));
	}
	menu->SetTargetForItems(fTarget);

	// Go() runs a nested event loop — safe from MessageReceived context.
	// It blocks until the user picks or dismisses.
	fVisible = true;
	menu->Go(screenPt, true, true, true);
	fVisible = false;
	delete menu;
}

// ===========================================================================
// InputView
// ===========================================================================

InputView::InputView(const char* name)
	: BTextView(BRect(0, 0, 200, 60), name,
	            BRect(4, 4, 196, 56),
	            B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS)
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
	// Set text rect now that we have a real frame.
	SetTextRect(Bounds().InsetByCopy(4.0f, 4.0f));
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
	// Draw drop-target highlight when a file is dragged over.
	if (fDropTarget) {
		SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
		SetPenSize(2.0f);
		StrokeRect(Bounds().InsetByCopy(1, 1));
		SetPenSize(1.0f);
	}
}

void InputView::MouseMoved(BPoint /*where*/, uint32 transit,
                            const BMessage* drag)
{
	const bool wasDrop = fDropTarget;
	if (drag != nullptr) {
		fDropTarget = (transit == B_ENTERED_VIEW || transit == B_INSIDE_VIEW);
	} else {
		fDropTarget = false;
	}
	if (fDropTarget != wasDrop) Invalidate();
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
		if (bytes[0] == B_ENTER || bytes[0] == B_RETURN) {
			if (modifiers() & B_SHIFT_KEY) {
				BTextView::KeyDown(bytes, numBytes);
				if (Window()) Window()->PostMessage(gui::MSG_JUMP_BOTTOM);
			} else {
				if (Window()) Window()->PostMessage(gui::MSG_SEND);
			}
			return;
		}
		if (bytes[0] == B_ESCAPE) {
			if (Window()) Window()->PostMessage(gui::MSG_CANCEL);
			return;
		}
	}

	// Up/Down → history.
	if (numBytes == 3 && bytes[0] == '\x1B') {
		if (bytes[2] == 'A') { _HistoryUp();   return; }
		if (bytes[2] == 'B') { _HistoryDown(); return; }
	}

	BTextView::KeyDown(bytes, numBytes);

	// After inserting '/' as the first character, show the command popup.
	// BPopUpMenu::Go() handles its own keyboard navigation.
	if (Window()) {
		const std::string txt(Text(), static_cast<size_t>(TextLength()));
		if (!txt.empty() && txt[0] == '/') {
			BMessage upd(gui::MSG_POPUP_UPDATE);
			upd.AddString("prefix", txt.c_str());
			Window()->PostMessage(&upd);
		}
	}

	Invalidate();
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

SettingsPanel::SettingsPanel(const std::string& systemPrompt, int maxTokens,
                             int notifyMinSec)
	: BView("settings", B_WILL_DRAW | B_SUPPORTS_LAYOUT)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	_BuildLayout(systemPrompt, maxTokens, notifyMinSec);
	// Start hidden.
	Hide();
}

void SettingsPanel::_BuildLayout(const std::string& systemPrompt, int maxTokens,
                                 int notifyMinSec)
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

	// Notification delay slider: how long a turn must run before it
	// fires a desktop notification. 0 = notifications disabled. A
	// left-aligned label mirrors the value in plain English.
	BStringView* notifyLabel = new BStringView("notifylbl", "");
	notifyLabel->SetAlignment(B_ALIGN_LEFT);
	NotifySlider* notifySlider = new NotifySlider("notifydelay", nullptr,
	                                              nullptr, 0, 300);
	fNotifyDelay = notifySlider;
	fNotifyDelay->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fNotifyDelay->SetHashMarkCount(7);
	fNotifyDelay->SetKeyIncrementValue(10);
	if (notifyMinSec < 0)   notifyMinSec = 0;
	if (notifyMinSec > 300) notifyMinSec = 300;
	fNotifyDelay->SetValue(notifyMinSec);
	notifySlider->SetValueLabel(notifyLabel);

	// Close button.
	BButton* closeBtn = new BButton("closesettings", "Close",
	                                 new BMessage(gui::MSG_SETTINGS));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(sysLabel)
		.Add(sysScroll, 1.0f)
		.Add(fMaxTokensCtl)
		.Add(notifyLabel)
		.Add(fNotifyDelay)
		.Add(closeBtn)
	.End();
}

void SettingsPanel::SetValues(const std::string& systemPrompt, int maxTokens,
                              int notifyMinSec)
{
	if (fSysPromptView)
		fSysPromptView->SetText(systemPrompt.c_str());
	if (fMaxTokensCtl)
		fMaxTokensCtl->SetText(std::to_string(maxTokens).c_str());
	if (fNotifyDelay) {
		if (notifyMinSec < 0)   notifyMinSec = 0;
		if (notifyMinSec > 300) notifyMinSec = 300;
		fNotifyDelay->SetValue(notifyMinSec);
	}
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

bool SettingsPanel::NotificationsEnabled() const
{
	// A delay of 0 means notifications are turned off entirely.
	if (!fNotifyDelay) return true;
	return fNotifyDelay->Value() > 0;
}

int SettingsPanel::NotifyMinSeconds() const
{
	if (!fNotifyDelay) return 5;
	return static_cast<int>(fNotifyDelay->Value());
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
	fStep = (fStep + 11) % 12;
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
// SessionPanel
// ===========================================================================

SessionPanel::SessionPanel(BHandler* target)
	: BView("sessions", B_WILL_DRAW | B_SUPPORTS_LAYOUT)
	, fTarget(target)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	_BuildLayout();
	Hide(); // starts hidden
}

void SessionPanel::_BuildLayout()
{
	BStringView* label = new BStringView("seslbl", "Sessions");
	BFont bold(be_bold_font);
	bold.SetSize(be_plain_font->Size());
	label->SetFont(&bold);

	fList = new BListView("seslist", B_SINGLE_SELECTION_LIST);
	fList->SetSelectionMessage(new BMessage(gui::MSG_SESSION_LOAD));
	fList->SetTarget(fTarget);
	fScroll = new BScrollView("sesscroll", fList,
	                           0, false, true, B_FANCY_BORDER);

	BButton* refreshBtn = new BButton("sesrefresh", "Refresh",
	                                   new BMessage(gui::MSG_SESSIONS));
	refreshBtn->SetTarget(fTarget);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(label)
		.Add(fScroll, 1.0f)
		.Add(refreshBtn)
	.End();
}

void SessionPanel::Refresh()
{
	fSessions = session::List();

	// Repopulate the list view.
	fList->MakeEmpty();
	for (const auto& info : fSessions) {
		// Format: "Title\nmodel • N turns • date"
		char dateBuf[32];
		struct tm tm_buf;
		localtime_r(&info.modified, &tm_buf);
		std::strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", &tm_buf);

		std::string label = info.title.empty() ? "(untitled)" : info.title;
		label += "\n";
		label += info.model.empty() ? "?" : info.model;
		label += " \xE2\x80\xA2 ";  // •
		label += std::to_string(info.turns) + " turns \xE2\x80\xA2 ";
		label += dateBuf;

		fList->AddItem(new BStringItem(label.c_str()));
	}
}

void SessionPanel::Toggle()
{
	fOpen = !fOpen;
	if (fOpen) {
		Refresh();
		Show();
	} else {
		Hide();
	}
}

const session::SessionInfo* SessionPanel::InfoAt(int32_t index) const
{
	if (index < 0 || index >= static_cast<int32_t>(fSessions.size()))
		return nullptr;
	return &fSessions[static_cast<size_t>(index)];
}


// ===========================================================================
// ChatWindow
// ===========================================================================

ChatWindow::ChatWindow(const config::Auth& auth, const std::string& model,
                        int maxTokens, const std::string& systemPrompt,
                        int notifyMinSec)
	: BWindow(BRect(100, 100, 900, 680), "Claude",
	           B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS)
	, fAuth(auth)
	, fModel(model)
	, fMaxTokens(maxTokens)
	, fSystemPrompt(systemPrompt)
	, fMessages(nlohmann::json::array())
	, fNotifyMinSec(notifyMinSec)
{
	// Load Genio theme and language set — passed to SciOutput for code highlighting.
	const std::string themePath = styling::FindDefaultTheme();
	const std::string langsDir  = styling::FindLanguagesDir();
	if (!themePath.empty()) fTheme.LoadFile(themePath);
	if (!langsDir.empty())  fLangSet.LoadDir(langsDir);

	_BuildLayout();

	// Configure SciOutput styles now that the view is attached and the
	// Scintilla engine is initialised (happens during _BuildLayout's AddView).
	if (fOutput) fOutput->Configure();

	// Markdown renderer — uses SciOutput backend.
	fMdRenderer = new md::MdRenderer(fOutput);

	// Slash-command popup.
	fCommandPopup = new CommandPopup(this);

	// Ensure BFS attribute indexes exist for fast session queries.
	session::EnsureIndexes();

	// Update title with model name.
	_UpdateTitle();

	// Register keyboard shortcuts.
	AddShortcut('N', B_COMMAND_KEY, new BMessage(gui::MSG_NEW_CHAT));
	AddShortcut('L', B_COMMAND_KEY, new BMessage(gui::MSG_CLEAR_OUTPUT));
	AddShortcut(',', B_COMMAND_KEY, new BMessage(gui::MSG_SETTINGS));
	AddShortcut('H', B_COMMAND_KEY, new BMessage(gui::MSG_SESSIONS));
}

ChatWindow::~ChatWindow()
{
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
	// ── SciOutput — chat scrollback ───────────────────────────────────────────
	// Direct child of the window (not inside a BScrollView).
	// BScintillaView provides built-in vertical scrolling.
	// Theme and language set are passed in for future syntax highlighting.
	fOutput = new SciOutput("output",
	    fTheme.IsLoaded()   ? &fTheme   : nullptr,
	    fLangSet.IsLoaded() ? &fLangSet : nullptr);

	// Floating jump-to-bottom button.
	fJumpBtn = new BButton("jumpbtn", "\xE2\x86\x93 New",
	                        new BMessage(gui::MSG_JUMP_BOTTOM));
	fJumpBtn->SetExplicitSize(BSize(80, 26));
	fJumpBtn->Hide();
	AddChild(fJumpBtn);

	// ── Token bar ────────────────────────────────────────────────────────────
	fTokenBar = new TokenBar();

	// ── Spinner ──────────────────────────────────────────────────────────────
	fSpinner = new SpinnerView();

	// ── Input area ───────────────────────────────────────────────────────────
	fInput = new InputView("input");
	fInputScroll = new BScrollView("inputscroll", fInput,
	                               0, false, true, B_FANCY_BORDER);
	fInputScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 54));
	fInputScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 180));

	// ── Buttons ──────────────────────────────────────────────────────────────
	fSend = new BButton("send", "Send", new BMessage(gui::MSG_SEND));
	fSend->MakeDefault(true);

	fStop = new BButton("stop", "Stop", new BMessage(gui::MSG_CANCEL));
	fStop->Hide();

	// ── Model picker ─────────────────────────────────────────────────────────
	fModelMenu  = new BPopUpMenu(fModel.c_str());
	fModelField = new BMenuField("modelpicker", "Model:", fModelMenu);
	_PopulateModelMenu();

	// ── Secondary toolbar buttons ─────────────────────────────────────────────
	fNewBtn      = new BButton("newbtn",      "New",      new BMessage(gui::MSG_NEW_CHAT));
	fClearBtn    = new BButton("clearbtn",    "Clear",    new BMessage(gui::MSG_CLEAR_OUTPUT));
	fSettingsBtn = new BButton("settingsbtn", "\xE2\x9A\x99",
	                            new BMessage(gui::MSG_SETTINGS));
	fSettingsBtn->SetToolTip("Settings (Cmd+,)");
	fSessionBtn  = new BButton("sessionbtn",  "History",  new BMessage(gui::MSG_SESSIONS));
	fSessionBtn->SetToolTip("Session history (Cmd+H)");

	// ── Session panel (left) + Settings panel (right) ─────────────────────────
	fSessionPanel = new SessionPanel(this);
	fSettings     = new SettingsPanel(fSystemPrompt, fMaxTokens, fNotifyMinSec);

	// ── Layout ────────────────────────────────────────────────────────────────
	// SciOutput replaces the BScrollView+BTextView combo — it's a single
	// view with built-in scrolling, added directly to the layout.
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fSessionPanel, 0.0f)
			.Add(fOutput, 1.0f)          // SciOutput directly in layout
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
			.Add(fSessionBtn)
			.Add(fSettingsBtn)
		.End()
	.End();

	SetSizeLimits(420, 32767, 300, 32767);
	fInput->MakeFocus(true);
}

void ChatWindow::_PopulateModelMenu()
{
	// Populate immediately with the hard-coded fallback so the menu is
	// usable right away, then kick off a background fetch to replace it.
	auto addItem = [&](const std::string& id, const std::string& label, bool mark) {
		BMessage* msg = new BMessage(gui::MSG_MODEL_PICK);
		msg->AddString("model", id.c_str());
		BMenuItem* item = new BMenuItem(label.c_str(), msg);
		if (mark) item->SetMarked(true);
		fModelMenu->AddItem(item);
	};

	bool found = false;
	for (int i = 0; kKnownModels[i] != nullptr; ++i) {
		bool mark = (fModel == kKnownModels[i]);
		if (mark) found = true;
		addItem(kKnownModels[i], kKnownModels[i], mark);
	}
	if (!found) {
		// Current model not in fallback list — add it at top, marked.
		BMessage* msg = new BMessage(gui::MSG_MODEL_PICK);
		msg->AddString("model", fModel.c_str());
		BMenuItem* item = new BMenuItem(fModel.c_str(), msg);
		item->SetMarked(true);
		fModelMenu->AddItem(item, 0);
	}
	fModelMenu->SetRadioMode(true);

	// Background fetch — capture messenger by value so it becomes invalid
	// cleanly if the window closes before the fetch completes.
	const config::Auth auth    = fAuth;
	const BMessenger   window  = BMessenger(this);
	std::thread([auth, window]() {
		std::vector<models::ModelEntry> fetched = models::FetchModels(auth);
		if (fetched.empty() || !window.IsValid()) return;
		BMessage ready(gui::MSG_MODELS_READY);
		for (const auto& e : fetched) {
			ready.AddString("id",   e.id.c_str());
			ready.AddString("name", e.display_name.c_str());
		}
		window.SendMessage(&ready);
	}).detach();
}

void ChatWindow::_RepositionOverlays()
{
	if (!fJumpBtn || !fOutput) return;
	const BRect ob = fOutput->Frame();
	const float bw = fJumpBtn->Frame().Width();
	const float bh = fJumpBtn->Frame().Height();
	fJumpBtn->MoveTo(ob.right - bw - 12.0f, ob.bottom - bh - 12.0f);
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
		if (fSettings->IsOpen()) {
			// Panel is open — closing it: read back the edited values.
			fSystemPrompt         = fSettings->SystemPrompt();
			fMaxTokens            = fSettings->MaxTokens();
			fNotificationsEnabled = fSettings->NotificationsEnabled();
			fNotifyMinSec         = fSettings->NotifyMinSeconds();
		}
		fSettings->Toggle();
		break;

	case gui::MSG_SESSIONS:
		fSessionPanel->Toggle();
		break;

	case gui::MSG_SESSION_LOAD: {
		if (!fSessionPanel) break;
		const int32_t idx = fSessionPanel->fList
		    ? fSessionPanel->fList->CurrentSelection() : -1;
		const session::SessionInfo* info = fSessionPanel->InfoAt(idx);
		if (info) _LoadSession(info->path);
		break;
	}

	case gui::MSG_POPUP_UPDATE: {
		if (!fCommandPopup || !fInputScroll) break;
		const char* prefix = nullptr;
		msg->FindString("prefix", &prefix);
		// Compute screen point: top-left of the input scroll view.
		BPoint pt = fInputScroll->Frame().LeftTop();
		ConvertToScreen(&pt);

		fCommandPopup->Show(prefix ? prefix : "/", pt);
		break;
	}

	// Arrow-key navigation and Escape/Enter are handled inside BPopUpMenu's
	// own event loop — these messages are no longer needed but kept for
	// safety in case old messages arrive.
	case gui::MSG_POPUP_NEXT:
	case gui::MSG_POPUP_PREV:
	case gui::MSG_POPUP_CONF:
	case gui::MSG_POPUP_HIDE:

		break;

	case gui::MSG_COMPLETE_CMD: {
		const char* cmd = nullptr;
		if (msg->FindString("cmd", &cmd) == B_OK && cmd) {
			fInput->SetText(cmd);
			const int32 len = fInput->TextLength();
			fInput->Select(len, len);
			fInput->MakeFocus(true);
		}

		break;
	}

	case B_SIMPLE_DATA:
	case B_REFS_RECEIVED:
		_RefsReceived(msg);
		break;

	case gui::MSG_MODELS_READY: {
		// Background fetch returned — rebuild the model menu with live data.
		if (!fModelMenu) break;
		fModelMenu->RemoveItems(0, fModelMenu->CountItems(), true);
		bool found = false;
		const char* id   = nullptr;
		const char* name = nullptr;
		for (int32 i = 0;
		     msg->FindString("id",   i, &id)   == B_OK &&
		     msg->FindString("name", i, &name) == B_OK; ++i) {
			const std::string sid(id);
			const std::string sname(name ? name : id);
			const std::string label = (sname.empty() || sname == sid)
			    ? sid : sname + "  (" + sid + ")";
			BMessage* m = new BMessage(gui::MSG_MODEL_PICK);
			m->AddString("model", sid.c_str());
			BMenuItem* item = new BMenuItem(label.c_str(), m);
			if (sid == fModel) { item->SetMarked(true); found = true; }
			fModelMenu->AddItem(item);
		}
		if (!found) {
			BMessage* m = new BMessage(gui::MSG_MODEL_PICK);
			m->AddString("model", fModel.c_str());
			BMenuItem* item = new BMenuItem(fModel.c_str(), m);
			item->SetMarked(true);
			fModelMenu->AddItem(item, 0);
		}
		fModelMenu->SetRadioMode(true);
		// Update the field label to reflect the live list.
		if (fModelField) fModelField->MenuItem()->SetLabel(fModel.c_str());
		break;
	}

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
		++fToolsUsed;
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
			fOutput->AppendText(
			    std::string("\n\xE2\x9A\xA0 ") + text + "\n",
			    SciOutput::kStyleError);
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

		// Auto-save session to BFS after every completed turn.
		_SaveSession();
		if (fSessionPanel && fSessionPanel->IsOpen())
			fSessionPanel->Refresh();

		// Desktop notification — always fire for longer turns (>5 s) so
		// the user knows the result is ready even while focused on the app
		// doing something else. Short turns are silent to avoid noise.
		{
			const bigtime_t elapsed = system_time() - fTurnStartTime;
			const int       elapsedSec = static_cast<int>(elapsed / 1000000LL);
			const bool      longTurn   = elapsedSec >= fNotifyMinSec;
			const bool      hadTools   = fToolsUsed > 0;

			if (fNotificationsEnabled && (longTurn || hadTools || !IsActive())) {
				BNotification notif(B_INFORMATION_NOTIFICATION);
				notif.SetGroup("Claude");
				notif.SetMessageID("claude-response"); // replaces previous

				// Title describes what happened.
				if (hadTools) {
					std::string t = "Done \xE2\x80\x94 ";  // em dash
					t += std::to_string(fToolsUsed);
					t += (fToolsUsed == 1) ? " tool run" : " tools run";
					t += " (" + std::to_string(elapsedSec) + "s)";
					notif.SetTitle(t.c_str());
				} else {
					std::string t = "Response ready";
					if (longTurn)
						t += " (" + std::to_string(elapsedSec) + "s)";
					notif.SetTitle(t.c_str());
				}

				// Content: first non-empty line of the assistant reply.
				std::string preview;
				if (!fPendingAssistantText.empty()) {
					// fPendingAssistantText was already cleared above;
					// use the last committed assistant message instead.
					for (auto it = fMessages.rbegin(); it != fMessages.rend(); ++it) {
						if ((*it).value("role", "") == "assistant") {
							preview = (*it).value("content", "");
							break;
						}
					}
				}
				// Strip leading whitespace / newlines.
				const size_t start = preview.find_first_not_of(" \t\n\r");
				if (start != std::string::npos) preview = preview.substr(start);
				// Truncate to ~120 chars for the notification body.
				if (preview.size() > 120)
					preview = preview.substr(0, 117) + "\xE2\x80\xA6"; // …
				if (!preview.empty())
					notif.SetContent(preview.c_str());

				notif.Send();
			}
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
	fOutput->AppendText(text, SciOutput::kStyleDefault);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_AppendToolLine(const std::string& text)
{
	fOutput->AppendText(text, SciOutput::kStyleToolLine);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_ScrollToBottom()
{
	fOutput->ScrollToEnd();
}

bool ChatWindow::_IsNearBottom() const
{
	return fOutput ? fOutput->IsNearBottom() : true;
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

	// Append the code block through SciOutput — styled monospace with
	// language tag. Syntax highlighting (Stage 2) will be added here.
	fOutput->AppendCodeBlock(fCodeBuffer, fCodeLang);

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
	fOutput->AppendText("\nyou \xE2\x96\xB8 ", SciOutput::kStyleUserLabel);
	_AppendText(userText + "\n");
	fOutput->AppendText("claude \xE2\x96\xB8 \n", SciOutput::kStyleModelLabel);

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

	// Record turn start time and reset tool counter for this turn.
	fTurnStartTime = system_time();
	fToolsUsed     = 0;

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
		const api::SendResult result = api::SendWithTools(auth, model, maxTokens,
		                   const_cast<nlohmann::json&>(messages),
		                   systemPrompt, sink);
		sink->EndMessage();

		// Post token counts so the TokenBar can update.
		BMessage tokMsg(gui::MSG_TOKENS);
		tokMsg.AddInt32("input",  result.input_tokens);
		tokMsg.AddInt32("output", result.output_tokens);
		tokMsg.AddInt32("max",    maxTokens);
		BMessenger(this).SendMessage(&tokMsg);
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
	fSessionPath.clear();
	fSessionInputTokens  = 0;
	fSessionOutputTokens = 0;
	if (fTokenBar) fTokenBar->SetTokens(0, fMaxTokens);
	_UpdateTitle();

	// Always restore the input to a ready state — _CancelWorker() only
	// requests interruption and leaves _SetBusy(false) to MSG_WORKER_DONE,
	// but if no worker was running the input would stay disabled/hidden.
	_SetBusy(false);
}

void ChatWindow::_ClearOutput()
{
	fOutput->Clear();
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

void ChatWindow::_SaveSession()
{
	if (fMessages.empty()) return;
	const std::string saved = session::Save(
	    fSessionPath,
	    fConvTopic.empty() ? "Untitled" : fConvTopic,
	    fModel,
	    fTurnCount,
	    fMessages);
	if (!saved.empty())
		fSessionPath = saved;
}

void ChatWindow::_LoadSession(const std::string& path)
{
	if (path.empty()) return;
	nlohmann::json loaded = session::Load(path);
	if (loaded.empty()) return;

	// Cancel any running worker first.
	_CancelWorker();
	_ClearOutput();

	fMessages    = loaded;
	fSessionPath = path;
	fTurnCount   = 0;
	fConvTopic.clear();

	// Replay the conversation into the output view so the user can see it.
	for (const auto& turn : fMessages) {
		const std::string role    = turn.value("role", "");
		const std::string content = turn.value("content", "");
		if (content.empty()) continue;

		if (role == "user") {
			if (fConvTopic.empty()) {
				fConvTopic = content.size() > 60
				    ? content.substr(0, 57) + "\xE2\x80\xA6"
				    : content;
			}
			fOutput->AppendText("\nyou \xE2\x96\xB8 ", SciOutput::kStyleUserLabel);
			_AppendText(content + "\n");
			++fTurnCount;
		} else if (role == "assistant") {
			fOutput->AppendText("claude \xE2\x96\xB8 \n", SciOutput::kStyleModelLabel);
			if (fMdRenderer) {
				fMdRenderer->Write(content);
				fMdRenderer->Flush();
			} else {
				_AppendText(content + "\n");
			}
		}
	}

	_UpdateTitle();
	_ScrollToBottom();
	fInput->MakeFocus(true);
}

void ChatWindow::_RefsReceived(BMessage* msg)
{
	// Handle files dropped onto the window or passed via B_REFS_RECEIVED.
	// Each file's text content is inserted into the input as a fenced
	// code block so the user can ask Claude about it.
	entry_ref ref;
	for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i) {
		BEntry entry(&ref);
		BPath  path;
		if (entry.GetPath(&path) == B_OK)
			_InsertFileContent(path.Path());
	}
}

void ChatWindow::_InsertFileContent(const std::string& path)
{
	// Read the file — cap at 64 KB to avoid flooding the context.
	constexpr size_t kMaxBytes = 64 * 1024;
	BFile file(path.c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) return;

	off_t size = 0;
	file.GetSize(&size);
	if (size <= 0) return;

	const size_t readLen = static_cast<size_t>(
	    std::min(static_cast<off_t>(kMaxBytes), size));
	std::string content(readLen, '\0');
	if (file.Read(&content[0], readLen) <= 0) return;

	// Detect if the content looks like binary — if more than 10% of
	// the first 512 bytes are non-printable, skip it.
	const size_t sample = std::min(readLen, size_t(512));
	size_t nonPrint = 0;
	for (size_t i = 0; i < sample; ++i) {
		const unsigned char c = static_cast<unsigned char>(content[i]);
		if (c < 32 && c != '\t' && c != '\n' && c != '\r') ++nonPrint;
	}
	if (nonPrint > sample / 10) {
		BAlert* alert = new BAlert("Binary file",
		    "That file appears to be binary and cannot be attached as text.",
		    "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->Go();
		return;
	}

	// Truncation notice.
	bool truncated = (static_cast<off_t>(readLen) < size);

	// Derive a language hint from the file extension for the fence.
	const std::string spath(path);
	std::string lang;
	const auto dot = spath.rfind('.');
	if (dot != std::string::npos) lang = spath.substr(dot + 1);
	// Map common extensions.
	if (lang == "h" || lang == "cc" || lang == "cxx") lang = "cpp";
	if (lang == "py") lang = "python";
	if (lang == "js") lang = "javascript";
	if (lang == "md") lang = "markdown";
	if (lang == "sh") lang = "bash";

	// Extract just the filename for the header.
	std::string filename = spath;
	const auto slash = spath.rfind('/');
	if (slash != std::string::npos) filename = spath.substr(slash + 1);

	// Build the fenced block to insert.
	std::string block = "\n`" + filename + "`\n";
	block += "```" + lang + "\n";
	block += content;
	if (!content.empty() && content.back() != '\n') block += '\n';
	if (truncated)
		block += "... [truncated at 64 KB]\n";
	block += "```\n";

	// Append to whatever is already in the input.
	const int32 end = fInput->TextLength();
	fInput->Insert(end, block.c_str(), static_cast<int32>(block.size()));
	fInput->Select(fInput->TextLength(), fInput->TextLength());
	fInput->MakeFocus(true);
}
