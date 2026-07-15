// chat_window.cpp — Claude GUI main window implementation.
//
// Three native widget classes are defined here before ChatWindow:
//   InputView    — multi-line input with history, placeholder, Shift+Enter.
//   TokenBar     — thin coloured fill bar showing context usage.
//   SettingsDialog — dialog window for system prompt / model config.
//   SpinnerView  — animated thinking indicator.
//
// All view mutations happen on the main thread inside MessageReceived or
// layout callbacks. The worker thread communicates exclusively via BMessages.

#include "chat_window.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <Alert.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <File.h>
#include <Font.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <Box.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <Menu.h>
#include <MenuBar.h>
#include <Message.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <OS.h>
#include <Roster.h>
#include <ScrollBar.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <SpaceLayoutItem.h>
#include <TextView.h>
#include <Window.h>

#include <FilePanel.h>
#include <ListItem.h>
#include <MenuItem.h>
#include <NodeInfo.h>
#include <Path.h>
#include <SplitView.h>
#include <PopUpMenu.h>

#include <private/interface/AboutWindow.h>
#include "api.h"
#include "code_styler.h"
#include "syntax_highlight.h"
#include "config.h"
#include "gui_sink.h"
#include "tee_sink.h"
#include "md_renderer.h"
#include "models.h"
#include "paths.h"
#include "session_store.h"
#include "telegram.h"

// ---------------------------------------------------------------------------
// Colour helpers — prefer ui_color() for theme-aware values.
// Hard-coded values are used only for chat-content colours that intentionally
// stay dark regardless of the system theme (the output area is always dark).
// ---------------------------------------------------------------------------
namespace {

// ---------------------------------------------------------------------------
// HiDPI scaling. Haiku does not expose a monitor DPI directly; instead the
// system font size grows on high-resolution displays (12pt is the 1x
// baseline). Deriving a scale factor from be_plain_font lets every hard-coded
// pixel dimension track the display so dialogs, buttons, bars and icons stay
// proportional on HiDPI screens. Mirrors the be_control_look convention.
// ---------------------------------------------------------------------------
float
gui_scale()
{
	const float base = be_plain_font ? be_plain_font->Size() : 12.0f;
	const float s    = base / 12.0f;
	// Clamp so an oddly configured font never produces an unusable window.
	if (s < 1.0f) return 1.0f;
	if (s > 4.0f) return 4.0f;
	return s;
}

// Scale a 1x pixel measurement to the current display, rounded up so we
// never lose a pixel that would clip glyphs. Named ScalePx (not Scale)
// because BView::Scale() already exists and would shadow a free Scale().
float
ScalePx(float px)
{
	return std::ceil(px * gui_scale());
}

// Standard RFC 4648 base64 encoder (not URL-safe — the Anthropic image
// API wants '+' / '/' with '=' padding). Used to embed dropped image
// files as base64 `image` content blocks in the outgoing message.
std::string Base64Encode(const std::string& in)
{
	static const char* kTable =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		const unsigned n = (static_cast<unsigned char>(in[i]) << 16)
		                 | (static_cast<unsigned char>(in[i + 1]) << 8)
		                 |  static_cast<unsigned char>(in[i + 2]);
		out += kTable[(n >> 18) & 63];
		out += kTable[(n >> 12) & 63];
		out += kTable[(n >> 6) & 63];
		out += kTable[n & 63];
	}
	if (i < in.size()) {
		unsigned n = static_cast<unsigned char>(in[i]) << 16;
		if (i + 1 < in.size())
			n |= static_cast<unsigned char>(in[i + 1]) << 8;
		out += kTable[(n >> 18) & 63];
		out += kTable[(n >> 12) & 63];
		out += (i + 1 < in.size()) ? kTable[(n >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

// Map a file extension to an Anthropic-supported image media type, or
// return an empty string if the extension is not a supported image.
std::string ImageMediaType(const std::string& path)
{
	const auto dot = path.rfind('.');
	if (dot == std::string::npos) return {};
	std::string ext = path.substr(dot + 1);
	for (char& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
	if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
	if (ext == "png")                  return "image/png";
	if (ext == "gif")                  return "image/gif";
	if (ext == "webp")                 return "image/webp";
	return {};
}

// ChoiceModal — a small modal window presenting one button per option,
// used by AskChoice when there are more than three options (BAlert caps
// at three buttons). Runs its own nested event loop via a semaphore so
// the calling code blocks until the user picks or closes the window.
//
// Returns the 0-based index of the chosen option, or -1 if the window is
// closed without a selection.
class ChoiceModal : public BWindow {
public:
	ChoiceModal(const std::string& prompt,
	            const std::vector<std::string>& options)
		: BWindow(BRect(0, 0, 360, 100), "Choose",
		          B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
		          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
		  fDoneSem(create_sem(0, "choice_modal"))
	{
		BStringView* label = new BStringView("prompt", prompt.c_str());
		label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

		BLayoutBuilder::Group<> builder(this, B_VERTICAL, B_USE_SMALL_SPACING);
		builder.SetInsets(B_USE_WINDOW_SPACING);
		builder.Add(label);
		BButton* first = nullptr;
		for (size_t i = 0; i < options.size(); ++i) {
			BMessage* m = new BMessage('CHpk');
			m->AddInt32("index", static_cast<int32>(i));
			BButton* b = new BButton("opt", options[i].c_str(), m);
			b->SetTarget(this);
			if (i == 0) first = b;
			builder.Add(b);
		}

		// Keyboard a11y: Enter activates the first option, Esc cancels.
		if (first) {
			first->MakeDefault(true);
			first->MakeFocus(true);
		}
		AddShortcut(B_ESCAPE, 0, new BMessage('CHcl'));
		CenterOnScreen();
	}

	~ChoiceModal() override { delete_sem(fDoneSem); }

	void MessageReceived(BMessage* msg) override
	{
		if (msg->what == 'CHpk') {
			int32 idx = -1;
			msg->FindInt32("index", &idx);
			fResult = idx;
			_Finish();
			return;
		}
		if (msg->what == 'CHcl') {   // Esc — cancel
			fResult = -1;
			_Finish();
			return;
		}
		BWindow::MessageReceived(msg);
	}

	bool QuitRequested() override
	{
		// Closing the window without a pick counts as cancel.
		_Finish();
		return true;
	}

	// Show the window and block until a choice is made or it is closed.
	int Go()
	{
		Show();
		// Block the caller (worker-thread context via the window's
		// MessageReceived dispatch happens on this window's thread, so
		// we wait on the semaphore here and let the looper run).
		acquire_sem(fDoneSem);
		const int r = fResult;
		if (Lock()) Quit();   // tears down the window + looper
		return r;
	}

private:
	// Release the wait semaphore exactly once, however the modal ends
	// (button, Esc, or window close).
	void _Finish()
	{
		if (!fFinished) {
			fFinished = true;
			release_sem(fDoneSem);
		}
	}

	sem_id fDoneSem;
	int    fResult   = -1;
	bool   fFinished = false;
};

// SessionItem — a BStringItem that remembers the .session file path so
// the sidebar can load or delete the file behind a selected row.
class SessionItem : public BStringItem {
public:
	SessionItem(const std::string& label, const std::string& path,
	            const std::string& title)
		: BStringItem(label.c_str()), fPath(path), fTitle(title) {}
	const std::string& Path()  const { return fPath; }
	const std::string& Title() const { return fTitle; }
private:
	std::string fPath;
	std::string fTitle;   // raw title (no "  (N)" turn-count suffix)
};

// RenameModal — a tiny modal prompt with a single text field, used to
// rename a saved session. Blocks on a semaphore (same pattern as
// ChoiceModal) and returns the entered text, or empty on cancel.
class RenameModal : public BWindow {
public:
	RenameModal(const std::string& current)
		: BWindow(BRect(0, 0, 320, 90), "Rename Session",
		          B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
		          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
		  fDoneSem(create_sem(0, "rename_modal"))
	{
		fField = new BTextControl("name", nullptr, current.c_str(),
		                          new BMessage('RNok'));
		fField->SetTarget(this);
		BButton* ok     = new BButton("ok", "Rename", new BMessage('RNok'));
		BButton* cancel = new BButton("cancel", "Cancel", new BMessage('RNcl'));
		ok->MakeDefault(true);

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fField)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.AddGlue()
				.Add(cancel)
				.Add(ok)
			.End()
		.End();

		AddShortcut(B_ESCAPE, 0, new BMessage('RNcl'));
		CenterOnScreen();
	}

	~RenameModal() override { delete_sem(fDoneSem); }

	void Show() override
	{
		BWindow::Show();
		if (Lock()) {
			fField->MakeFocus(true);
			if (BTextView* tv = fField->TextView()) tv->SelectAll();
			Unlock();
		}
	}

	void MessageReceived(BMessage* msg) override
	{
		if (msg->what == 'RNok') {
			if (const char* t = fField->Text()) fResult = t;
			_Finish();
			return;
		}
		if (msg->what == 'RNcl') { fResult.clear(); _Finish(); return; }
		BWindow::MessageReceived(msg);
	}

	bool QuitRequested() override { _Finish(); return true; }

	// Show modally and return the entered name (empty = cancelled).
	std::string Go()
	{
		Show();
		acquire_sem(fDoneSem);
		const std::string r = fResult;
		if (Lock()) Quit();
		return r;
	}

private:
	void _Finish()
	{
		if (!fFinished) { fFinished = true; release_sem(fDoneSem); }
	}

	BTextControl* fField    = nullptr;
	sem_id        fDoneSem;
	std::string   fResult;
	bool          fFinished = false;
};

// SessionListView — BListView that pops a right-click context menu
// (Rename / Open / Delete) on the row under the cursor. The menu items
// post the existing sidebar messages to the window, so the handlers are
// shared with the New/Open/Delete buttons.
class SessionListView : public BListView {
public:
	SessionListView(const char* name, BHandler* target)
		: BListView(name, B_MULTIPLE_SELECTION_LIST), fTarget(target) {}

	void MouseDown(BPoint where) override
	{
		uint32 buttons = 0;
		if (Window()) {
			BMessage* msg = Window()->CurrentMessage();
			if (msg) msg->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
		}

		if (buttons & B_SECONDARY_MOUSE_BUTTON) {
			const int32 idx = IndexOf(where);
			if (idx < 0) { BListView::MouseDown(where); return; }
			// Right-clicking a row that isn't part of the current
			// selection makes it the sole selection (Tracker-like).
			if (!IsItemSelected(idx)) Select(idx);

			BPopUpMenu* menu = new BPopUpMenu("ctx", false, false);
			menu->AddItem(new BMenuItem("Rename\xE2\x80\xA6",
				new BMessage(gui::MSG_SESSION_RENAME)));
			menu->AddItem(new BMenuItem("Open",
				new BMessage(gui::MSG_SESSION_SELECT)));
			menu->AddSeparatorItem();
			menu->AddItem(new BMenuItem("Delete",
				new BMessage(gui::MSG_SESSION_DELETE)));
			menu->SetTargetForItems(fTarget);

			BPoint screenPt = where;
			ConvertToScreen(&screenPt);
			menu->Go(screenPt, true, true, true);
			return;
		}
		BListView::MouseDown(where);
	}

private:
	BHandler* fTarget = nullptr;
};

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
const rgb_color kColorDiffAdd     = {  80, 200,  80, 255 }; // green  — added lines
const rgb_color kColorDiffRemove  = { 220,  80,  80, 255 }; // red    — removed lines
const rgb_color kColorDiffHeader  = { 140, 180, 220, 255 }; // steel-blue — diff header/meta
// Cyan used for the input text, echoing the CLI's cyan "you>" prompt.
// Bright cyan that pops on the dark input background (kColorChatBg).
const rgb_color kColorInputCyan   = {  60, 200, 215, 255 };

// Known Anthropic models listed in the model picker.
// Fallback model list shown in the picker until the live /v1/models fetch
// replaces it. Kept roughly current with the API lineup; the background
// fetch in _PopulateModelMenu() is the source of truth at runtime.
const char* kKnownModels[] = {
	"claude-opus-4-8",
	"claude-opus-4-7",
	"claude-opus-4-6",
	"claude-opus-4-5",
	"claude-sonnet-5",
	"claude-sonnet-4-6",
	"claude-sonnet-4-5",
	"claude-haiku-4-5",
	"claude-opus-4-1",
	"claude-3-7-sonnet-20250219",
	"claude-3-5-sonnet-20241022",
	"claude-3-5-haiku-20241022",
	nullptr
};

void AppendWithColor(BTextView* view, const std::string& text, rgb_color color,
                     float zoom = 1.0f)
{
	if (text.empty()) return;
	BFont font;
	view->GetFont(&font);
	// Render at the current zoom so appended (non-markdown) text matches
	// the user's chosen size immediately.
	if (zoom > 0.0f && zoom != 1.0f)
		font.SetSize(font.Size() * zoom);
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

} // namespace


// ===========================================================================
// InputView
// ===========================================================================

InputView::InputView(const char* name)
	: BTextView(BRect(0, 0, 200, 24), name,
	            BRect(4, 4, 196, 20),
	            B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS)
{
	SetWordWrap(true);
	SetStylable(false);
}

void InputView::AttachedToWindow()
{
	BTextView::AttachedToWindow();
	// Match the dark chat area so the cyan input text pops like the
	// CLI's bright-cyan-on-dark prompt.
	SetViewColor(kColorChatBg);
	SetLowColor(kColorChatBg);
	SetHighColor(kColorInputCyan);
	// A slightly larger font than the default for comfortable typing.
	BFont f(be_plain_font);
	f.SetSize(f.Size() + 1.0f);
	SetFontAndColor(&f, B_FONT_ALL, &kColorInputCyan);
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
	// Dim gray that reads on the dark input background (matches the
	// chat's muted tool-line colour).
	SetHighColor(kColorToolLine);
	BPoint pen(TextRect().left, TextRect().top + f.Size());
	DrawString(placeholder, pen);
	// Restore the cyan typing colour so it isn't left as the dim gray.
	SetHighColor(kColorInputCyan);
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
			// Shift+Enter → insert a newline; plain Enter → send.
			BMessage* cur = Window() ? Window()->CurrentMessage() : nullptr;
			int32 modifiers = 0;
			if (cur) cur->FindInt32("modifiers", &modifiers);
			if (modifiers & B_SHIFT_KEY) {
				Insert("\n");
			} else {
				if (Window()) Window()->PostMessage(gui::MSG_SEND);
			}
			return;
		}
		if (bytes[0] == B_ESCAPE) {
			if (Window()) Window()->PostMessage(gui::MSG_CANCEL);
			return;
		}
		// Up/Down → prompt history, but only at the text boundaries so a
		// multi-line draft can still be navigated with the arrows. On Haiku
		// the arrow keys arrive as single bytes (B_UP_ARROW / B_DOWN_ARROW),
		// not a VT escape sequence.
		if (bytes[0] == B_UP_ARROW || bytes[0] == B_DOWN_ARROW) {
			int32 selStart = 0, selEnd = 0;
			GetSelection(&selStart, &selEnd);
			const int32 curLine  = LineAt(selStart);
			const int32 lastLine = CountLines() - 1;
			if (bytes[0] == B_UP_ARROW && curLine == 0) {
				_HistoryUp();
				return;
			}
			if (bytes[0] == B_DOWN_ARROW && curLine == lastLine) {
				_HistoryDown();
				return;
			}
			// Otherwise fall through to normal cursor movement.
		}
	}

	BTextView::KeyDown(bytes, numBytes);

	Invalidate();
}

void InputView::FrameResized(float w, float h)
{
	BTextView::FrameResized(w, h);
	SetTextRect(Bounds().InsetByCopy(4.0f, 4.0f));
}

// ===========================================================================
// InputContainer — Genio TerminalTab-style host (see TerminalTab.cpp). A
// plain B_FOLLOW_ALL BView with no size overrides claims layout space (its
// default unlimited max lets the layout stretch it); the real content is an
// AddChild'd scroll view that the container resizes to fill itself on every
// frame change.
// ===========================================================================

InputContainer::InputContainer(const char* name)
	: BView(name, B_FRAME_EVENTS | B_WILL_DRAW)
{
	SetResizingMode(B_FOLLOW_ALL);
	// Use the dark chat background (not panel gray) so any sub-pixel gap
	// between the scroll view and the container edges blends with the input
	// instead of showing a gray strip.
	SetViewColor(kColorChatBg);
	SetLowColor(kColorChatBg);
}

void InputContainer::SetContent(BView* content, float minHeight)
{
	fContent   = content;
	fMinHeight = minHeight;
	if (fContent == nullptr)
		return;
	// The content is positioned/sized manually (not by a layout), exactly
	// like TerminalTab's terminal view.
	fContent->SetResizingMode(B_FOLLOW_NONE);
	fContent->SetExplicitMinSize(BSize(48.0f, minHeight));
	fContent->SetExplicitPreferredSize(BSize(48.0f, minHeight));
	fContent->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	AddChild(fContent);
	// Pin the container's own height so the bottom bar is exactly the input
	// row height. Min and preferred sit at minHeight; max is unlimited in
	// height so the host absorbs any leftover row height (from the button
	// column's spacing/insets) and the dark input reaches the window's
	// bottom edge with no gray dead strip.
	SetExplicitMinSize(BSize(48.0f, minHeight));
	SetExplicitPreferredSize(BSize(B_SIZE_UNLIMITED, minHeight));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}

void InputContainer::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (fContent != nullptr) {
		BRect b = Bounds();
		fContent->MoveTo(0, 0);
		fContent->ResizeTo(b.Width(), b.Height());
	}
}

void InputContainer::FrameResized(float w, float h)
{
	if (fContent != nullptr)
		fContent->ResizeTo(w, h);
	BView::FrameResized(w, h);
	// A divider drag resizes this container but does NOT fire the window's
	// FrameResized, so the sibling button column can be left un-repainted and
	// appear to vanish. Invalidate the parent (the input bar) so every sibling
	// — including the Send/Stop/Clear buttons — is redrawn on each drag tick.
	if (BView* parent = Parent())
		parent->Invalidate();
}

void InputView::SetEnabled(bool enabled)
{
	fEnabled = enabled;
	MakeEditable(enabled);
	// Stay on the dark chat-matching background; dim it slightly while
	// disabled (during a turn) so the state is still legible. Do NOT
	// fall back to the light B_DOCUMENT_BACKGROUND_COLOR — that would
	// undo the dark unified look after the first turn.
	const rgb_color bg = enabled
		? kColorChatBg
		: tint_color(kColorChatBg, B_LIGHTEN_1_TINT);
	SetViewColor(bg);
	SetLowColor(bg);
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

void InputView::ClearHistory()
{
	fHistory.clear();
	fHistIdx = -1;
	fDraft.clear();
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
	// Height = one line of plain-font text plus a small fixed padding. The
	// bar only draws a single row of text (usage label + stats), so it does
	// not need the full control-look item spacing that made it look like it
	// had an unused extra row.
	font_height fh;
	be_plain_font->GetHeight(&fh);
	float height = std::ceil(fh.ascent + fh.descent + fh.leading) + ScalePx(6.0f);
	// Pin a small explicit min WIDTH (not B_SIZE_UNSET): a custom BView with an
	// unset min width reports its current frame width as the minimum, which at
	// the initial window size latches the whole chat column — and thus the
	// window — to a huge minimum width, letting the bottom input bar overflow
	// and clip its buttons. A small min lets the bar and window shrink freely.
	SetExplicitMinSize(BSize(ScalePx(40), height));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}

void TokenBar::Draw(BRect updateRect)
{
	BRect r = Bounds();
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

	// Top border drawn the Genio/GlobalStatusView way: a control-look border
	// on the top edge instead of a hand-stroked divider line, so it matches
	// the system look and the toolbar/menu separators.
	be_control_look->DrawBorder(this, r, updateRect,
		ui_color(B_PANEL_BACKGROUND_COLOR),
		B_PLAIN_BORDER, 0, BControlLook::B_TOP_BORDER);

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

	// Track the system font but render the label two points smaller: the
	// token bar is a compact status strip, so a slightly smaller size than
	// be_plain_font reads better while still scaling with the system font
	// on HiDPI displays.
	BFont f(be_plain_font);
	f.SetSize(f.Size() - 2.0f);
	SetFont(&f);
	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));

	// Vertically centre the text in the bar: place the baseline so that the
	// ascent/descent block is centred within the view's height.
	font_height fh;
	f.GetHeight(&fh);
	const float textH    = fh.ascent + fh.descent;
	const float baseline = std::floor((r.Height() - textH) / 2.0f + fh.ascent);

	const float lblW    = f.StringWidth(lbl.c_str());
	const float lblLeft = std::floor(r.right - lblW - ScalePx(8.0f));
	// The context-window label is the priority indicator, so draw it first
	// and reserve its space; the left-aligned stats yield to it.
	MovePenTo(lblLeft, baseline);
	DrawString(lbl.c_str());

	// Left-aligned per-session stats, mirroring the CLI status row:
	// "turn N · ↑ 1.2k · ↓ 420 · $0.0123".
	std::string stats =
		"turn " + std::to_string(fTurn)
		+ "  \xC2\xB7  \xE2\x86\x91 " + models::CompactTokens(fInput)
		+ "  \xC2\xB7  \xE2\x86\x93 " + models::CompactTokens(fOutput);
	if (fPriceIn > 0.0 || fPriceOut > 0.0) {
		const double cost = (fInput  / 1'000'000.0) * fPriceIn
		                  + (fOutput / 1'000'000.0) * fPriceOut;
		char costBuf[32];
		std::snprintf(costBuf, sizeof(costBuf), "  \xC2\xB7  $%.4f", cost);
		stats += costBuf;
	}

	// When ludicrous mode is on, draw a yellow "⚡ LUDICROUS" badge at the
	// far left so the auto-approve state is always visible, mirroring the
	// CLI status bar. The session stats start after the badge(s).
	float statsLeft = r.left + ScalePx(4.0f);
	const std::string sep = "  \xC2\xB7  ";
	if (fLudicrous) {
		const std::string badge = "\xE2\x9A\xA1 LUDICROUS";
		SetHighColor(230, 170, 50, 255); // amber, matches the bar's warning tone
		MovePenTo(statsLeft, baseline);
		DrawString(badge.c_str());
		statsLeft += f.StringWidth(badge.c_str()) + f.StringWidth(sep.c_str());
		SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	}
	if (fRemote) {
		const std::string badge = "\xF0\x9F\x93\xA1 REMOTE"; // 📡
		SetHighColor(80, 200, 80, 255); // green, matching the CLI's "Remote Control active"
		MovePenTo(statsLeft, baseline);
		DrawString(badge.c_str());
		statsLeft += f.StringWidth(badge.c_str()) + f.StringWidth(sep.c_str());
		SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	}

	// Only draw the stats if they fit to the left of the context-window label
	// without overlapping it (leave an 8px gutter between the two). When the
	// window is narrow the stats are dropped rather than overdrawing the
	// always-important "x / 200k (n%)" indicator.
	const float statsWidth = f.StringWidth(stats.c_str());
	if (statsLeft + statsWidth + ScalePx(8.0f) <= lblLeft) {
		MovePenTo(statsLeft, baseline);
		DrawString(stats.c_str());
	}
}

void TokenBar::SetPrice(double inputPerM, double outputPerM)
{
	fPriceIn  = inputPerM;
	fPriceOut = outputPerM;
	Invalidate();
}

void TokenBar::SetStats(int turn, int sessionInput, int sessionOutput)
{
	fTurn   = turn;
	fInput  = sessionInput;
	fOutput = sessionOutput;
	Invalidate();
}

void TokenBar::SetTokens(int used, int maxCtx)
{
	fUsed = used;
	if (maxCtx > 0) fMax = maxCtx;
	Invalidate();
}

void TokenBar::SetLudicrous(bool on)
{
	if (fLudicrous == on) return;
	fLudicrous = on;
	Invalidate();
}

void TokenBar::SetRemote(bool on)
{
	if (fRemote == on) return;
	fRemote = on;
	Invalidate();
}


// ===========================================================================
// SettingsDialog
// ===========================================================================

SettingsDialog::SettingsDialog(BWindow* parent, const std::string& systemPrompt,
                               int maxTokens, int notifyMinSec,
                               const std::string& workingDir,
                               BMenuField* modelField)
	: BWindow(BRect(0, 0, ScalePx(640), ScalePx(480)), "Settings",
	          B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
	          B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	  fParent(parent)
{
	_BuildLayout(systemPrompt, maxTokens, notifyMinSec, workingDir, modelField);
	// Give the window a sensible default size: wide enough to read a full
	// working-directory path, then center it. AUTO_UPDATE_SIZE_LIMITS only
	// sets the minimum, so resize explicitly to the preferred width.
	// Scaled so the dialog grows with the system font on HiDPI displays.
	ResizeTo(ScalePx(640), ScalePx(480));
	CenterOnScreen();
	// Start the looper running but keep the window off-screen: Hide() before
	// Show() leaves a net-hidden window whose looper is alive, so Toggle()
	// can reveal it instantly and the parent can lock it to read values.
	Hide();
	Show();
}

void SettingsDialog::_BuildLayout(const std::string& systemPrompt, int maxTokens,
                                  int notifyMinSec, const std::string& workingDir,
                                  BMenuField* modelField)
{
	// System-prompt label + editor.
	BStringView* sysLabel = new BStringView("syslbl", "System Prompt:");
	fSysPromptView        = new BTextView("sysprompt", B_WILL_DRAW | B_FRAME_EVENTS);
	fSysPromptView->SetWordWrap(true);
	fSysPromptView->SetText(systemPrompt.c_str());
	fSysPromptView->SetExplicitMinSize(BSize(ScalePx(80), ScalePx(80)));
	BScrollView* sysScroll = new BScrollView("sysscroll", fSysPromptView,
	                                          0, false, true, B_FANCY_BORDER);
	sysScroll->SetExplicitMinSize(BSize(ScalePx(100), ScalePx(80)));
	sysScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

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

	// Working directory field + Browse button.
	// The text control holds the path; the Browse button opens a
	// BFilePanel so the user can pick a directory without typing.
	fWorkingDirCtl = new BTextControl("workingdir", "Working directory:",
	                                   workingDir.c_str(), nullptr);
	fWorkingDirCtl->SetToolTip("Directory Claude uses as the root for "
	                            "relative file paths and tool calls.");
	// Make the path field roomy enough to read a full path without scrolling.
	fWorkingDirCtl->SetExplicitMinSize(BSize(ScalePx(360), B_SIZE_UNSET));
	BButton* browseBtn = new BButton("browseworkdir", "Browse" B_UTF8_ELLIPSIS,
	                                  new BMessage(gui::MSG_BROWSE_WORKDIR));
	BButton* closeBtn = new BButton("closesettings", "Close",
	                                 new BMessage(gui::MSG_SETTINGS));
	closeBtn->MakeDefault(true);

	// Browse and Close are handled by the parent ChatWindow (the working-dir
	// BFilePanel and the value read-back logic both live there).
	if (fParent) {
		browseBtn->SetTarget(fParent);
		closeBtn->SetTarget(fParent);
	}

	// Group the controls into labelled BBoxes, mirroring Genio's ConfigWindow
	// idiom (MakeViewFor wraps each settings group in a BBox with a label).
	BBox* modelBox = new BBox("modelbox");
	modelBox->SetLabel("Model");
	BGroupView* modelGroup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	modelGroup->GroupLayout()->SetInsets(B_USE_ITEM_INSETS);
	BLayoutBuilder::Group<>(modelGroup)
		.Add(modelField)
		.Add(fMaxTokensCtl)
		.Add(sysLabel)
		.Add(sysScroll, 1.0f)
	.End();
	modelBox->AddChild(modelGroup);

	BBox* behaviorBox = new BBox("behaviorbox");
	behaviorBox->SetLabel("Behavior");
	BGroupView* behaviorGroup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	behaviorGroup->GroupLayout()->SetInsets(B_USE_ITEM_INSETS);
	BLayoutBuilder::Group<>(behaviorGroup)
		.Add(notifyLabel)
		.Add(fNotifyDelay)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fWorkingDirCtl, 1.0f)
			.Add(browseBtn, 0.0f)
		.End()
	.End();
	behaviorBox->AddChild(behaviorGroup);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(modelBox, 1.0f)
		.Add(behaviorBox, 0.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.AddGlue()
			.Add(closeBtn)
		.End()
	.End();
}

void SettingsDialog::SetValues(const std::string& systemPrompt, int maxTokens,
                               int notifyMinSec, const std::string& workingDir)
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
	if (fWorkingDirCtl)
		fWorkingDirCtl->SetText(workingDir.c_str());
}

std::string SettingsDialog::SystemPrompt() const
{
	if (!fSysPromptView) return {};
	const char* t = fSysPromptView->Text();
	return t ? std::string(t) : std::string();
}

int SettingsDialog::MaxTokens() const
{
	if (!fMaxTokensCtl) return 8192;
	const char* t = fMaxTokensCtl->Text();
	if (!t || t[0] == '\0') return 8192;
	int v = std::atoi(t);
	return (v > 0) ? v : 8192;
}

bool SettingsDialog::NotificationsEnabled() const
{
	// A delay of 0 means notifications are turned off entirely.
	if (!fNotifyDelay) return true;
	return fNotifyDelay->Value() > 0;
}

int SettingsDialog::NotifyMinSeconds() const
{
	if (!fNotifyDelay) return 5;
	return static_cast<int>(fNotifyDelay->Value());
}

std::string SettingsDialog::WorkingDir() const
{
	if (!fWorkingDirCtl) return {};
	const char* t = fWorkingDirCtl->Text();
	return t ? std::string(t) : std::string();
}

void SettingsDialog::Toggle()
{
	fOpen = !fOpen;
	if (fOpen) {
		if (IsHidden())
			Show();
		Activate(true);
	} else {
		if (!IsHidden())
			Hide();
	}
}

bool SettingsDialog::QuitRequested()
{
	// The window is created once and kept alive (hidden) for the life of
	// the app, so a close request from the title-bar X must not actually
	// quit it. Defer to the parent's MSG_SETTINGS handler (which reads the
	// edited values back, then calls Toggle() to hide us).
	if (fOpen && fParent)
		fParent->PostMessage(gui::MSG_SETTINGS);
	return false;
}


// ===========================================================================
// SpinnerView
// ===========================================================================

// CLI-style spinner vocabulary (mirrors tui.cpp): braille rotation glyphs
// and randomized gerund verbs. Kept local to the GUI so it doesn't depend
// on the terminal Spinner class.
namespace {
const char* kGuiSpinnerGlyphs[] = {
	"\xE2\xA3\xBE", "\xE2\xA3\xBD", "\xE2\xA3\xBB", "\xE2\xA2\xBF",
	"\xE2\xA1\xBF", "\xE2\xA0\xBF", "\xE2\xA2\xAF", "\xE2\xA3\xB7",
};
constexpr int kGuiGlyphCount = sizeof(kGuiSpinnerGlyphs) / sizeof(kGuiSpinnerGlyphs[0]);

const char* kGuiSpinnerVerbs[] = {
	"Thinking",  "Forming",   "Pondering", "Musing",
	"Brewing",   "Weaving",   "Crafting",  "Conjuring",
	"Distilling","Scheming",  "Plotting",  "Sifting",
	"Unraveling","Cooking",   "Stewing",   "Mulling",
	"Simmering", "Reckoning", "Percolating","Chewing",
};
constexpr int kGuiVerbCount = sizeof(kGuiSpinnerVerbs) / sizeof(kGuiSpinnerVerbs[0]);
} // namespace


// ===========================================================================
// WelcomeView — startup splash with the app icon + greeting text.
// ===========================================================================

WelcomeView::WelcomeView()
	: BView("welcome", B_WILL_DRAW)
{
	SetViewColor(kColorChatBg);
	SetLowColor(kColorChatBg);

	// Pull the HVIF icon stamped onto the binary at link time — the same
	// source the About box uses. 64x64 keeps it crisp on HiDPI displays.
	// We read the raw vector data and rasterise it ourselves: the
	// BAppFileInfo::GetIcon(BBitmap*, icon_size) overload requires the
	// bitmap bounds to match the enum (16 or 32 px) exactly, so it cannot
	// produce a 64x64 icon and silently fails with B_BAD_VALUE.
	app_info info;
	if (be_roster->GetRunningAppInfo(be_app->Team(), &info) == B_OK) {
		BFile appFile(&info.ref, B_READ_ONLY);
		BAppFileInfo fileInfo(&appFile);
		uint8* data = nullptr;
		size_t size = 0;
		if (fileInfo.GetIcon(&data, &size) == B_OK && data != nullptr) {
			// Render the vector HVIF into a scaled bitmap so the icon stays
			// crisp (not upscaled) on HiDPI displays.
			const int32 px = static_cast<int32>(ScalePx(64.0f)) - 1;
			BBitmap* icon = new BBitmap(BRect(0, 0, px, px), B_RGBA32);
			if (BIconUtils::GetVectorIcon(data, size, icon) == B_OK)
				fIcon = icon;
			else
				delete icon;
			free(data);
		}
	}

	// Reserve enough height for the icon plus two text lines. Pin a small
	// minimum WIDTH too: a plain BView with an unset min width reports its
	// current frame width as the minimum, which (at the initial window size)
	// latches the whole chat column — and thus the window — to a huge minimum
	// width, letting the bottom input bar overflow and clip its buttons. A
	// small explicit min lets the column and window shrink freely.
	SetExplicitMinSize(BSize(ScalePx(40), ScalePx(96)));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, ScalePx(96)));
}

WelcomeView::~WelcomeView()
{
	delete fIcon;
}

void WelcomeView::Draw(BRect /*updateRect*/)
{
	const BRect b = Bounds();

	// Icon on the left, vertically centred. fIcon is already rendered at the
	// scaled size, so use its real dimensions and scale the margins.
	const float margin = ScalePx(16.0f);
	float textLeft = margin;
	if (fIcon != nullptr) {
		const float iconH = fIcon->Bounds().Height() + 1.0f;
		const float iconW = fIcon->Bounds().Width() + 1.0f;
		const float iconY = (b.Height() - iconH) / 2.0f;
		SetDrawingMode(B_OP_ALPHA);
		DrawBitmap(fIcon, BPoint(margin, iconY));
		SetDrawingMode(B_OP_COPY);
		textLeft = margin + iconW + margin;
	}

	// Title line: bold "Claude" in the model accent colour.
	BFont titleFont(be_bold_font);
	titleFont.SetSize(titleFont.Size() * 1.6f);
	SetFont(&titleFont);
	SetHighColor(kColorModelLabel);
	const float titleY = b.Height() / 2.0f - ScalePx(6.0f);
	DrawString("Claude", BPoint(textLeft, titleY));

	// Subtitle: dim hint, regular font.
	BFont subFont(be_plain_font);
	SetFont(&subFont);
	SetHighColor(kColorToolLine);
	DrawString("Join the AI revolution, resistance is futile!",
	           BPoint(textLeft, titleY + ScalePx(22.0f)));
}


// ===========================================================================
// ChatWindow
// ===========================================================================

ChatWindow::ChatWindow(const config::Auth& auth, const std::string& model,
                        int maxTokens, const std::string& systemPrompt,
                        int notifyMinSec, const std::string& workingDir,
                        const std::string& initialPrompt, bool autoSend)
	: BWindow(BRect(100, 100, 900, 680), "Claude",
	           B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	, fAuth(auth)
	, fModel(model)
	, fConfigModel(model)
	, fMaxTokens(maxTokens)
	, fSystemPrompt(systemPrompt)
	, fWorkingDir(workingDir)
	, fMessages(nlohmann::json::array())
{
	fNotifyMinSec = (notifyMinSec < 0) ? 0 : notifyMinSec;
	// Try to load the Genio theme and language set.
	const std::string themePath = styling::FindDefaultTheme();
	const std::string langsDir  = styling::FindLanguagesDir();
	if (!themePath.empty() && fTheme.LoadFile(themePath)
	    && !langsDir.empty()  && fLangSet.LoadDir(langsDir)) {
		fStyler = new styling::CodeStyler(fTheme, fLangSet);
	}

	_BuildMenuBar();
	_BuildLayout();

	// Markdown renderer (needs fOutput to exist).
	fMdRenderer = new md::MdRenderer(fOutput);
	// ChatWindow owns scroll positioning: it does a single _ScrollToBottom()
	// per streamed chunk. Disable the renderer's own per-line ScrollToEnd()
	// calls so the two don't issue several competing ScrollToOffset()s while
	// the buffer is still reflowing — that fight is what makes the view
	// jitter up and down after Send when the user had scrolled up.
	fMdRenderer->SetAutoScroll(false);

	// Update title with model name.
	_UpdateTitle();

	// Register keyboard shortcuts.
	AddShortcut('L', B_COMMAND_KEY, new BMessage(gui::MSG_CLEAR_OUTPUT));
	AddShortcut(',', B_COMMAND_KEY, new BMessage(gui::MSG_SETTINGS));
	AddShortcut('F', B_COMMAND_KEY, new BMessage(gui::MSG_FIND));
	// Font zoom. Bind both '+' and '=' (same key, unshifted) for zoom-in
	// so the user doesn't have to hold Shift.
	AddShortcut('+', B_COMMAND_KEY, new BMessage(gui::MSG_ZOOM_IN));
	AddShortcut('=', B_COMMAND_KEY, new BMessage(gui::MSG_ZOOM_IN));
	AddShortcut('-', B_COMMAND_KEY, new BMessage(gui::MSG_ZOOM_OUT));
	AddShortcut('0', B_COMMAND_KEY, new BMessage(gui::MSG_ZOOM_RESET));

	// Restore window frame, zoom, and last model from the prefs file.
	_LoadGuiPrefs();

	// Seed the token bar's cost estimate from the (possibly restored)
	// model now that both the bar and final model are known.
	_UpdateTokenBarPrice();

	// Register this window as a live remote-control session so the phone
	// can /sessions list it and /session N route prompts here.
	_RegisterSession();

	// Seed an initial prompt supplied at launch (e.g. Genio's "Ask Claude
	// to fix this" via --prompt or the 'ASKP' message). Prefill the input
	// box so the user can review/edit it; auto-send only if explicitly
	// requested. Posted (not called inline) so it runs after the window is
	// shown and the layout has settled.
	if (!initialPrompt.empty()) {
		fInput->SetText(initialPrompt.c_str());
		if (autoSend) {
			BMessage send(gui::MSG_SEND);
			PostMessage(&send);
		} else {
			fInput->MakeFocus(true);
		}
	}
}

ChatWindow::~ChatWindow()
{
	// Leave the session registry first so no routed remote prompt can be
	// dispatched to this window while it is tearing down.
	_UnregisterSession();
	delete fStyler;
	delete fMdRenderer;
	// The worker is normally joined in QuitRequested() before the window
	// is destroyed. This is a defensive fallback: if a worker somehow
	// outlives that path, ask it to stop and join (never detach — that
	// would leave it racing on a freed fSink).
	if (fWorker.joinable()) {
		g_interrupted = 1;
		fWorker.join();
	}
	delete fSink;
	delete fSpinnerTimer;
	delete fExportPanel;
}

// ---------------------------------------------------------------------------
// _BuildMenuBar — native BMenuBar with File, Edit, and Help menus.
// ---------------------------------------------------------------------------

void ChatWindow::_BuildMenuBar()
{
	fMenuBar = new BMenuBar("menubar");

	// ── File ────────────────────────────────────────────────────────────────
	BMenu* fileMenu = new BMenu("File");
	// New Session spawns a fresh window with its own live session, so the
	// phone can /sessions list and /session N switch between them. Targets
	// be_app (the application) which owns window creation.
	BMenuItem* newWin = new BMenuItem("New Session",
		new BMessage(gui::MSG_NEW_WINDOW), 'N');
	newWin->SetTarget(be_app);
	fileMenu->AddItem(newWin);
	fileMenu->AddSeparatorItem();
	fileMenu->AddItem(new BMenuItem("Settings\xE2\x80\xA6", // …
		new BMessage(gui::MSG_SETTINGS), ','));
	fileMenu->AddItem(new BMenuItem("Export Transcript\xE2\x80\xA6", // …
		new BMessage(gui::MSG_EXPORT), 'E'));
	fileMenu->AddSeparatorItem();
	fileMenu->AddItem(new BMenuItem("Quit",
		new BMessage(B_QUIT_REQUESTED), 'Q'));
	fMenuBar->AddItem(fileMenu);

	// ── Edit ────────────────────────────────────────────────────────────────
	BMenu* editMenu = new BMenu("Edit");
	editMenu->AddItem(new BMenuItem("Clear Chat",
		new BMessage(gui::MSG_CLEAR_OUTPUT), 'L'));
	editMenu->AddItem(new BMenuItem("Find\xE2\x80\xA6", // …
		new BMessage(gui::MSG_FIND), 'F'));
	editMenu->AddSeparatorItem();
	editMenu->AddItem(new BMenuItem("Zoom In",
		new BMessage(gui::MSG_ZOOM_IN), '+'));
	editMenu->AddItem(new BMenuItem("Zoom Out",
		new BMessage(gui::MSG_ZOOM_OUT), '-'));
	editMenu->AddItem(new BMenuItem("Actual Size",
		new BMessage(gui::MSG_ZOOM_RESET), '0'));
	editMenu->AddSeparatorItem();
	editMenu->AddItem(new BMenuItem("Compact Conversation",
		new BMessage(gui::MSG_COMPACT), 'K'));
	editMenu->AddSeparatorItem();
	editMenu->AddItem(new BMenuItem("Clear Prompt History" B_UTF8_ELLIPSIS,
		new BMessage(gui::MSG_CLEAR_HISTORY)));
	fMenuBar->AddItem(editMenu);

	// ── View ────────────────────────────────────────────────────────────────
	BMenu* viewMenu = new BMenu("View");
	viewMenu->AddItem(new BMenuItem("Sessions",
		new BMessage(gui::MSG_TOGGLE_SESSIONS), 'B'));
	fInputItem = new BMenuItem("Input",
		new BMessage(gui::MSG_TOGGLE_INPUT), 'I');
	fInputItem->SetMarked(true);   // input bar is shown by default
	viewMenu->AddItem(fInputItem);
	fMenuBar->AddItem(viewMenu);

	// ── Tools ───────────────────────────────────────────────────────────────
	// Ludicrous mode: auto-approves every tool permission for the session.
	// The menu item carries a checkmark that mirrors api::g_ludicrous_mode.
	BMenu* toolsMenu = new BMenu("Tools");
	fLudicrousItem = new BMenuItem(
		"\xE2\x9A\xA1 Ludicrous Mode  \xE2\x80\x94  auto-approve all tools",
		new BMessage(gui::MSG_LUDICROUS));
	fLudicrousItem->SetMarked(api::g_ludicrous_mode.load());
	toolsMenu->AddItem(fLudicrousItem);

	// Remote control: starts a background Telegram poller so allowed users can
	// drive turns on this machine. The checkmark mirrors fRemote's running
	// state, kept in sync by _ToggleRemote(). The item is disabled (grayed
	// out) when there is no valid Telegram config, so it is visibly
	// unavailable rather than erroring on click.
	fRemoteItem = new BMenuItem(
		"\xF0\x9F\x93\xA1 Remote Control  \xE2\x80\x94  Telegram bridge",
		new BMessage(gui::MSG_REMOTE_CONTROL));
	fRemoteItem->SetMarked(false);
	std::string remoteWhy;
	const bool remoteConfigured =
		telegram::RemoteControl::ConfigIsValid(config::Load(), &remoteWhy);
	fRemoteItem->SetEnabled(remoteConfigured);
	toolsMenu->AddItem(fRemoteItem);
	fMenuBar->AddItem(toolsMenu);

	// ── Help ────────────────────────────────────────────────────────────────
	BMenu* helpMenu = new BMenu("Help");
	helpMenu->AddItem(new BMenuItem("Documentation",
		new BMessage(gui::MSG_HELP_DOCS)));
	helpMenu->AddItem(new BMenuItem("Show Markdown Demo",
		new BMessage(gui::MSG_DEMO_MARKDOWN)));
	helpMenu->AddSeparatorItem();
	helpMenu->AddItem(new BMenuItem("About Claude\xE2\x80\xA6", // …
		new BMessage(gui::MSG_ABOUT)));
	fMenuBar->AddItem(helpMenu);

	// NOTE: the menu bar is added to the window's layout (as the top row)
	// in _BuildLayout, not AddChild'd here — mixing a manually-added menu
	// bar with a window-wide BLayoutBuilder layout makes the layout cover
	// the full height (including under the menu bar), which on resize
	// pushes the bottom input bar off-screen.
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ChatWindow::_BuildLayout()
{
	// ── Output BTextView (always-dark chat area) ─────────────────────────────
	// B_FOLLOW_ALL so the text view fills its enclosing BScrollView (the chat
	// uses all available space). The scroll view — not this view — is what
	// the window layout sizes; we give fScroll explicit min AND max sizes
	// below so the layout is free to shrink/grow it without the BTextView's
	// content-derived minimum freezing the whole window's width.
	fOutput = new ChatTextView(BRect(0, 0, 600, 400), "output",
	                        BRect(4, 4, 596, 396),
	                        B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS);
	fOutput->MakeEditable(false);
	fOutput->MakeSelectable(true);
	fOutput->SetWordWrap(true);
	fOutput->SetStylable(true);
	fOutput->SetViewColor(kColorChatBg);
	fOutput->SetLowColor(kColorChatBg);
	fOutput->SetHighColor(kColorText);
	fOutput->SetFontAndColor(be_fixed_font, B_FONT_ALL, &kColorText);
	// Pin a small explicit minimum directly on the text view. A bare
	// BTextView reports a content-derived minimum (often its current
	// width), which propagates up through the BScrollView and the
	// BSplitView and freezes the whole window's minimum width at the
	// current content width. An explicit small min keeps the window
	// freely shrinkable; B_FOLLOW_ALL still lets it fill the scroll view.
	fOutput->SetExplicitMinSize(BSize(80, 40));

	fScroll = new BScrollView("scroll", fOutput,
	                          B_FOLLOW_NONE, 0, false, true, B_FANCY_BORDER);
	// Decouple the chat's layout size from the BTextView's content-derived
	// size: an explicit min lets it yield space to the fixed bottom strip,
	// and an explicit *max* of unlimited tells the layout it may grow to
	// fill all available space (so the chat uses the whole window) without
	// the inner BTextView's intrinsic minimum freezing the window width.
	fScroll->SetExplicitMinSize(BSize(120, 50));
	fScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	// Also pin the *preferred* width small. The BScrollView otherwise
	// derives a preferred width from its target's current content width and
	// the window's BGroupLayout latches that as a minimum, re-freezing the
	// window (and clipping the bottom buttons) once the frame shrinks past
	// the current content width.
	fScroll->SetExplicitPreferredSize(BSize(120, 50));

	// ── Welcome splash (shown above the chat until the first turn) ───────────
	fWelcome = new WelcomeView();

	// Floating jump-to-bottom button (overlaid, repositioned in FrameResized).
	fJumpBtn = new BButton("jumpbtn", "\xE2\x86\x93 New", // ↓
	                        new BMessage(gui::MSG_JUMP_BOTTOM));
	fJumpBtn->SetExplicitSize(BSize(ScalePx(80), ScalePx(26)));
	fJumpBtn->Hide();
	AddChild(fJumpBtn); // added directly to window, not layout

	// ── Token bar ────────────────────────────────────────────────────────────
	fTokenBar = new TokenBar();
	// Mirror any pre-existing ludicrous state (e.g. restored session) so the
	// badge matches the Tools menu checkmark from the start.
	fTokenBar->SetLudicrous(api::g_ludicrous_mode.load());

	// ── Session sidebar (left dock, hidden until View ▸ Sessions) ─────────────
	fSessionList = new SessionListView("sessionlist", this);
	fSessionList->SetSelectionMessage(nullptr); // single-click just highlights
	fSessionList->SetInvocationMessage(           // double-click loads
		new BMessage(gui::MSG_SESSION_SELECT));
	fSessionScroll = new BScrollView("sessionscroll", fSessionList,
	                                 0, false, true, B_FANCY_BORDER);
	// A BListView reports a content-derived minimum width (the widest session
	// title), which propagates up through the scroll view, the session panel
	// and the horizontal split, freezing the whole window's minimum width far
	// wider than it should be — and that in turn lets the bottom input bar
	// overflow to the right and clip the buttons once the window is narrowed.
	// Pin a small explicit min on both the list and its scroller so the
	// sidebar (and the window) can shrink freely; the splitter governs the
	// real width.
	fSessionList->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
	fSessionScroll->SetExplicitMinSize(BSize(60, 50));
	fSessionScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	BButton* sessNew = new BButton("sessnew", "New",
	                               new BMessage(gui::MSG_SESSION_NEW));
	BButton* sessOpen = new BButton("sessopen", "Open",
	                                new BMessage(gui::MSG_SESSION_SELECT));
	BButton* sessRen = new BButton("sessren", "Rename",
	                               new BMessage(gui::MSG_SESSION_RENAME));
	BButton* sessDel = new BButton("sessdel", "Delete",
	                               new BMessage(gui::MSG_SESSION_DELETE));
	fSessionPanel = new BView("sessionpanel", B_WILL_DRAW | B_SUPPORTS_LAYOUT);
	fSessionPanel->SetResizingMode(B_FOLLOW_NONE);
	fSessionPanel->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	// Keep the action buttons grouped at the left at a uniform width
	// (so the 2x2 grid lines up) rather than stretching to fill the
	// panel. A trailing glue on each row pushes them left no matter how
	// wide the sidebar is dragged.
	float btnW = 0.0f;
	for (BButton* b : { sessNew, sessOpen, sessRen, sessDel })
		btnW = std::max(btnW, b->PreferredSize().Width());
	for (BButton* b : { sessNew, sessOpen, sessRen, sessDel })
		b->SetExplicitSize(BSize(btnW, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(fSessionPanel, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS)
		.Add(new BStringView("sesshdr", "Sessions"), 0.0f)
		.Add(fSessionScroll, 1.0f)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(sessNew, 0.0f)
			.Add(sessOpen, 0.0f)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(sessRen, 0.0f)
			.Add(sessDel, 0.0f)
			.AddGlue()
		.End()
	.End();
	// Keep the panel usable but let the window shrink: a small min-width
	// (the splitter governs the actual width). A large min here would be
	// summed into the window's minimum and block reducing the window.
	fSessionPanel->SetExplicitMinSize(BSize(ScalePx(80), B_SIZE_UNSET));
	fSessionPanel->SetExplicitMaxSize(BSize(ScalePx(500), B_SIZE_UNLIMITED));

	// ── Input area ───────────────────────────────────────────────────────────
	fInput = new InputView("input");
	// Wrap the input in a BScrollView so a vertical scrollbar appears on the
	// right when the typed text grows taller than the visible area. Horizontal
	// scrolling stays off (the input word-wraps).
	fInputScroll = new BScrollView("inputscroll", fInput,
		B_FOLLOW_ALL, B_FRAME_EVENTS, false /*horizontal*/, true /*vertical*/,
		B_NO_BORDER);
	// A BTextView only grows to its content height, so when the typed text is
	// shorter than the input area the scroll view shows its own background
	// below the text — a grey strip against the dark input. Paint the scroll
	// view (and its inner document area) in the same dark chat colour so that
	// empty region blends seamlessly with the input instead of showing grey.
	fInputScroll->SetViewColor(kColorChatBg);
	fInputScroll->SetLowColor(kColorChatBg);
	// Host the scroll view in an InputContainer (Genio TerminalTab pattern):
	// the container claims the layout space and manually resizes the scroll
	// view to fill it, so the input reliably takes the whole bottom-bar area
	// without any content-driven size overrides. The starting height is a
	// few text lines tall.
	font_height ifh;
	be_plain_font->GetHeight(&ifh);
	const float kInputLineH = ifh.ascent + ifh.descent + ifh.leading + 1.0f;
	const float kInputRowH  = 5.0f * kInputLineH + 12.0f;
	fInputHost = new InputContainer("inputhost");
	fInputHost->SetContent(fInputScroll, kInputRowH);

	// ── Model picker ─────────────────────────────────────────────────────────
	fModelMenu  = new BPopUpMenu(fModel.c_str());
	fModelField = new BMenuField("modelpicker", "Model:", fModelMenu);
	_PopulateModelMenu();

	// ── Settings dialog (separate window, created hidden) ─────────────────────
	// The model picker lives inside the dialog (reparented there by the
	// layout builder). The dialog is shown/hidden via File ▸ Settings.
	fSettings = new SettingsDialog(this, fSystemPrompt, fMaxTokens,
	                               fNotifyMinSec, fWorkingDir, fModelField);

	// ── Find bar (hidden until Cmd-F) ─────────────────────────────────────────
	// A thin horizontal strip: query field | ◀ | ▶ | counter | ✕.
	fFindField = new BTextControl("findfield", nullptr, "",
	                              new BMessage(gui::MSG_FIND_NEXT));
	fFindField->SetModificationMessage(new BMessage(gui::MSG_FIND_LIVE));
	// Esc inside the find field closes the find bar. The filter must go on the
	// inner BTextView (the view that is actually focused and receives key
	// events) — a filter on the BTextControl itself never sees the keystroke.
	if (BTextView* findTV = fFindField->TextView()) {
		findTV->AddFilter(new BMessageFilter(B_KEY_DOWN,
			[](BMessage* msg, BHandler** /*target*/, BMessageFilter* filter)
				-> filter_result {
				const char* bytes = nullptr;
				if (msg->FindString("bytes", &bytes) == B_OK
				    && bytes != nullptr && bytes[0] == B_ESCAPE) {
					if (BLooper* looper = filter->Looper())
						looper->PostMessage(gui::MSG_FIND_CLOSE);
					return B_SKIP_MESSAGE;
				}
				return B_DISPATCH_MESSAGE;
			}));
	}
	fFindStatus = new BStringView("findstatus", "");
	BButton* findPrev  = new BButton("findprev",  "\xE2\x97\x80", // ◀
	                                  new BMessage(gui::MSG_FIND_PREV));
	BButton* findNext  = new BButton("findnext",  "\xE2\x96\xB6", // ▶
	                                  new BMessage(gui::MSG_FIND_NEXT));
	BButton* findClose = new BButton("findclose", "\xE2\x9C\x95", // ✕
	                                  new BMessage(gui::MSG_FIND_CLOSE));
	findPrev->SetExplicitSize(BSize(ScalePx(32), B_SIZE_UNSET));
	findNext->SetExplicitSize(BSize(ScalePx(32), B_SIZE_UNSET));
	findClose->SetExplicitSize(BSize(ScalePx(32), B_SIZE_UNSET));

	fFindBar = new BView("findbar", B_WILL_DRAW | B_SUPPORTS_LAYOUT);
	fFindBar->SetResizingMode(B_FOLLOW_NONE);
	fFindBar->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(fFindBar, B_HORIZONTAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_INSETS, 4, B_USE_SMALL_INSETS, 4)
		.Add(new BStringView("findlabel", "Find:"), 0.0f)
		.Add(fFindField, 1.0f)
		.Add(findPrev, 0.0f)
		.Add(findNext, 0.0f)
		.Add(fFindStatus, 0.0f)
		.Add(findClose, 0.0f)
	.End();

	// ── Layout ────────────────────────────────────────────────────────────────
	// The session sidebar and chat area sit in a horizontal BSplitView so the
	// sidebar divider can be dragged. The chat column (welcome splash +
	// scroll) is the second item. It is a BGroupView so its own layout
	// reflows its children on resize (Genio uses group views for such
	// columns rather than bare B_FOLLOW_NONE BViews with explicit-size pins).
	BGroupView* chatColumn = new BGroupView("chatcolumn", B_VERTICAL, 0.0f);
	BLayoutBuilder::Group<>(chatColumn)
		.Add(fWelcome, 0.0f)
		.Add(fScroll, 1.0f)
	.End();

	fSplit = new BSplitView(B_HORIZONTAL, 1.0f);
	fSplit->SetName("mainsplit");
	BLayoutBuilder::Split<>(fSplit)
		.Add(fSessionPanel, 0.28f)   // sidebar  — collapsible
		.Add(chatColumn,    0.72f);  // chat     — takes the rest
	// The sidebar is collapsible; the chat (index 1) is not.
	fSplit->SetCollapsible(0, true);

	// ── Bottom input pane ─────────────────────────────────────────────────────
	// Built to match Genio's ConsoleIOTabView verbatim — the golden-standard
	// pane with a content area and a side button column:
	//
	//   Group(this, B_HORIZONTAL, 0.0f)
	//     .Add(content, 3.0f)
	//     .AddGroup(B_VERTICAL, 0.0f)
	//       .SetInsets(B_USE_SMALL_SPACING)
	//       .AddGlue()
	//       .Add(button)…
	//     .End()
	//
	// The input takes weight 3 (fills all leftover width); the buttons live
	// in a weight-0 vertical sub-group with a leading glue, so they keep
	// their natural width pinned to the right and never get clipped or
	// stretched. fInputBar is a BGroupView so its own layout reflows its
	// children on a live resize (no LayoutView/InvalidateTree hack needed).
	// ── Bottom input pane ─────────────────────────────────────────────────────
	// Genio ConsoleIOTabView idiom: input fills the width (weight 1), a
	// compact button column sits on the right. The input scroll view is told
	// to FILL the row height (B_SIZE_UNLIMITED max) so it is exactly as tall
	// as the button column — no top/bottom misalignment or dead gap. The row
	// height is therefore driven by the buttons (two visible at a time, since
	// Send and Stop share a slot), giving a comfortable multi-line input.
	fSend = new BButton("send", "Send", new BMessage(gui::MSG_SEND));
	fSend->MakeDefault(true);
	fStop = new BButton("stop", "Stop", new BMessage(gui::MSG_CANCEL));
	fStop->Hide();   // hidden until busy; Send shows in its place
	fClearBtn = new BButton("clearbtn", "Clear",
		new BMessage(gui::MSG_CLEAR_OUTPUT));
	// A uniform button width keeps the column tidy.
	float inBtnW = std::max({ fSend->PreferredSize().Width(),
	                          fStop->PreferredSize().Width(),
	                          fClearBtn->PreferredSize().Width() });
	fSend->SetExplicitSize(BSize(inBtnW, B_SIZE_UNSET));
	fStop->SetExplicitSize(BSize(inBtnW, B_SIZE_UNSET));
	fClearBtn->SetExplicitSize(BSize(inBtnW, B_SIZE_UNSET));

	fInputBar = new BGroupView("inputbar", B_HORIZONTAL, B_USE_SMALL_SPACING);
	fInputBar->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(fInputBar)
		.SetInsets(0.0f, 0.0f, 0.0f, 0.0f)
		.Add(fInputHost, 1.0f)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 0.0f)
			.SetInsets(0.0f, 0.0f, 2.0f, 2.0f)
			// Trailing glue pushes the button column to the TOP of the input
			// pane, so Send/Stop/Clear sit at the top edge of the input area.
			.Add(fSend)
			.Add(fStop)
			.Add(fClearBtn)
			.AddGlue()
		.End()
	.End();

	// ── Bottom pane container ──────────────────────────────────────────────────
	// The find bar and input bar form one logical "input pane" that docks
	// against the chat area. Grouping them into a single BGroupView lets the
	// whole pane be the bottom item of a vertical BSplitView, so the divider
	// between chat and input is draggable (Genio editor/output split idiom).
	// The token bar is NOT part of this pane: it belongs to the chat area
	// above the divider, so the divider sits directly below the token bar and
	// the usage readout stays pinned to the chat output it describes.
	BGroupView* inputPane = new BGroupView("inputpane", B_VERTICAL, 0.0f);
	fInputPane = inputPane;   // keep a member handle for show/hide toggling
	inputPane->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(inputPane)
		.Add(fFindBar, 0.0f)
		.Add(fInputBar, 1.0f)
	.End();
	// Do NOT force an explicit minimum on the pane. Genio's ConsoleIOTabView
	// lets the layout engine derive the pane's minimum from its children, so
	// the button column (Send/Stop/Clear) always reserves its full height and
	// the buttons never clip when the window or splitter shrinks the pane.
	// The input host carries its own small min; the derived pane min is the
	// taller of the input row and the button column.

	// ── Chat area (top of the vertical split) ──────────────────────────────────
	// The sidebar/chat split plus the token bar form the top item. Keeping the
	// token bar here — above the divider — means the draggable divider sits
	// directly below the token bar, and the usage readout stays attached to the
	// chat output it summarises rather than riding up and down with the input.
	BGroupView* chatArea = new BGroupView("chatarea", B_VERTICAL, 0.0f);
	chatArea->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(chatArea)
		.Add(fSplit,    1.0f)
		.Add(fTokenBar, 0.0f)
	.End();

	// ── Vertical split: chat (top) over input pane (bottom) ────────────────────
	// Mirrors Genio's nested AddSplit(B_VERTICAL): the chat area (sidebar/chat
	// split + token bar) is the top item, the input pane the bottom item, so
	// dragging the divider trades height between the chat output and the input.
	fVSplit = new BSplitView(B_VERTICAL, 1.0f);
	fVSplit->SetName("vsplit");
	BLayoutBuilder::Split<>(fVSplit)
		.Add(chatArea,  0.89f)   // chat area — takes most of the height
		.Add(inputPane, 0.11f);  // input pane — collapsible (~half its old size)
	// The input pane is collapsible; the chat (index 0) is not.
	fVSplit->SetCollapsible(0, false);
	fVSplit->SetCollapsible(1, true);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fMenuBar, 0.0f)
		.Add(fVSplit, 1.0f)
	.End();

	// ── Window minimum size ─────────────────────────────────────────────────
	// A BSplitView (and a layout group holding a manually-resized child) will
	// NOT shrink a child below its minimum size — instead it lets the child
	// overflow past the window edge, clipping content. Vertically that hangs
	// the input pane off the bottom; horizontally it pushes the Send/Stop/
	// Clear button column off the right edge. The only robust fix — and what
	// Genio relies on for its Console I/O tab — is to forbid the window from
	// ever being smaller than what the layout genuinely needs, using the
	// views' real derived minimums (not magic numbers, and not Frame(), which
	// is still empty before the first layout pass).

	const float kInputPaneMinH = fInputBar->MinSize().height;
	// Keep the chat area itself usable; its derived min (a tiny BTextView min
	// plus the token bar) can be near-zero, so floor it to a few visible lines.
	const float kChatAreaMinH  = std::max(ScalePx(120.0f), chatArea->MinSize().height);
	const float kMenuH         = fMenuBar ? fMenuBar->PreferredSize().height
	                                      : ScalePx(20.0f);
	const float kSplitterH     = fVSplit->SplitterSize();

	const float minH = kMenuH + kChatAreaMinH + kSplitterH + kInputPaneMinH
	                 + ScalePx(8.0f) /*frame slack*/;

	// Horizontal floor: the window must be at least as wide as the widest of
	// its full-width rows — the input bar (input field + button column) and
	// the chat area (chat split + token bar) — plus a little frame slack, so
	// the button column is never pushed off the right edge.
	const float minW = std::max({ ScalePx(360.0f),
	                              fInputBar->MinSize().width + ScalePx(8.0f),
	                              chatArea->MinSize().width + ScalePx(8.0f) });
	SetSizeLimits(minW, 32767, minH, 32767);
	fWindowMinH = minH;
	fWindowMinW = minW;

	// Belt-and-suspenders: pin the pane's minimum height to the input bar's
	// own derived minimum (the button column / input row), NOT the whole
	// pane's (which includes the currently-hidden find bar). This gives the
	// vertical BSplitView an unambiguous floor so it can never crush the pane
	// and clip the Send/Stop/Clear buttons, without wasting input height on
	// the find bar while it is hidden.
	inputPane->SetExplicitMinSize(BSize(B_SIZE_UNSET,
	                                     fInputBar->MinSize().height));

	// Find bar starts hidden; Cmd-F reveals it.
	if (fFindBar) fFindBar->Hide();

	// Session sidebar starts hidden; View ▸ Sessions reveals it.
	if (fSessionPanel) fSessionPanel->Hide();

	// Enlarge the default window on HiDPI so the initial size stays
	// proportional to the scaled widgets (the constructor's BRect is the
	// 1x baseline of 800x580). Only grow — never shrink below the default.
	const float scale = gui_scale();
	if (scale > 1.0f) {
		ResizeTo(std::ceil(800.0f * scale), std::ceil(580.0f * scale));
		CenterOnScreen();
	}

	// Load persisted prompt history so Up/Down recalls prompts from previous
	// GUI sessions (a plain-text file, separate from the CLI's libedit
	// repl_history).
	if (fInput) fInput->LoadHistory(paths::GuiHistoryPath());

	// Give input focus on startup.
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

	// Background fetch — replace menu items when results arrive.
	const config::Auth auth = fAuth;
	std::thread([auth, this]() {
		std::vector<models::ModelEntry> fetched = models::FetchModels(auth);
		if (fetched.empty()) return;
		// Pack ids into a BMessage and post to ourselves.
		BMessage* ready = new BMessage(gui::MSG_MODELS_READY);
		for (const auto& e : fetched) {
			ready->AddString("id",   e.id.c_str());
			ready->AddString("name", e.display_name.c_str());
		}
		BMessenger(this).SendMessage(ready);
		delete ready;
	}).detach();
}

void ChatWindow::_RepositionOverlays()
{
	if (!fJumpBtn || !fScroll) return;
	const BRect sb = fScroll->Frame();
	const float bw = fJumpBtn->Frame().Width();
	const float bh = fJumpBtn->Frame().Height();
	fJumpBtn->MoveTo(sb.right - bw - 12.0f, sb.bottom - bh - 12.0f);
}

// Re-assert the window's size limits after the base class has shown the
// window and the layout has run its first pass. A window-level BLayout
// otherwise overwrites the limits we set in _BuildLayout with the layout's
// own (too-wide) computed MinSize, which blocks horizontal shrinking.
void ChatWindow::Show()
{
	BWindow::Show();
	if (Lock()) {
		SetSizeLimits(fWindowMinW, 32767, fWindowMinH, 32767);
		Unlock();
	}
}

// Recompute the input scroll view's maximum height so it never takes more
// than 30 % of the window height (with a hard floor of kMinLines).
void ChatWindow::FrameResized(float w, float h)
{
	BWindow::FrameResized(w, h);

	// The BSplitView does not always track the window's content width on a
	// live resize — it leaves its children (and thus the chat column) frozen
	// at their previous width, pushing content off the right edge. Nudging
	// just the split's layout makes it re-fit its items to the new width.
	if (fSplit) {
		fSplit->InvalidateLayout(true);
		fSplit->Relayout();
	}
	if (fVSplit) {
		fVSplit->InvalidateLayout(true);
		fVSplit->Relayout();
	}

	// During a live drag the app_server issues a continuous stream of
	// FrameResized events but does not always repaint layout-managed children
	// that sit beside a manually-resized B_FOLLOW_ALL view (the InputContainer)
	// — the Send/Stop/Clear column can be left un-redrawn and appear to vanish.
	// Force the whole input bar (and each button) to repaint every tick so the
	// buttons stay on screen throughout the drag.
	if (fInputBar) {
		fInputBar->Invalidate();
		if (fSend) fSend->Invalidate();
		if (fStop) fStop->Invalidate();
		if (fClearBtn) fClearBtn->Invalidate();
	}

	if (fInputPane && fInputPane->IsHidden()
			&& fInputItem && fInputItem->IsMarked()) {
		fInputPane->Show();
	}
	// The TokenBar text is drawn by hand; repaint it so its right-aligned
	// context label re-runs the collision logic at the new width.
	if (fTokenBar)
		fTokenBar->Invalidate();
	_RepositionOverlays();
	// Keep output text rect in sync with the view bounds so word-wrap
	// and Insert() render correctly after the window is resized.
	if (fOutput) {
		BRect b = fOutput->Bounds();
		fOutput->SetTextRect(b.InsetByCopy(4.0f, 4.0f));
	}
	if (fInput && fInput->IsFocus())
		fInput->Invalidate();
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
		// fSettings is a separate BWindow (its own looper); lock it before
		// touching its views from this (the main window's) thread.
		if (fSettings && fSettings->Lock()) {
			if (fSettings->IsOpen()) {
				// Dialog is open — closing it: read back edited values.
				fSystemPrompt         = fSettings->SystemPrompt();
				fMaxTokens            = fSettings->MaxTokens();
				fNotificationsEnabled = fSettings->NotificationsEnabled();
				fNotifyMinSec         = fSettings->NotifyMinSeconds();
				fWorkingDir           = fSettings->WorkingDir();
			}
			fSettings->Toggle();
			fSettings->Unlock();
		}
		break;

	case gui::MSG_BROWSE_WORKDIR: {
		// Open a directory-picker panel. The reply goes to this window
		// as B_REFS_RECEIVED so we can extract the chosen path and push
		// it back into the working-dir field.
		BFilePanel* panel = new BFilePanel(B_OPEN_PANEL,
		                                   new BMessenger(this),
		                                   nullptr,
		                                   B_DIRECTORY_NODE,
		                                   false); // single selection
		panel->SetButtonLabel(B_DEFAULT_BUTTON, "Select");
		panel->Show();
		// panel deletes itself via BFilePanel's built-in quit handling.
		break;
	}

	case gui::MSG_FIND:
		_ToggleFindBar();
		break;

	case gui::MSG_ZOOM_IN:
		_Zoom(+1);
		break;

	case gui::MSG_ZOOM_OUT:
		_Zoom(-1);
		break;

	case gui::MSG_ZOOM_RESET:
		_Zoom(0);
		break;

	case gui::MSG_TOGGLE_SESSIONS:
		_ToggleSessionList();
		break;

	case gui::MSG_TOGGLE_INPUT:
		// Hide/show the entire bottom split item (find bar + input bar), not
		// just the input bar inside it — otherwise the empty pane slot stays
		// in the split and shows as a grey strip. Hiding the whole pane lets
		// the vertical split give its space back to the chat area, so the chat
		// fills down to the window's bottom edge.
		if (fInputPane) {
			if (fInputPane->IsHidden()) {
				fInputPane->Show();
				if (fInputItem) fInputItem->SetMarked(true);
				if (fInput) fInput->MakeFocus(true);
			} else {
				fInputPane->Hide();
				if (fInputItem) fInputItem->SetMarked(false);
			}
			if (fVSplit) {
				fVSplit->InvalidateLayout(true);
				fVSplit->Relayout();
			}
		}
		break;

	case gui::MSG_SESSION_SELECT:
		_LoadSelectedSession();
		break;

	case gui::MSG_SESSION_DELETE:
		_DeleteSelectedSession();
		break;

	case gui::MSG_SESSION_RENAME:
		_RenameSelectedSession();
		break;

	case gui::MSG_COMPACT:
		_LaunchCompact();
		break;

	case gui::MSG_CLEAR_HISTORY: {
		// Confirm before wiping persisted prompt history.
		const size_t n = fInput ? fInput->HistoryCount() : 0;
		if (n == 0) {
			BAlert* none = new BAlert("Prompt History",
				"There are no saved prompts to clear.", "OK");
			none->SetType(B_INFO_ALERT);
			none->Go();
			break;
		}
		BString msg;
		msg.SetToFormat("Clear all %zu saved prompt%s? This cannot be undone.",
		                n, n == 1 ? "" : "s");
		BAlert* confirm = new BAlert("Clear Prompt History", msg.String(),
			"Cancel", "Clear", nullptr,
			B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		confirm->SetShortcut(0, B_ESCAPE);
		if (confirm->Go() == 1) {   // "Clear"
			if (fInput) fInput->ClearHistory();
			// Remove the on-disk file so the cleared state persists.
			BEntry(paths::GuiHistoryPath().c_str()).Remove();
		}
		break;
	}

	case gui::MSG_SESSION_NEW:
		_NewChat();
		_RefreshSessionList();
		break;

	case gui::MSG_FIND_NEXT:
		_FindNext(true);
		break;

	case gui::MSG_FIND_LIVE:
		// Query text changed — re-search from the top without advancing
		// past the current match.
		fFindMatchStart = -1;
		_FindNext(true);
		break;

	case gui::MSG_FIND_PREV:
		_FindNext(false);
		break;

	case gui::MSG_FIND_CLOSE:
		if (fFindBar && !fFindBar->IsHidden()) {
			fFindBar->Hide();
			fInput->MakeFocus(true);
		}
		break;

	case gui::MSG_EXPORT: {
		// Nothing to export on an empty conversation.
		if (fMessages.empty()) {
			BAlert* alert = new BAlert("Export Transcript",
			    "There is no conversation to export yet.",
			    "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT);
			alert->Go();
			break;
		}
		// Lazily create the save panel; its B_SAVE_REQUESTED reply is
		// retargeted to this window as MSG_EXPORT_SAVE.
		if (!fExportPanel) {
			fExportPanel = new BFilePanel(B_SAVE_PANEL,
			                              new BMessenger(this),
			                              nullptr, 0, false,
			                              new BMessage(gui::MSG_EXPORT_SAVE));
			fExportPanel->SetButtonLabel(B_DEFAULT_BUTTON, "Export");
		}
		// Suggest a filename derived from the conversation topic.
		std::string suggested = fConvTopic.empty() ? "transcript" : fConvTopic;
		for (char& c : suggested)
			if (c == '/' || c == ':' || c < 32) c = ' ';
		if (suggested.size() > 60) suggested.resize(60);
		suggested += ".md";
		fExportPanel->SetSaveText(suggested.c_str());
		fExportPanel->Show();
		break;
	}

	case gui::MSG_EXPORT_SAVE: {
		// B_SAVE_REQUESTED reply: directory ref + chosen "name".
		entry_ref dirRef;
		const char* name = nullptr;
		if (msg->FindRef("directory", &dirRef) == B_OK
				&& msg->FindString("name", &name) == B_OK && name) {
			BPath dir(&dirRef);
			if (dir.InitCheck() == B_OK) {
				BPath full(dir.Path(), name);
				_ExportTranscript(full.Path());
			}
		}
		break;
	}

	case B_SIMPLE_DATA:
	case B_REFS_RECEIVED: {
		// If the message carries a directory ref from the working-dir
		// BFilePanel, push it into the settings dialog; otherwise treat it
		// as a file-drop and forward to _RefsReceived.
		bool handledByPanel = false;
		if (fSettings) {
			entry_ref ref;
			if (msg->FindRef("refs", &ref) == B_OK) {
				BEntry entry(&ref);
				if (entry.IsDirectory()) {
					BPath path(&ref);
					// fSettings is a separate looper — lock before touching
					// its views from the main window thread.
					if (path.InitCheck() == B_OK && fSettings->Lock()) {
						fSettings->SetValues(fSettings->SystemPrompt(),
						                     fSettings->MaxTokens(),
						                     fSettings->NotifyMinSeconds(),
						                     path.Path());
						fSettings->Unlock();
						handledByPanel = true;
					}
				}
			}
		}
		if (!handledByPanel)
			_RefsReceived(msg);
		break;
	}

	case gui::MSG_MODELS_READY: {
		// Background fetch returned — rebuild the model menu with live data.
		// The menu lives in the settings dialog (a separate looper), so
		// lock that window before mutating it.
		if (!fModelMenu) break;
		const bool dlgLocked = fSettings && fSettings->Lock();
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
		if (dlgLocked) fSettings->Unlock();
		break;
	}

	case gui::MSG_MODEL_PICK: {
		const char* model = nullptr;
		if (msg->FindString("model", &model) == B_OK && model) {
			fModel = model;
			_UpdateTitle();
			_UpdateTokenBarPrice();
		}
		break;
	}

	// ── Spinner tick ─────────────────────────────────────────────────────────
	case gui::MSG_TICK:
		_SpinnerTick();
		break;

	// ── Worker → window messages ─────────────────────────────────────────────
	case gui::MSG_CHUNK: {
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			// Decide once, before inserting, whether to keep the view pinned
			// to the bottom: only if the user was already near the bottom.
			// If they had scrolled up to read, leave their position alone so
			// streaming text doesn't yank (and jitter) the view back down.
			const bool stick = _IsNearBottom();

			// First real output of the turn — erase the thinking spinner.
			_SpinnerStop();
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

			// Exactly one scroll per chunk (the renderer's own scrolling is
			// disabled), so the view settles in a single move instead of
			// fighting several competing ScrollToOffset() calls.
			if (stick)
				_ScrollToBottom();
			else if (fJumpBtn && fJumpBtn->IsHidden())
				fJumpBtn->Show();  // let the user jump back down when ready
		}
		break;
	}

	case gui::MSG_DONE:
		if (fInCodeBlock) { fInCodeBlock = false; _FlushCodeBlock(); }
		// Emit a trailing partial line before flushing so it isn't lost.
		if (!fLineBuffer.empty() && !fInCodeBlock) {
			if (fMdRenderer) fMdRenderer->Write(fLineBuffer);
			else             _AppendText(fLineBuffer);
		}
		fLineBuffer.clear();
		if (fMdRenderer)  fMdRenderer->Flush();
		fInWebFetch = false;
		fWebFetchBuf.clear();
		break;

	case gui::MSG_TOOL_START: {
		// A tool may run before any assistant text — clear the spinner.
		_SpinnerStop();
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
		line += "\n";
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

	case gui::MSG_TOOL_DIFF: {
		_SpinnerStop();
		// Render a structured diff (from Edit/Write tools) as coloured lines.
		// Each line in the "diff" string starts with a sigil:
		//   '!' — header / meta line (path, ellipsis notice)
		//   '+' — added line (green)
		//   '-' — removed line (red)
		//   ' ' — context line (dim grey)
		const char* rawDiff = nullptr;
		msg->FindString("diff", &rawDiff);
		if (!rawDiff || !rawDiff[0]) break;

		// Blank line before the diff block for visual separation.
		AppendWithColor(fOutput, "\n", kColorToolLine, fZoomFactor);

		std::istringstream iss(rawDiff);
		std::string line;
		while (std::getline(iss, line)) {
			if (line.empty()) {
				AppendWithColor(fOutput, "\n", kColorToolLine, fZoomFactor);
				continue;
			}
			const char sigil = line[0];
			const std::string text = line.substr(1) + "\n";
			switch (sigil) {
				case '!':
					AppendWithColor(fOutput, text, kColorDiffHeader, fZoomFactor);
					break;
				case '+':
					AppendWithColor(fOutput, "+ " + text, kColorDiffAdd, fZoomFactor);
					break;
				case '-':
					AppendWithColor(fOutput, "- " + text, kColorDiffRemove, fZoomFactor);
					break;
				default: // ' ' context
					AppendWithColor(fOutput, "  " + text, kColorToolLine, fZoomFactor);
					break;
			}
		}
		AppendWithColor(fOutput, "\n", kColorToolLine, fZoomFactor);
		if (!fUserScrolled) _ScrollToBottom();
		break;
	}

	case gui::MSG_ASK_PERM:
		_HandlePermRequest(msg);
		break;

	case gui::MSG_ASK_CHOICE:
		_HandleChoiceRequest(msg);
		break;

	case gui::MSG_STATUS: {
		int32 kind = 0;
		msg->FindInt32("kind", &kind);
		switch (static_cast<sink::StatusKind>(kind)) {
			case sink::StatusKind::kThinking:
				SetTitle("Claude");
				break;
			case sink::StatusKind::kCallingTool:
				SetTitle("Claude");
				break;
			case sink::StatusKind::kIdle:
				_UpdateTitle();
				break;
		}
		break;
	}

	case gui::MSG_ERR: {
		_SpinnerStop();
		const char* text = nullptr;
		if (msg->FindString("text", &text) == B_OK && text) {
			AppendWithColor(fOutput,
			    std::string("\n\xE2\x9A\xA0 ") + text + "\n", // ⚠
			    kColorError, fZoomFactor);
			_ScrollToBottom();
		}
		break;
	}

	case gui::MSG_TOKENS: {
		int32 input  = 0;
		int32 output = 0;
		int32 maxCtx = 0;
		bool  ok     = true;
		msg->FindInt32("input",  &input);
		msg->FindInt32("output", &output);
		msg->FindInt32("max",    &maxCtx);
		msg->FindBool("ok",      &ok);
		// Remember whether the turn completed cleanly so MSG_WORKER_DONE
		// only commits the mutated history on success.
		fTurnCommitted = ok;
		fSessionInputTokens  += input;
		fSessionOutputTokens += output;
		if (fTokenBar) {
			fTokenBar->SetTokens(fSessionInputTokens, maxCtx > 0 ? maxCtx : fMaxTokens);
			fTokenBar->SetStats(fTurnCount, fSessionInputTokens, fSessionOutputTokens);
		}
		break;
	}

	case gui::MSG_WORKER_DONE: {
		fWorkerRunning.store(false);
		// The worker is no longer detached — join it so fSink is only
		// freed after the worker thread has fully exited (no UAF).
		if (fWorker.joinable()) fWorker.join();
		delete fSink;
		fSink = nullptr;

		// Stop spinner.
		delete fSpinnerTimer;
		fSpinnerTimer = nullptr;
		_SpinnerStop();

		// Flush any open code block.
		if (fInCodeBlock) { fInCodeBlock = false; _FlushCodeBlock(); }
		// Emit a final partial line (a response that didn't end in '\n')
		// before flushing the renderer so trailing text isn't dropped.
		if (!fLineBuffer.empty() && !fInCodeBlock) {
			if (fMdRenderer) fMdRenderer->Write(fLineBuffer);
			else             _AppendText(fLineBuffer);
		}
		fLineBuffer.clear();
		if (fMdRenderer)  fMdRenderer->Flush();
		fInWebFetch = false;
		fWebFetchBuf.clear();

		// Commit turn to history. On a clean completion adopt the full
		// messages array the worker built — it contains the user turn
		// plus any tool_use / tool_result blocks and the assistant's
		// final content, so tool context survives into the next turn.
		// On cancellation (fTurnCommitted == false) fall back to the
		// plain user/assistant text so a half-finished tool exchange
		// doesn't leave an orphaned tool_use that would 400 the API.
		{
			// Hold fMessagesMutex while swapping/appending so the remote-control
			// poller's SetSharedHistory snapshot never observes a half-updated
			// or moved-from array.
			std::lock_guard<std::mutex> lock(fMessagesMutex);
			if (fCompactPending) {
				// /compact: replace the underlying context with the summary
				// (the streamed assistant text), but keep the on-screen
				// transcript visible. On failure leave history untouched.
				if (fTurnCommitted && !fPendingAssistantText.empty()) {
					fMessages = nlohmann::json::array({
						{{"role", "user"},
						 {"content", "[previous conversation context follows]"}},
						{{"role", "assistant"}, {"content", fPendingAssistantText}},
					});
					_AppendToolLine("\xE2\x9C\x93 context compacted \xE2\x80\x94 "
						"conversation summarized; on-screen history kept\n");
				} else {
					_AppendToolLine("[compact failed \xE2\x80\x94 history unchanged]\n");
				}
				fCompactPending = false;
			} else if (fTurnCommitted && fWorkerMessages.is_array()
					&& !fWorkerMessages.empty()) {
				fMessages = std::move(fWorkerMessages);
			} else {
				if (!fPendingUserText.empty())
					fMessages.push_back({{"role", "user"},
					                     {"content", fPendingUserText}});
				if (!fPendingAssistantText.empty())
					fMessages.push_back({{"role", "assistant"},
					                     {"content", fPendingAssistantText}});
			}
		}
		fWorkerMessages = nlohmann::json::array();
		fPendingUserText.clear();
		fPendingAssistantText.clear();

		++fTurnCount;
		if (fTokenBar)
			fTokenBar->SetStats(fTurnCount, fSessionInputTokens, fSessionOutputTokens);
		_SetBusy(false);
		_UpdateTitle();

		// Streamed text is now inserted pre-scaled to the current zoom
		// (the renderer and AppendWithColor honour fZoomFactor), so no
		// retro-scaling is needed here.

		// Auto-save session to BFS after every completed turn.
		_SaveSession();

		// Keep the sidebar current if it's open (title/turn count may
		// have changed, or this may be a brand-new session file).
		if (fSessionPanel && !fSessionPanel->IsHidden())
			_RefreshSessionList();

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
				if (preview.size() > 120) {
					preview.resize(117);
					preview += "\xE2\x80\xA6"; // …
				}
				if (!preview.empty())
					notif.SetContent(preview.c_str());

				notif.Send();
			}
		}
		break;
	}

	case gui::MSG_ABOUT: {
		// Build a native BAboutWindow with the app icon loaded from the
		// running binary via BAppFileInfo — the same pattern used by
		// MountEncrypted and other polished Haiku apps.
		BAboutWindow* about = new BAboutWindow("Claude",
		    "application/x-vnd.Microgeni-claude-gui");

		// Pull the HVIF icon that was stamped onto the binary at link time.
		// Rasterise the raw vector data to 64x64 — the fixed-size GetIcon
		// overload cannot fill a 64x64 bitmap (see WelcomeView for details).
		app_info info;
		if (be_roster->GetRunningAppInfo(be_app->Team(), &info) == B_OK) {
			BFile appFile(&info.ref, B_READ_ONLY);
			BAppFileInfo fileInfo(&appFile);
			uint8* data = nullptr;
			size_t size = 0;
			if (fileInfo.GetIcon(&data, &size) == B_OK && data != nullptr) {
				BBitmap* icon = new BBitmap(BRect(0, 0, 63, 63), B_RGBA32);
				if (BIconUtils::GetVectorIcon(data, size, icon) == B_OK)
					about->SetIcon(icon);
				else
					delete icon;
				free(data);
			}
		}

		about->SetVersion(config::kVersion);
		about->AddDescription(
		    "A native Haiku GUI for the Anthropic Claude API.");

		const char* authors[] = {
			"Daniel Benjaminsson <info@microgeni.se>",
			nullptr
		};
		about->AddAuthors(authors);
		about->AddCopyright(2025, "Microgeni AB");

		about->Show();
		break;
	}

	case gui::MSG_HELP_DOCS: {
		// Open the project README on GitHub in the default browser.
		const std::string url = "https://github.com/microgeni/haiku-claude-cli";
		const std::string cmd = "open " + url + " &";
		std::system(cmd.c_str());  // flawfinder: ignore
		break;
	}

	case gui::MSG_DEMO_MARKDOWN:
		_ShowMarkdownDemo();
		break;

	case gui::MSG_LUDICROUS: {
		// Toggle ludicrous mode and update the menu checkmark to match.
		const bool nowOn = !api::g_ludicrous_mode.load();
		api::g_ludicrous_mode.store(nowOn);
		if (fLudicrousItem) fLudicrousItem->SetMarked(nowOn);
		if (fTokenBar) fTokenBar->SetLudicrous(nowOn);
		const std::string notice = nowOn
			? "\xE2\x9A\xA1 Ludicrous mode ON \xE2\x80\x94 all tool permissions auto-approved\n"
			: "\xE2\x9A\xA1 Ludicrous mode OFF \xE2\x80\x94 permission prompts restored\n";
		_AppendToolLine(notice);
		break;
	}

	case gui::MSG_REMOTE_CONTROL:
		_ToggleRemote();
		break;

	case gui::MSG_REMOTE_APPEND: {
		// A remote (Telegram) turn finished. Append the user + assistant
		// messages to the local history on the window thread so the GUI's
		// fMessages stays in sync and the exchange is saved, AND render the
		// exchange into the transcript so the chat window mirrors what the
		// phone saw. This is the Telegram -> GUI half of the bidirectional
		// mirror (the GUI -> Telegram half lives in _SpawnWorker).
		const char* userStr = nullptr;
		const char* asstStr = nullptr;
		if (msg->FindString("user", &userStr) == B_OK
		    && msg->FindString("assistant", &asstStr) == B_OK) {
			try {
				nlohmann::json userMsg = nlohmann::json::parse(userStr);
				nlohmann::json asstMsg = nlohmann::json::parse(asstStr);

				// Pull plain text out of each message for display. Content
				// may be a string or an array of typed blocks; we render the
				// text blocks and ignore images/tool_use for the transcript.
				const std::string userText = _PlainTextFromContent(userMsg.value("content", nlohmann::json{}));
				const std::string asstText = _PlainTextFromContent(asstMsg.value("content", nlohmann::json{}));

				{
					std::lock_guard<std::mutex> lock(fMessagesMutex);
					fMessages.push_back(std::move(userMsg));
					fMessages.push_back(std::move(asstMsg));
				}
				_SaveSession();

				// Render the exchange. A dim 📡 marker on the user label
				// signals the turn originated from Telegram, not the GUI.
				_DismissWelcome();
				AppendWithColor(fOutput, "\n\xF0\x9F\x93\xA1 you \xE2\x96\xB8 ", kColorUserLabel, fZoomFactor);
				_AppendText(userText + "\n");
				AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel, fZoomFactor);
				if (!asstText.empty()) _AppendText(asstText + "\n");
			} catch (const std::exception& e) {
				config::LogLine(std::string("remote append parse failed: ")
					+ e.what());
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
	// Persist window frame / zoom / model before tearing down.
	_SaveGuiPrefs();

	// Persist prompt history so future GUI sessions can recall these prompts.
	if (fInput) fInput->SaveHistory(paths::GuiHistoryPath());

	if (fWorkerRunning.load()) {
		// Ask the in-flight request to abort, then wait for the worker
		// thread to actually exit before tearing down the sink it holds.
		g_interrupted = 1;
		if (fWorker.joinable()) fWorker.join();
		fWorkerRunning.store(false);
		delete fSink;
		fSink = nullptr;
	}

	// Multi-window: only quit the application when THIS is the last chat
	// window. CountWindows() includes hidden helper windows (the settings
	// dialog), so count visible ChatWindows explicitly. The window itself
	// is still in the list at this point, so a count of 1 means "last".
	int chatWindows = 0;
	for (int32 i = 0; i < be_app->CountWindows(); ++i) {
		if (dynamic_cast<ChatWindow*>(be_app->WindowAt(i)) != nullptr)
			++chatWindows;
	}
	if (chatWindows <= 1)
		be_app->PostMessage(B_QUIT_REQUESTED);

	// Tear down the settings dialog (a separate BWindow we created and
	// keep alive hidden) so its looper thread exits cleanly.
	if (fSettings && fSettings->Lock())
		fSettings->Quit();
	fSettings = nullptr;

	return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

void ChatWindow::_AppendText(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorText, fZoomFactor);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_AppendToolLine(const std::string& text)
{
	AppendWithColor(fOutput, text, kColorToolLine, fZoomFactor);
	if (!fUserScrolled) _ScrollToBottom();
}

// ── Inline thinking spinner ────────────────────────────────────────────────
// Renders a CLI-style "⣾ Thinking… 3s" line as the last text in the chat
// output while waiting for the first token. _SpinnerTick rewrites it in
// place; _SpinnerStop erases it before the real reply (or tool line) lands.

static std::string SpinnerLineText(int step, int verb, bigtime_t start)
{
	const char* glyph = kGuiSpinnerGlyphs[step % kGuiGlyphCount];
	const char* v     = kGuiSpinnerVerbs[verb % kGuiVerbCount];
	const int total = static_cast<int>((system_time() - start) / 1000000LL);
	char elapsed[24];
	if (total < 60)
		std::snprintf(elapsed, sizeof(elapsed), "%ds", total);
	else
		std::snprintf(elapsed, sizeof(elapsed), "%dm %ds", total / 60, total % 60);
	return std::string(glyph) + " " + v + "\xE2\x80\xA6  " + elapsed;
}

void ChatWindow::_SpinnerStart()
{
	fSpinnerStep   = 0;
	fSpinnerVerb   = static_cast<int>((system_time() / 1000) % kGuiVerbCount);
	fSpinnerStart  = system_time();
	fSpinnerOffset = fOutput->TextLength();
	fSpinnerActive = true;
	AppendWithColor(fOutput, SpinnerLineText(fSpinnerStep, fSpinnerVerb,
	                                         fSpinnerStart), kColorInputCyan, fZoomFactor);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_SpinnerTick()
{
	if (!fSpinnerActive) return;
	fSpinnerStep++;
	// Replace everything from the spinner offset to the end with the new
	// frame. (Nothing else writes to fOutput while we're waiting, so the
	// spinner is always the trailing text.)
	const int32 end = fOutput->TextLength();
	if (fSpinnerOffset <= end)
		fOutput->Delete(fSpinnerOffset, end);
	AppendWithColor(fOutput, SpinnerLineText(fSpinnerStep, fSpinnerVerb,
	                                         fSpinnerStart), kColorInputCyan, fZoomFactor);
	if (!fUserScrolled) _ScrollToBottom();
}

void ChatWindow::_SpinnerStop()
{
	if (!fSpinnerActive) return;
	fSpinnerActive = false;
	const int32 end = fOutput->TextLength();
	if (fSpinnerOffset <= end)
		fOutput->Delete(fSpinnerOffset, end);
}

void ChatWindow::_ScrollToBottom()
{
	fOutput->ScrollToOffset(fOutput->TextLength());
}

void ChatWindow::_DismissWelcome()
{
	// Collapse the startup splash the first time the chat gets real
	// content. Safe to call repeatedly — Hide() on an already-hidden
	// view is a no-op.
	if (fWelcome != nullptr && !fWelcome->IsHidden())
		fWelcome->Hide();
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

	// A trailing partial line (no terminating '\n' yet) is intentionally
	// left in fLineBuffer: the next chunk appends to it and the fence /
	// markdown logic above runs once the line actually completes. The
	// renderer also buffers internally, so passing partial text here would
	// either render nothing (held until newline) or risk processing an
	// incomplete fence marker. The end-of-turn handler flushes any final
	// partial line via the explicit fLineBuffer write + fMdRenderer->Flush().
}

// ---------------------------------------------------------------------------
// _FlushCodeBlock — render accumulated code in a BScintillaView
// ---------------------------------------------------------------------------

// Map a syntax::TokenKind to an rgb_color and bold flag for BTextView.
// Colours are chosen to read well on the dark code-block background.
static void TokenStyle(syntax::TokenKind kind,
                       rgb_color& color, bool& bold)
{
	bold = false;
	switch (kind) {
		case syntax::TokenKind::Keyword:
			color = {220, 100, 220, 255}; bold = true;  break; // bold magenta
		case syntax::TokenKind::Type:
			color = { 80, 200, 220, 255}; bold = true;  break; // bold cyan
		case syntax::TokenKind::Preprocessor:
			color = {200, 100, 200, 255};                break; // magenta
		case syntax::TokenKind::String:
			color = {100, 200, 100, 255};                break; // green
		case syntax::TokenKind::Number:
			color = { 80, 200, 200, 255};                break; // cyan
		case syntax::TokenKind::Comment:
			color = {130, 130, 140, 255};                break; // dim grey
		case syntax::TokenKind::Operator:
			color = {210, 180,  80, 255};                break; // yellow
		case syntax::TokenKind::Constant:
			color = {220, 180,  60, 255}; bold = true;  break; // bold yellow
		case syntax::TokenKind::Builtin:
			color = { 80, 200,  80, 255}; bold = true;  break; // bold green
		case syntax::TokenKind::Variable:
			color = { 80, 200, 200, 255};                break; // cyan
		case syntax::TokenKind::Special:
			color = {220,  80,  80, 255}; bold = true;  break; // bold red
		default: // Plain
			color = {200, 200, 160, 255};                break; // warm cream
	}
}

void ChatWindow::_FlushCodeBlock()
{
	if (fCodeBuffer.empty()) return;

	// Render the code block as styled text in the BTextView.
	// Embedding BScintillaView as a child of BTextView is unreliable —
	// Scintilla's internal state isn't ready until the view is fully
	// attached to the screen, causing GPF crashes on SendMessage().
	// Styled BTextView runs are simpler and crash-free.

	if (fMdRenderer) {
		// ── Language label (italic, dim) ─────────────────────────────────
		if (!fCodeLang.empty()) {
			md::Run langRun;
			langRun.text     = fCodeLang + "\n";
			langRun.hasColor = true;
			langRun.color    = {120, 120, 140, 255};
			langRun.italic   = true;
			langRun.monospace = true;
			fMdRenderer->AppendRun(langRun);
		}

		// ── Syntax-highlighted body ───────────────────────────────────────
		// Split the accumulated buffer into lines and tokenise each one.
		// Each token span becomes its own md::Run so BTextView renders it
		// in the correct colour.  Adjacent spans of the same kind are
		// already merged by syntax::Tokenise's push() helper, so the
		// run count is minimal.
		const std::string& buf = fCodeBuffer;
		size_t lineStart = 0;
		while (lineStart <= buf.size()) {
			const size_t nl = buf.find('\n', lineStart);
			const bool   atEnd = (nl == std::string::npos);
			const std::string line = atEnd
			    ? buf.substr(lineStart)
			    : buf.substr(lineStart, nl - lineStart + 1); // include '\n'

			if (!line.empty()) {
				// Tokenise without the trailing newline, then re-attach it.
				const std::string bare = (line.back() == '\n')
				    ? line.substr(0, line.size() - 1)
				    : line;

				const auto spans = syntax::Tokenise(fCodeLang, bare);
				for (const auto& span : spans) {
					md::Run r;
					r.text      = span.text;
					r.monospace = true;
					r.hasColor  = true;
					TokenStyle(span.kind, r.color, r.bold);
					fMdRenderer->AppendRun(r);
				}
				// Re-emit the newline as a plain monospace run so the
				// colour doesn't bleed into the next line's background.
				if (line.back() == '\n') {
					md::Run nlRun;
					nlRun.text      = "\n";
					nlRun.monospace = true;
					nlRun.hasColor  = true;
					nlRun.color     = {200, 200, 160, 255};
					fMdRenderer->AppendRun(nlRun);
				}
			}

			if (atEnd) break;
			lineStart = nl + 1;
		}

		// Trailing blank line to separate the block from the next paragraph.
		md::Run sep;
		sep.text = "\n";
		fMdRenderer->AppendRun(sep);
		fMdRenderer->ScrollToEnd();
	} else {
		_AppendText(fCodeBuffer);
	}

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
	std::string userText(raw);

	// The GUI has no slash commands — everything the CLI exposes via
	// /commands is available through the menus here. Any text starting with
	// '/' is sent verbatim as a normal turn.

	fInput->SetText("");
	if (userText == std::string(raw)) fInput->PushHistory(userText);

	// If this is the first turn, set the conversation topic for the title.
	if (fConvTopic.empty()) {
		fConvTopic = userText.size() > 60
		    ? userText.substr(0, 57) + "\xE2\x80\xA6"
		    : userText;
	}

	// Emit user label + text into the output.
	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel, fZoomFactor);   // ▸
	_AppendText(userText + "\n");
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel, fZoomFactor);

	// Sending is an explicit "show me this exchange" action: snap to the
	// bottom and hide the jump button so streaming follows the reply.
	_ScrollToBottom();
	if (fJumpBtn && !fJumpBtn->IsHidden()) fJumpBtn->Hide();

	_LaunchWorker(userText);
}

void ChatWindow::_LaunchWorker(const std::string& userText)
{
	_DismissWelcome();

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
	_SpinnerStart();
	BMessage tickMsg(gui::MSG_TICK);
	fSpinnerTimer = new BMessageRunner(BMessenger(this), &tickMsg, 80000LL);

	// Seed the worker-owned messages array from the canonical history and
	// append this turn's user message. The worker mutates fWorkerMessages
	// in place; the main thread adopts it after join() in MSG_WORKER_DONE.
	fWorkerMessages = fMessages;
	if (fPendingImages.empty()) {
		// Plain text turn — content is a simple string.
		fWorkerMessages.push_back({{"role", "user"}, {"content", userText}});
	} else {
		// Multimodal turn — content is an array of blocks: the text first,
		// then one `image` block per dropped image (base64 source).
		nlohmann::json content = nlohmann::json::array();
		if (!userText.empty())
			content.push_back({{"type", "text"}, {"text", userText}});
		for (const auto& [mediaType, b64] : fPendingImages) {
			content.push_back({
				{"type", "image"},
				{"source", {
					{"type",       "base64"},
					{"media_type", mediaType},
					{"data",       b64},
				}},
			});
		}
		fWorkerMessages.push_back({{"role", "user"}, {"content", content}});
		fPendingImages.clear();
	}
	fTurnCommitted  = false;
	fCompactPending = false;

	_SpawnWorker();
}

// Run /compact: ask Claude to (1) persist file summaries via WriteAttr and
// (2) summarize the conversation, then replace the underlying context with
// the summary while leaving the on-screen transcript intact. Mirrors the
// CLI's /compact (commands.cpp), reusing the worker + GuiSink so the
// summary streams into the chat like a normal reply.
void ChatWindow::_LaunchCompact()
{
	if (fWorkerRunning.load()) return;          // a turn is already running
	if (fMessages.empty()) {
		_AppendToolLine("[nothing to compact]\n");
		return;
	}

	_DismissWelcome();

	// Visible markers in the scrollback so the user sees what happened.
	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 /compact\n", kColorUserLabel);
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel, fZoomFactor);

	fPendingUserText.clear();
	fPendingAssistantText.clear();
	fInCodeBlock = false;
	fCodeBuffer.clear();
	fLineBuffer.clear();
	fInWebFetch = false;
	fWebFetchBuf.clear();

	_SetBusy(true);
	fTurnStartTime = system_time();
	fToolsUsed     = 0;

	_SpinnerStart();
	BMessage tickMsg(gui::MSG_TICK);
	fSpinnerTimer = new BMessageRunner(BMessenger(this), &tickMsg, 80000LL);

	// Build the request: current history + the two-task compact prompt
	// (same wording as the CLI so behavior matches).
	fWorkerMessages = fMessages;
	fWorkerMessages.push_back({
		{"role", "user"},
		{"content",
			"Two tasks, in order:\n"
			"\n"
			"1. For each source file you've gained real understanding of "
			"during this conversation, call WriteAttr to persist a concise "
			"one-line claude:summary capturing what the file is for. Only "
			"write for files you could confidently describe \xE2\x80\x94 skip "
			"files only mentioned in passing. WriteAttr is auto-approved and "
			"restricted to the claude:* namespace, so these writes are cheap "
			"and safe. This lets future sessions start with accurate "
			"summaries instead of mechanical placeholders.\n"
			"\n"
			"2. Then summarize the preceding conversation in 2-3 short "
			"paragraphs, preserving important context, decisions, code, and "
			"open questions. Reply with only the summary after the WriteAttr "
			"calls."},
	});
	fTurnCommitted  = false;
	fCompactPending = true;

	_SpawnWorker();
}

// Shared worker spawn: assumes fWorkerMessages is prepared. Streams the
// reply through a fresh GuiSink and posts MSG_TOKENS / MSG_WORKER_DONE.
void ChatWindow::_SpawnWorker()
{
	const config::Auth auth         = fAuth;
	const std::string  model        = fModel;
	const int          maxTokens    = fMaxTokens;
	const std::string  systemPrompt = config::ComposeSystem(fSystemPrompt,
	                                                         fWorkingDir);

	fSink = new gui::GuiSink(BMessenger(this));
	gui::GuiSink* sink = fSink;
	sink->BeginMessage("assistant");

	// When remote control is active, mirror this locally-typed turn to the
	// phone: stream to a TelegramSink for the primary chat in addition to
	// the GUI. Capture the prompt text now (on the window thread) so the
	// worker can send a "> prompt" preamble before the reply streams.
	telegram::RemoteControl* remote =
		(fRemote && fRemote->Running()) ? fRemote.get() : nullptr;
	const std::string promptText = fPendingUserText;

	fWorkerRunning.store(true);
	// fWorkerMessages is mutated through `this`; the main thread does not
	// touch it until after join() in MSG_WORKER_DONE, so no data race.
	fWorker = std::thread([this, auth, model, maxTokens, systemPrompt, sink,
	                       remote, promptText]() {
		// Build the active sink. With no remote, it's just the GuiSink.
		// With remote active, tee the GuiSink and a TelegramSink so the
		// turn appears on both surfaces. AcquireTurn() serialises against
		// the remote poll loop so a phone turn and a GUI turn never run
		// concurrently against the shared history or the same chat.
		std::unique_ptr<telegram::TelegramSink> tgSink;
		std::unique_ptr<TeeSink>                tee;
		OutputSink* active = sink;

		if (remote && remote->PrimaryUserId() != 0) {
			remote->AcquireTurn();
			remote->SendPromptNotice(promptText);
			tgSink = std::make_unique<telegram::TelegramSink>(
				remote->GetClient(),
				remote->PrimaryUserId(),
				remote->AllowDestructive(),
				remote->AllowedToolsRef(),
				remote->PermQueueRef());
			tgSink->BeginMessage("assistant");
			tee = std::make_unique<TeeSink>(sink, tgSink.get());
			active = tee.get();
		}

		const api::SendResult result = api::SendWithTools(auth, model, maxTokens,
		                   fWorkerMessages, systemPrompt, active);
		sink->EndMessage();

		if (remote) {
			if (tgSink) tgSink->EndMessage();
			remote->ReleaseTurn();
		}

		BMessage tokMsg(gui::MSG_TOKENS);
		tokMsg.AddInt32("input",  result.input_tokens);
		tokMsg.AddInt32("output", result.output_tokens);
		tokMsg.AddInt32("max",    maxTokens);
		tokMsg.AddBool("ok", result.exit_code == 0);
		BMessenger(this).SendMessage(&tokMsg);
		BMessenger(this).SendMessage(gui::MSG_WORKER_DONE);
	});
	// NOTE: do NOT detach — joined in MSG_WORKER_DONE / QuitRequested so
	// fSink is never freed while the worker still holds it.
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
	{
		std::lock_guard<std::mutex> lock(fMessagesMutex);
		fMessages    = nlohmann::json::array();
	}
	fTurnCount   = 0;
	fConvTopic.clear();
	fSessionPath.clear();
	fPendingImages.clear();
	fSessionInputTokens  = 0;
	fSessionOutputTokens = 0;
	if (fTokenBar) {
		fTokenBar->SetTokens(0, fMaxTokens);
		fTokenBar->SetStats(0, 0, 0);
	}
	_UpdateTitle();

	// Always restore the input to a ready state — _CancelWorker() only
	// requests interruption and leaves _SetBusy(false) to MSG_WORKER_DONE,
	// but if no worker was running the input would stay disabled/hidden.
	_SetBusy(false);
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
	// The buffer is empty; keep the user's chosen factor so new text is
	// inserted pre-scaled to it. fAppliedZoom tracks the buffer's uniform
	// zoom (nothing on screen, so it equals the current factor).
	fAppliedZoom = fZoomFactor;
	if (fMdRenderer) fMdRenderer->SetZoom(fZoomFactor);

	// The output is empty again — bring back the welcome splash with the
	// app icon and greeting, just as on a fresh window.
	if (fWelcome != nullptr && fWelcome->IsHidden())
		fWelcome->Show();
}

void ChatWindow::_HandlePermRequest(BMessage* msg)
{
	const char* tool    = nullptr;
	const char* preview = nullptr;
	msg->FindString("tool",    &tool);
	msg->FindString("preview", &preview);

	const std::string toolName = tool ? tool : "(unknown)";

	std::string body = "Allow tool: ";
	body += toolName;
	if (preview && preview[0]) {
		body += "\n\n";
		const std::string pv(preview);
		body += (pv.size() > 400) ? pv.substr(0, 400) + "\xE2\x80\xA6" : pv;
	}

	// Three-button alert:
	//   button 0 (B_ESCAPE shortcut) — Deny
	//   button 1                     — Allow once
	//   button 2                     — Always allow this tool (session)
	//
	// "Always allow" adds the tool name to api::AlwaysAllowed(), the
	// same session-scoped allowlist the CLI uses. Subsequent calls to
	// the same tool short-circuit in GuiSink::AskPermission and never
	// reach this dialog again. (The blanket "⚡ Allow All" / ludicrous
	// mode lives in the Tools menu for users who want it.)
	//
	// The worker thread is parked on fPermSem while this runs, so
	// mutating api::AlwaysAllowed() here is race-free.
	std::string alwaysLabel = "Always allow " + toolName;
	BAlert* alert = new BAlert("Tool Permission", body.c_str(),
	    "Deny", "Allow Once", alwaysLabel.c_str(),
	    B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	const int32 choice = alert->Go();

	if (choice == 2) {
		// Add to the session allowlist so this tool is never prompted
		// again for the rest of the session.
		api::AlwaysAllowed().insert(toolName);
		_AppendToolLine("\xE2\x9C\x93 " + toolName
			+ " allowed for this session\n");
		if (fSink) fSink->DeliverPermissionReply(true);
	} else {
		// choice 1 = allow once; choice 0 / Esc = deny.
		if (fSink) fSink->DeliverPermissionReply(choice == 1);
	}
}

void ChatWindow::_HandleChoiceRequest(BMessage* msg)
{
	const char* prompt = nullptr;
	msg->FindString("prompt", &prompt);

	std::vector<std::string> options;
	const char* opt = nullptr;
	for (int32 i = 0; msg->FindString("options", i, &opt) == B_OK; ++i)
		options.emplace_back(opt ? opt : "");

	if (options.empty()) {
		if (fSink) fSink->DeliverChoiceReply(-1);
		return;
	}

	const std::string promptStr = prompt && prompt[0] ? prompt : "Choose an option";

	int chosen = -1;
	if (options.size() <= 3) {
		// Native BAlert handles up to three buttons. Button order matches
		// option order (button 0 = option 0). Esc cancels to -1.
		BAlert* alert = new BAlert("Choose", promptStr.c_str(),
		    options.size() > 0 ? options[0].c_str() : nullptr,
		    options.size() > 1 ? options[1].c_str() : nullptr,
		    options.size() > 2 ? options[2].c_str() : nullptr,
		    B_WIDTH_AS_USUAL, B_OFFSET_SPACING, B_INFO_ALERT);
		alert->SetShortcut(options.size() - 1, B_ESCAPE);
		chosen = static_cast<int>(alert->Go());
	} else {
		// More than three options — use the scrolling button modal.
		ChoiceModal* modal = new ChoiceModal(promptStr, options);
		chosen = modal->Go();   // deletes itself via Quit()
	}

	if (fSink) fSink->DeliverChoiceReply(chosen);
}

// ---------------------------------------------------------------------------
// Toolbar / UI state
// ---------------------------------------------------------------------------

void ChatWindow::_SetBusy(bool busy)
{
	if (busy) {
		fSend->Hide();
		fStop->Show();
	} else {
		fStop->Hide();
		fSend->Show();
		fInput->MakeFocus(true);
	}
	// The input stays enabled, dark, and cyan throughout — busy state no
	// longer disables or recolours it (type-ahead friendly; matches the
	// CLI which keeps the prompt live). Progress is shown by the in-chat
	// spinner instead.
}

void ChatWindow::_UpdateTitle()
{
	SetTitle("Claude");
}

// Look up the active model's per-million-token pricing and hand it to
// the token bar so it can show a running cost estimate. Uses the
// built-in fallback price table (config "prices" overrides aren't
// plumbed into the GUI yet).
void ChatWindow::_UpdateTokenBarPrice()
{
	if (!fTokenBar) return;
	const models::PriceEntry price =
		models::GetPrice(fModel, nlohmann::json());
	fTokenBar->SetPrice(price.input, price.output);
}

// Extract displayable plain text from a message "content" field. A bare
// string is returned as-is; an array of blocks yields the concatenated
// "text" blocks (image and tool_use blocks are skipped).
std::string ChatWindow::_PlainTextFromContent(const nlohmann::json& content)
{
	if (content.is_string())
		return content.get<std::string>();
	if (!content.is_array())
		return std::string();

	std::string out;
	for (const auto& block : content) {
		if (!block.is_object()) continue;
		if (block.value("type", std::string()) == "text") {
			if (!out.empty()) out += "\n";
			out += block.value("text", std::string());
		}
	}
	return out;
}

// ── Multi-session registration (Phase 2a) ───────────────────────────────────

// A short label for /sessions: the conversation topic when set, otherwise
// a stable "GUI session N" using the registry ID.
std::string ChatWindow::_SessionLabel() const
{
	if (!fConvTopic.empty()) return fConvTopic;
	return "GUI session " + std::to_string(fRegistrySessionId);
}

// Register this window as a live remote-control session so the phone can
// list it via /sessions and route prompts to it via /session N. Each
// window supplies its own history provider and appender; a prompt routed
// here runs against THIS window's conversation and its reply is mirrored
// into THIS window's transcript via MSG_REMOTE_APPEND. Idempotent.
void ChatWindow::_RegisterSession()
{
	if (fRegistrySessionId != 0) return;

	BMessenger self(this);

	auto provider = [this]() -> nlohmann::json {
		std::lock_guard<std::mutex> lock(fMessagesMutex);
		return fMessages;
	};

	auto appender = [self](nlohmann::json userMsg, nlohmann::json asstMsg) {
		BMessage msg(gui::MSG_REMOTE_APPEND);
		msg.AddString("user", userMsg.dump().c_str());
		msg.AddString("assistant", asstMsg.dump().c_str());
		self.SendMessage(&msg);
	};

	// No live sink factory in 2a: the appender renders the finished turn
	// into this window via the proven MSG_REMOTE_APPEND path. Live
	// token-by-token cross-window streaming is a later refinement.
	fRegistrySessionId = telegram::SessionRegistry::Instance().Register(
		nullptr, _SessionLabel(), fModel,
		std::move(provider), std::move(appender), nullptr);

	config::LogLine("gui session registered id="
		+ std::to_string(fRegistrySessionId));
}

// Remove this window from the registry so no routed prompt reaches a
// window that is tearing down. Called from the destructor.
void ChatWindow::_UnregisterSession()
{
	if (fRegistrySessionId == 0) return;
	telegram::SessionRegistry::Instance().Unregister(fRegistrySessionId);
	config::LogLine("gui session unregistered id="
		+ std::to_string(fRegistrySessionId));
	fRegistrySessionId = 0;
}

// Tools > Remote Control: start or stop the background Telegram poller.
// Mirrors the CLI's /remote-control toggle. Keeps the menu checkmark and
// the token-bar "📡 REMOTE" badge in sync with the actual running state.
void ChatWindow::_ToggleRemote()
{
	const bool running = fRemote && fRemote->Running();

	if (running) {
		// Stop and tear down the poller.
		fRemote->Stop();
		fRemote.reset();
		if (fRemoteItem) fRemoteItem->SetMarked(false);
		if (fTokenBar)   fTokenBar->SetRemote(false);
		_AppendToolLine("\xF0\x9F\x93\xA1 Remote control OFF "
			"\xE2\x80\x94 Telegram poller stopped\n");
		return;
	}

	// Starting: validate the telegram config first so we can give a clear
	// reason rather than silently doing nothing.
	const config::Config cfg = config::Load();
	std::string why;
	if (!telegram::RemoteControl::ConfigIsValid(cfg, &why)) {
		_AppendToolLine("\xF0\x9F\x93\xA1 Remote control: " + why + "\n"
			"  Add a 'telegram' block to config.json with a bot_token and "
			"allowed_user_ids. See README.md.\n");
		if (fRemoteItem) fRemoteItem->SetMarked(false);
		return;
	}

	try {
		// The auth getter hands the bridge a fresh copy of our session auth
		// before each remote turn, so it piggybacks on the same credentials.
		fRemote = std::make_unique<telegram::RemoteControl>(
			cfg,
			[this] { return fAuth; },
			fSystemPrompt);

		// Each ChatWindow registers itself as a session, so the poller must
		// NOT register a duplicate session of its own. It is purely the
		// transport here; routing is handled by the per-window registry
		// entries created in _RegisterSession().
		fRemote->SetSelfRegister(false);

		// Preflight before starting threads: catch an invalid token or a
		// 409 Conflict (another instance polling the same bot) up front, so
		// we report a clear reason instead of a badge that lies.
		std::string preflightWhy;
		if (!fRemote->Preflight(&preflightWhy)) {
			fRemote.reset();
			if (fRemoteItem) fRemoteItem->SetMarked(false);
			if (fTokenBar)   fTokenBar->SetRemote(false);
			_AppendToolLine("\xF0\x9F\x93\xA1 Remote control: "
				+ preflightWhy + "\n");
			return;
		}

		if (fRemote->Start()) {
			// Give the registry a meaningful label so /sessions on the
			// phone shows the conversation topic rather than a bare ID.
			fRemote->SetSessionTitle(
				fConvTopic.empty() ? std::string("GUI session") : fConvTopic);
			// Share the GUI's conversation so remote turns see both sides of
			// the dialogue (mirrors the CLI). The provider snapshots fMessages
			// under fMessagesMutex; the appender posts a BMessage so the actual
			// history mutation happens on the window thread (BLooper-safe).
			fRemote->SetSharedHistory([this]() -> nlohmann::json {
				std::lock_guard<std::mutex> lock(fMessagesMutex);
				return fMessages;
			});
			BMessenger self(this);
			fRemote->SetSharedHistoryAppender(
				[self](nlohmann::json userMsg, nlohmann::json asstMsg) {
					BMessage msg(gui::MSG_REMOTE_APPEND);
					msg.AddString("user", userMsg.dump().c_str());
					msg.AddString("assistant", asstMsg.dump().c_str());
					self.SendMessage(&msg);
				});
			if (fRemoteItem) fRemoteItem->SetMarked(true);
			if (fTokenBar)   fTokenBar->SetRemote(true);
			_AppendToolLine("\xF0\x9F\x93\xA1 Remote control ON "
				"\xE2\x80\x94 Telegram poller started\n");
		} else {
			fRemote.reset();
			if (fRemoteItem) fRemoteItem->SetMarked(false);
			_AppendToolLine("\xF0\x9F\x93\xA1 Remote control: "
				"failed to start (already running?)\n");
		}
	} catch (const std::exception& e) {
		fRemote.reset();
		if (fRemoteItem) fRemoteItem->SetMarked(false);
		_AppendToolLine(std::string("\xF0\x9F\x93\xA1 Remote control error: ")
			+ e.what() + "\n");
	}
}

void ChatWindow::_SaveSession()
{
	if (fMessages.empty()) return;
	session::SessionSettings settings;
	settings.model        = fModel;
	settings.systemPrompt = fSystemPrompt;
	settings.workingDir   = fWorkingDir;
	settings.maxTokens    = fMaxTokens;
	const std::string saved = session::Save(
	    fSessionPath,
	    fConvTopic.empty() ? "Untitled" : fConvTopic,
	    fModel,
	    fTurnCount,
	    fMessages,
	    settings);
	if (!saved.empty())
		fSessionPath = saved;
}

// Serialize the conversation to a Markdown file. Handles plain-string
// content, array content (text + image placeholders), and the
// tool_use / tool_result blocks that live in the history after a
// tool-using turn. Best-effort: unknown block shapes are skipped.
void ChatWindow::_ExportTranscript(const std::string& path)
{
	std::string out;
	out += "# Claude transcript\n\n";
	if (!fConvTopic.empty()) out += "**Topic:** " + fConvTopic + "\n\n";
	out += "**Model:** " + fModel + "  \n";
	out += "**Turns:** " + std::to_string(fTurnCount) + "\n\n---\n\n";

	// Render one content value (string or block array) to Markdown.
	auto renderContent = [](const nlohmann::json& content) -> std::string {
		std::string s;
		if (content.is_string()) {
			s = content.get<std::string>();
		} else if (content.is_array()) {
			for (const auto& block : content) {
				const std::string type = block.value("type", "");
				if (type == "text") {
					s += block.value("text", "");
				} else if (type == "image") {
					const std::string mt =
						block.contains("source")
							? block["source"].value("media_type", "image")
							: "image";
					s += "_[image attachment: " + mt + "]_";
				} else if (type == "tool_use") {
					s += "\n> 🔧 **tool call:** `" + block.value("name", "?")
					   + "`\n";
				} else if (type == "tool_result") {
					std::string rc;
					const auto& c = block.contains("content")
						? block["content"] : nlohmann::json();
					if (c.is_string()) rc = c.get<std::string>();
					else if (c.is_array()) {
						for (const auto& cb : c)
							if (cb.value("type", "") == "text")
								rc += cb.value("text", "");
					}
					if (rc.size() > 1000) { rc.resize(1000); rc += "\n…[truncated]"; }
					s += "\n> 🔧 **tool result:**\n```\n" + rc + "\n```\n";
				}
			}
		}
		return s;
	};

	for (const auto& turn : fMessages) {
		const std::string role = turn.value("role", "");
		if (!turn.contains("content")) continue;
		const nlohmann::json& content = turn["content"];

		// A user turn whose content is purely tool_result blocks is the
		// automated half of a tool round-trip, not something the human
		// typed — label it as such so the transcript reads correctly.
		bool toolResultOnly = false;
		if (role == "user" && content.is_array() && !content.empty()) {
			toolResultOnly = true;
			for (const auto& b : content)
				if (b.value("type", "") != "tool_result") { toolResultOnly = false; break; }
		}

		const std::string body = renderContent(content);
		if (body.empty()) continue;
		if (toolResultOnly)
			out += "## Tool result\n\n" + body + "\n\n";
		else if (role == "user")
			out += "## You\n\n" + body + "\n\n";
		else if (role == "assistant")
			out += "## Claude\n\n" + body + "\n\n";
		else
			out += "## " + role + "\n\n" + body + "\n\n";
	}

	BFile file(path.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK) {
		BAlert* alert = new BAlert("Export failed",
		    "Could not write the transcript to that location.",
		    "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->Go();
		return;
	}
	file.Write(out.data(), out.size());

	// Stamp a text MIME type so Tracker opens it sensibly.
	BNodeInfo nodeInfo(&file);
	nodeInfo.SetType("text/markdown");

	_AppendToolLine("\xE2\x9C\x93 transcript exported to " + path + "\n");
}

// ---------------------------------------------------------------------------
// Find in conversation (Cmd-F)
// ---------------------------------------------------------------------------

void ChatWindow::_ToggleFindBar()
{
	if (!fFindBar) return;
	if (fFindBar->IsHidden()) {
		// The find bar lives inside the input pane; if that pane is collapsed
		// (View > Input), revealing the find bar alone would draw nothing.
		// Show the pane first so the find bar is actually visible.
		if (fInputPane && fInputPane->IsHidden()) {
			fInputPane->Show();
			if (fInputItem) fInputItem->SetMarked(true);
			if (fVSplit) {
				fVSplit->InvalidateLayout(true);
				fVSplit->Relayout();
			}
		}
		fFindBar->Show();
		if (fFindField) {
			// Pre-fill with the current selection, if any, then focus.
			int32 selStart = 0, selEnd = 0;
			fOutput->GetSelection(&selStart, &selEnd);
			if (selEnd > selStart && (selEnd - selStart) < 128) {
				std::string sel(fOutput->Text() + selStart, selEnd - selStart);
				fFindField->SetText(sel.c_str());
			}
			fFindField->MakeFocus(true);
			if (BTextView* tv = fFindField->TextView())
				tv->SelectAll();
		}
		fFindMatchStart = -1;
	} else {
		fFindBar->Hide();
		fInput->MakeFocus(true);
	}
}

// Select and scroll to the next (forward) or previous match of the find
// field's text, searching case-insensitively over the chat output. Wraps
// around the ends. Updates the "n / total" counter.
void ChatWindow::_FindNext(bool forward)
{
	if (!fFindField || !fOutput) return;
	const char* raw = fFindField->Text();
	const std::string needle = raw ? raw : "";
	if (needle.empty()) {
		if (fFindStatus) fFindStatus->SetText("");
		return;
	}

	// Case-insensitive haystack/needle copies.
	auto lower = [](std::string s) {
		for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
		return s;
	};
	const std::string hay = lower(fOutput->Text());
	const std::string ndl = lower(needle);

	// Count all matches and find the current/next one in the chosen
	// direction relative to fFindMatchStart.
	std::vector<size_t> matches;
	for (size_t p = hay.find(ndl); p != std::string::npos;
	     p = hay.find(ndl, p + 1))
		matches.push_back(p);

	if (matches.empty()) {
		if (fFindStatus) fFindStatus->SetText("not found");
		fFindMatchStart = -1;
		return;
	}

	// Pick the target index relative to the current match (wraps).
	int target = 0;
	if (fFindMatchStart < 0) {
		target = forward ? 0 : static_cast<int>(matches.size()) - 1;
	} else {
		// Locate the current match's index.
		int cur = 0;
		for (size_t i = 0; i < matches.size(); ++i)
			if (static_cast<int32>(matches[i]) == fFindMatchStart) { cur = static_cast<int>(i); break; }
		const int n = static_cast<int>(matches.size());
		target = forward ? (cur + 1) % n : (cur - 1 + n) % n;
	}

	const size_t start = matches[target];
	const int32  s = static_cast<int32>(start);
	const int32  e = static_cast<int32>(start + ndl.size());
	fFindMatchStart = s;

	fOutput->Select(s, e);
	fOutput->ScrollToSelection();

	if (fFindStatus) {
		const std::string label = std::to_string(target + 1) + " / "
		                        + std::to_string(matches.size());
		fFindStatus->SetText(label.c_str());
	}
}

// ---------------------------------------------------------------------------
// Font zoom (Cmd +/-/0)
// ---------------------------------------------------------------------------

// delta: +1 = zoom in, -1 = zoom out, 0 = reset to 100%.
void ChatWindow::_Zoom(int delta)
{
	// 5% per step gives fine-grained zoom control; the range spans 60%–250%.
	constexpr float kStep = 0.05f;
	constexpr float kMin  = 0.6f;
	constexpr float kMax  = 2.5f;

	if (delta == 0) {
		fZoomFactor = 1.0f;
	} else {
		fZoomFactor += (delta > 0 ? kStep : -kStep);
		if (fZoomFactor < kMin) fZoomFactor = kMin;
		if (fZoomFactor > kMax) fZoomFactor = kMax;
	}
	// Rescale text already on screen to the new factor, and make the
	// renderer emit future streamed runs pre-scaled so new replies match.
	_ApplyZoom();
	if (fMdRenderer) fMdRenderer->SetZoom(fZoomFactor);
}

// Rescale all on-screen text from the previously-applied zoom
// (fAppliedZoom) to the current one (fZoomFactor). Because new text is now
// inserted pre-scaled to fZoomFactor by the renderer and AppendWithColor,
// the whole buffer is always at a single uniform zoom, so one incremental
// rescale by (fZoomFactor / fAppliedZoom) covers everything. Relative sizes
// the markdown renderer chose (headings vs body) are preserved because each
// run is scaled multiplicatively. Called only from _Zoom().
void ChatWindow::_ApplyZoom()
{
	if (!fOutput) return;
	const int32 len = fOutput->TextLength();
	if (len <= 0) { fAppliedZoom = fZoomFactor; return; }

	// Incremental ratio from the last applied zoom to the new one.
	const float ratio = (fAppliedZoom > 0.0f) ? (fZoomFactor / fAppliedZoom)
	                                           : fZoomFactor;
	if (ratio != 1.0f) {
		text_run_array* runs = fOutput->RunArray(0, len);
		if (runs) {
			for (int32 i = 0; i < runs->count; ++i)
				runs->runs[i].font.SetSize(runs->runs[i].font.Size() * ratio);
			fOutput->SetRunArray(0, len, runs);
			free(runs);
		}
	}

	fAppliedZoom = fZoomFactor;
	fOutput->Invalidate();
}

// ---------------------------------------------------------------------------
// Session sidebar
// ---------------------------------------------------------------------------

void ChatWindow::_ToggleSessionList()
{
	if (!fSessionPanel) return;
	if (fSessionPanel->IsHidden()) {
		_RefreshSessionList();
		fSessionPanel->Show();
	} else {
		fSessionPanel->Hide();
	}
}

// Repopulate the list from the BFS session store, newest first. Marks the
// row matching the currently-open session (fSessionPath) as selected.
void ChatWindow::_RefreshSessionList()
{
	if (!fSessionList) return;

	// Clear existing items.
	for (int32 i = fSessionList->CountItems() - 1; i >= 0; --i)
		delete fSessionList->RemoveItem(i);

	const std::vector<session::SessionInfo> sessions = session::List();
	int32 selectIdx = -1;
	for (size_t i = 0; i < sessions.size(); ++i) {
		const session::SessionInfo& s = sessions[i];
		std::string label = s.title.empty() ? "(untitled)" : s.title;
		if (s.turns > 0) label += "  (" + std::to_string(s.turns) + ")";
		fSessionList->AddItem(new SessionItem(label, s.path,
			s.title.empty() ? "" : s.title));
		if (!fSessionPath.empty() && s.path == fSessionPath)
			selectIdx = static_cast<int32>(i);
	}
	if (selectIdx >= 0) fSessionList->Select(selectIdx);
}

void ChatWindow::_LoadSelectedSession()
{
	if (!fSessionList) return;
	// Loading is a single-session action. If several rows are selected
	// (multi-select for bulk delete), don't guess — require exactly one.
	if (fSessionList->CountItems() == 0) return;
	int32 selCount = 0;
	int32 only     = -1;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i) {
		if (fSessionList->IsItemSelected(i)) { ++selCount; only = i; }
	}
	if (selCount != 1) return;
	SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(only));
	if (!item) return;
	// _LoadSession replays the conversation and sets fSessionPath.
	_LoadSession(item->Path());
}

void ChatWindow::_DeleteSelectedSession()
{
	if (!fSessionList) return;

	// Collect the paths of every selected row (multi-select).
	std::vector<std::string> paths;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i) {
		if (!fSessionList->IsItemSelected(i)) continue;
		SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(i));
		if (item) paths.push_back(item->Path());
	}
	if (paths.empty()) return;

	// Confirm, with the count when more than one is selected.
	std::string msg = (paths.size() == 1)
		? "Delete this saved session? This cannot be undone."
		: "Delete " + std::to_string(paths.size())
		  + " saved sessions? This cannot be undone.";
	BAlert* confirm = new BAlert("Delete sessions", msg.c_str(),
	    "Cancel", "Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	confirm->SetShortcut(0, B_ESCAPE);
	if (confirm->Go() != 1) return;   // 0 = Cancel

	for (const std::string& path : paths) {
		if (session::Delete(path)) {
			// If we deleted the open session, detach so the next save
			// makes a fresh file rather than recreating the deleted one.
			if (fSessionPath == path) fSessionPath.clear();
		}
	}
	_RefreshSessionList();
}

void ChatWindow::_RenameSelectedSession()
{
	if (!fSessionList) return;
	// Rename is a single-row action — require exactly one selection.
	int32 selCount = 0, only = -1;
	for (int32 i = 0; i < fSessionList->CountItems(); ++i)
		if (fSessionList->IsItemSelected(i)) { ++selCount; only = i; }
	if (selCount != 1) return;

	SessionItem* item = dynamic_cast<SessionItem*>(fSessionList->ItemAt(only));
	if (!item) return;

	const std::string current = item->Title();
	RenameModal* modal = new RenameModal(current);
	const std::string entered = modal->Go();   // self-quits

	// Trim whitespace; empty / unchanged → no-op.
	std::string name = entered;
	while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
		name.erase(name.begin());
	while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
		name.pop_back();
	if (name.empty() || name == current) return;

	if (session::Rename(item->Path(), name)) {
		// If we renamed the currently-open session, keep fConvTopic in
		// sync so the next auto-save doesn't overwrite the new title.
		if (item->Path() == fSessionPath) fConvTopic = name;
		_RefreshSessionList();
	}
}

// ---------------------------------------------------------------------------
// Global GUI preferences — a flattened BMessage at paths::GuiPrefsPath().
// ---------------------------------------------------------------------------

void ChatWindow::_LoadGuiPrefs()
{
	BFile file(paths::GuiPrefsPath().c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) return;
	BMessage prefs;
	if (prefs.Unflatten(&file) != B_OK) return;

	// Window frame — clamp into the current screen so a prefs file from
	// a larger display doesn't park the window off-screen.
	BRect frame;
	if (prefs.FindRect("frame", &frame) == B_OK && frame.IsValid()
			&& frame.Width() > 200 && frame.Height() > 150) {
		MoveTo(frame.LeftTop());
		// Clamp to the window minimum so a small saved frame can't start
		// the window below the floor that keeps the input bar visible.
		float rw = frame.Width();
		float rh = frame.Height();
		if (rw < fWindowMinW) rw = fWindowMinW;
		if (rh < fWindowMinH) rh = fWindowMinH;
		ResizeTo(rw, rh);
	}

	float zoom = 1.0f;
	if (prefs.FindFloat("zoom", &zoom) == B_OK
			&& zoom >= 0.6f && zoom <= 2.5f) {
		fZoomFactor  = zoom;
		fAppliedZoom = zoom;   // empty buffer; new text arrives pre-scaled
		if (fMdRenderer) fMdRenderer->SetZoom(zoom);
	}

	// Restore the splitter weight of the sidebar relative to the chat.
	if (fSplit) {
		float sidebarWeight = 0.0f;
		if (prefs.FindFloat("sidebar_weight", &sidebarWeight) == B_OK
				&& sidebarWeight > 0.0f && sidebarWeight < 5.0f)
			fSplit->SetItemWeight((int32)0, sidebarWeight, false);
		fSplit->SetItemWeight((int32)1, 1.0f, true);
	}

	// Restore the splitter weight of the chat relative to the input pane.
	if (fVSplit) {
		float inputWeight = 0.0f;
		if (prefs.FindFloat("input_weight", &inputWeight) == B_OK
				&& inputWeight > 0.0f && inputWeight < 5.0f)
			fVSplit->SetItemWeight((int32)1, inputWeight, false);
		fVSplit->SetItemWeight((int32)0, 1.0f, true);
	}

	// Last-used model — only adopt the auto-saved GUI model when the user
	// hasn't explicitly pinned one in config.json / via a CLI flag. If the
	// constructor model differs from the built-in default, that value was
	// deliberately chosen and must win over the saved last-used model.
	// A loaded session still overrides this later.
	const bool userPinnedModel =
		!fConfigModel.empty() && fConfigModel != config::kDefaultModel;
	const char* model = nullptr;
	if (!userPinnedModel
			&& prefs.FindString("model", &model) == B_OK && model && model[0]) {
		fModel = model;
		_UpdateTitle();
		if (fModelMenu) {
			// Menu lives in the settings dialog looper — lock it.
			const bool dlgLocked = fSettings && fSettings->Lock();
			for (int32 i = 0; i < fModelMenu->CountItems(); ++i) {
				BMenuItem* it = fModelMenu->ItemAt(i);
				if (it) it->SetMarked(it->Label() && fModel == it->Label());
			}
			if (dlgLocked) fSettings->Unlock();
		}
	}
}

void ChatWindow::_SaveGuiPrefs()
{
	paths::MkdirP(paths::ConfigDir());
	BMessage prefs('GPRF');
	prefs.AddRect("frame", Frame());
	prefs.AddFloat("zoom", fZoomFactor);
	prefs.AddString("model", fModel.c_str());
	// Splitter weight (relative width of the sidebar).
	if (fSplit) {
		prefs.AddFloat("sidebar_weight",  fSplit->ItemWeight((int32)0));
	}
	// Splitter weight (relative height of the input pane).
	if (fVSplit) {
		prefs.AddFloat("input_weight",  fVSplit->ItemWeight((int32)1));
	}

	BFile file(paths::GuiPrefsPath().c_str(),
	           B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		prefs.Flatten(&file);
}

void ChatWindow::_LoadSession(const std::string& path)
{
	_DismissWelcome();
	if (path.empty()) return;
	nlohmann::json loaded = session::Load(path);
	if (loaded.empty()) return;

	// Cancel any running worker first.
	_CancelWorker();
	_ClearOutput();

	{
		std::lock_guard<std::mutex> lock(fMessagesMutex);
		fMessages    = loaded;
	}
	fSessionPath = path;
	fTurnCount   = 0;
	fConvTopic.clear();

	// Restore the per-session settings (model, system prompt, working
	// dir, max-tokens) so continuing the conversation uses the same
	// context it was created with. Absent fields (older files) keep the
	// window's current values.
	session::SessionSettings st;
	if (session::LoadSettings(path, st)) {
		if (!st.model.empty())        fModel        = st.model;
		if (!st.systemPrompt.empty()) fSystemPrompt = st.systemPrompt;
		if (!st.workingDir.empty())   fWorkingDir   = st.workingDir;
		if (st.maxTokens > 0)         fMaxTokens    = st.maxTokens;

		// Reflect the restored values in the UI.
		_UpdateTitle();
		if (fSettings && fSettings->Lock()) {
			fSettings->SetValues(fSystemPrompt, fMaxTokens,
			                     fNotifyMinSec, fWorkingDir);
			// Mark the matching model menu item (menu is in this looper).
			if (fModelMenu) {
				for (int32 i = 0; i < fModelMenu->CountItems(); ++i) {
					BMenuItem* it = fModelMenu->ItemAt(i);
					if (it) it->SetMarked(it->Label() && fModel == it->Label());
				}
			}
			fSettings->Unlock();
		}
		// Refresh the cost estimate for the restored model.
		_UpdateTokenBarPrice();
	}

	// Replay the conversation into the output view so the user can see it.
	for (const auto& turn : fMessages) {
		if (!turn.is_object()) continue;
		const std::string role = turn.value("role", "");

		// Sessions that used tools store "content" as an array of
		// content blocks (text / tool_use / tool_result), not a plain
		// string. Flatten it to displayable text; calling
		// turn.value("content", "") on an array throws type_error.302
		// and aborts the whole app.
		std::string content;
		if (turn.contains("content")) {
			const nlohmann::json& c = turn["content"];
			if (c.is_string()) {
				content = c.get<std::string>();
			} else if (c.is_array()) {
				for (const auto& block : c) {
					if (!block.is_object()) continue;
					const std::string type = block.value("type", "");
					if (type == "text") {
						content += block.value("text", "");
					} else if (type == "tool_use") {
						content += "\n> \xF0\x9F\x94\xA7 tool call: "
						         + block.value("name", "?") + "\n";
					} else if (type == "tool_result") {
						const auto& rc = block.contains("content")
							? block["content"] : nlohmann::json();
						if (rc.is_string())
							content += rc.get<std::string>();
						else if (rc.is_array())
							for (const auto& cb : rc)
								if (cb.is_object()
								    && cb.value("type", "") == "text")
									content += cb.value("text", "");
					}
				}
			}
		}
		if (content.empty()) continue;

		if (role == "user") {
			if (fConvTopic.empty()) {
				fConvTopic = content.size() > 60
				    ? content.substr(0, 57) + "\xE2\x80\xA6"
				    : content;
			}
			AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel, fZoomFactor);
			_AppendText(content + "\n");
			++fTurnCount;
		} else if (role == "assistant") {
			AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel, fZoomFactor);
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

	// Apply the current zoom to the replayed transcript.
	if (fZoomFactor != 1.0f) _ApplyZoom();

	// Loaded sessions don't carry token totals; reset and show the
	// replayed turn count so the bar stays consistent with the CLI.
	fSessionInputTokens  = 0;
	fSessionOutputTokens = 0;
	if (fTokenBar) {
		fTokenBar->SetTokens(0, fMaxTokens);
		fTokenBar->SetStats(fTurnCount, 0, 0);
	}
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

void ChatWindow::_ShowMarkdownDemo()
{
	_DismissWelcome();

	// Emit a user "question" label so the demo looks like a real turn.
	AppendWithColor(fOutput, "\nyou \xE2\x96\xB8 ", kColorUserLabel, fZoomFactor);
	_AppendText("Show me a markdown rendering demo\n");
	AppendWithColor(fOutput, "claude \xE2\x96\xB8 \n", kColorModelLabel, fZoomFactor);

	// ── Rich markdown sample ─────────────────────────────────────────────────
	// Every feature supported by md::MdRenderer is exercised here so the
	// developer can visually verify the renderer at a glance.
	static const char kDemo[] =
		// H1 heading
		"# Markdown Rendering Demo\n"
		"\n"
		"Welcome to the **Claude GUI** markdown renderer. "
		"This demo exercises every feature supported by `md::MdRenderer`.\n"
		"\n"
		// H2 heading
		"## Inline Formatting\n"
		"\n"
		"You can write **bold text**, *italic text*, or `inline code` "
		"within a paragraph. Links look like this: "
		"[Haiku OS](https://www.haiku-os.org). "
		"Combinations like **bold and *nested italic*** fall back gracefully.\n"
		"\n"
		// H3 heading
		"### Emphasis Variants\n"
		"\n"
		"- __double-underscore bold__\n"
		"- _single-underscore italic_\n"
		"- `backtick code` in a list item\n"
		"- Plain text for comparison\n"
		"\n"
		// Horizontal rule
		"---\n"
		"\n"
		// H2
		"## Lists\n"
		"\n"
		"**Unordered** (hyphen markers):\n"
		"\n"
		"- Haiku R1 beta 4\n"
		"- Haiku R1 beta 5\n"
		"- Haiku R1 (upcoming stable release)\n"
		"\n"
		"**Ordered** list:\n"
		"\n"
		"1. Clone the repository\n"
		"2. Run `make` to build\n"
		"3. Run `make test` to verify\n"
		"4. Enjoy native Claude on Haiku!\n"
		"\n"
		"**Nested** unordered list (two-space indent):\n"
		"\n"
		"- Widgets\n"
		"  - BTextView\n"
		"  - BButton\n"
		"  - BScrollView\n"
		"- Layout\n"
		"  - BGroupLayout\n"
		"  - BLayoutBuilder\n"
		"\n"
		// Horizontal rule
		"---\n"
		"\n"
		// H2
		"## Blockquote\n"
		"\n"
		"> *\"The best way to predict the future is to invent it.\"*\n"
		"> \xE2\x80\x94 Alan Kay\n"  // — Alan Kay
		"\n"
		// H2
		"## Headings at Every Level\n"
		"\n"
		"# H1 \xE2\x80\x94 golden, large\n"    // em dash
		"## H2 \xE2\x80\x94 sky-blue, medium\n"
		"### H3 \xE2\x80\x94 soft-green, slight\n"
		"\n"
		// H2
		"## Table\n"
		"\n"
		"| Language   | Paradigm     | Year |\n"
		"| ---------- | ------------ | ---- |\n"
		"| C++        | Multi-paradigm | 1985 |\n"
		"| Python     | Imperative   | 1991 |\n"
		"| Rust       | Systems      | 2010 |\n"
		"| Haskell    | Functional   | 1990 |\n"
		"\n"
		// H2
		"## Horizontal Rules\n"
		"\n"
		"Three variants all render as the same box-drawing separator:\n"
		"\n"
		"---\n"
		"\n"
		"***\n"
		"\n"
		"___\n"
		"\n"
		// H2
		"## Code Block (via ChatWindow fence handler)\n"
		"\n"
		"Fenced code blocks (` ``` `) are handled upstream by `_ProcessChunk` "
		"and `_FlushCodeBlock`, then rendered as monospace styled runs:\n"
		"\n"
		"```cpp\n"
		"// Say hello from Haiku!\n"
		"#include <stdio.h>\n"
		"int main() {\n"
		"    printf(\"Hello from Haiku!\\n\");\n"
		"    return 0;\n"
		"}\n"
		"```\n"
		"\n"
		// Closing paragraph
		"---\n"
		"\n"
		"That covers **all** features. "
		"Use **Help \xE2\x86\x92 Show Markdown Demo** any time to re-run this. "  // →
		"Happy hacking on Haiku! \xF0\x9F\x90\xBE\n";  // 🐾

	if (fMdRenderer) {
		fMdRenderer->Write(kDemo);
		fMdRenderer->Flush();
	} else {
		_AppendText(kDemo);
	}
	_ScrollToBottom();
}

void ChatWindow::_InsertFileContent(const std::string& path)
{
	// ── Image files become base64 `image` content blocks ─────────────────────
	// Anthropic supports JPEG / PNG / GIF / WebP up to ~5 MB each. We read
	// the raw bytes, base64-encode them, and stash them in fPendingImages so
	// _LaunchWorker can emit them as image blocks alongside the text. A chip
	// is inserted into the input so the user sees the attachment.
	if (const std::string mediaType = ImageMediaType(path); !mediaType.empty()) {
		constexpr off_t kMaxImageBytes = 5 * 1024 * 1024;
		BFile imgFile(path.c_str(), B_READ_ONLY);
		if (imgFile.InitCheck() != B_OK) return;
		off_t imgSize = 0;
		imgFile.GetSize(&imgSize);
		if (imgSize <= 0) return;
		if (imgSize > kMaxImageBytes) {
			BAlert* alert = new BAlert("Image too large",
			    "That image exceeds the 5 MB limit and cannot be attached.",
			    "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->Go();
			return;
		}
		std::string raw(static_cast<size_t>(imgSize), '\0');
		if (imgFile.Read(&raw[0], raw.size()) <= 0) return;

		fPendingImages.emplace_back(mediaType, Base64Encode(raw));

		// Insert a visible chip and focus the input.
		std::string name = path;
		if (const auto slash = name.rfind('/'); slash != std::string::npos)
			name = name.substr(slash + 1);
		const std::string chip = "\n[\xF0\x9F\x96\xBC image: " + name + "]\n";
		const int32 end = fInput->TextLength();
		fInput->Insert(end, chip.c_str(), static_cast<int32>(chip.size()));
		fInput->Select(fInput->TextLength(), fInput->TextLength());
		fInput->MakeFocus(true);
		return;
	}

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
