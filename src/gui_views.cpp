#include "gui_views.h"

#include <cstdlib>
#include <fstream>
#include <string>

#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <ControlLook.h>
#include <File.h>
#include <IconUtils.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <Roster.h>
#include <ScrollView.h>
#include <Window.h>

#include "gui_colors.h"
#include "gui_messages.h"
#include "gui_scale.h"
#include "gui_sink.h"    // gui::MSG_SEND
#include "models.h"

// The definitions below were lifted verbatim from chat_window.cpp. They use
// the unqualified ScalePx()/gui_scale palette names; a using-declaration
// brings the scale helpers into scope without editing every call site.
using gui::ScalePx;

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
	if (fPlan) {
		const std::string badge = "\xF0\x9F\x93\x8B PLAN"; // 📋
		SetHighColor(80, 200, 80, 255); // green — read-only research
		MovePenTo(statsLeft, baseline);
		DrawString(badge.c_str());
		statsLeft += f.StringWidth(badge.c_str()) + f.StringWidth(sep.c_str());
		SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	}
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

void TokenBar::SetPlan(bool on)
{
	if (fPlan == on) return;
	fPlan = on;
	Invalidate();
}

void TokenBar::SetRemote(bool on)
{
	if (fRemote == on) return;
	fRemote = on;
	Invalidate();
}


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
