#ifndef HAIKU_CLAUDE_CLI_GUI_VIEWS_H
#define HAIKU_CLAUDE_CLI_GUI_VIEWS_H

#include <string>
#include <vector>

#include <Bitmap.h>
#include <LayoutUtils.h>
#include <TextView.h>
#include <View.h>

// gui_views — the custom BView / BTextView subclasses used to build the chat
// window's layout. Extracted from chat_window so the window's implementation
// file holds just ChatWindow itself.
//
// Every class here is decoupled from ChatWindow: they communicate with the
// owning window only through the shared gui::MSG_* codes (posted to
// Window()), never by holding a ChatWindow pointer. Colours come from
// gui_colors.h and pixel scaling from gui_scale.h.

// ─────────────────────────────────────────────────────────────────────────────
// InputView — multi-line BTextView that sends on Enter (Shift+Enter inserts a
// newline). Up/Down arrow keys navigate prompt history. Escape forwards
// MSG_CANCEL to the window. It is sized by its enclosing InputContainer (see
// below), the Genio TerminalTab way, so it carries no layout-size overrides.
class InputView : public BTextView {
public:
	explicit InputView(const char* name);

	void	AttachedToWindow() override;
	void	Draw(BRect updateRect) override;
	void	KeyDown(const char* bytes, int32 numBytes) override;
	void	FrameResized(float w, float h) override;
	void	MakeFocus(bool focused) override;
	void	MouseMoved(BPoint where, uint32 transit,
	                   const BMessage* drag) override;
	// Drop is handled via the window's MessageReceived(B_SIMPLE_DATA).
	// InputView::MessageDropped forwards the message there.

	// Push an entry onto the history ring.
	void	PushHistory(const std::string& text);

	// Load / save history from a file (one entry per line).
	void	LoadHistory(const std::string& path);
	void	SaveHistory(const std::string& path) const;

	// Clear the in-memory prompt history ring. Number of stored entries.
	void	ClearHistory();
	size_t	HistoryCount() const { return fHistory.size(); }

	// Enable / disable editing (analogous to BControl::SetEnabled).
	void	SetEnabled(bool enabled);
	bool	IsEnabled() const { return fEnabled; }

private:
	void	_HistoryUp();
	void	_HistoryDown();
	void	_DrawPlaceholder();

	std::vector<std::string> fHistory;       // ring of past prompts
	int                      fHistIdx   = -1;
	std::string              fDraft;
	bool                     fEnabled    = true;
	bool                     fFocused    = false;
	bool                     fDropTarget = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// InputContainer — thin BView wrapper that claims layout space for the input
// and manually resizes its child scroll view to fill it. This is exactly how
// Genio's TerminalTab hosts its terminal/console view: a plain B_FOLLOW_ALL
// BView (no size overrides, so the layout's default unlimited max lets it
// stretch) that AddChild()s the real content and ResizeTo()s it on every
// frame change. It frees the input from any content-driven size gymnastics.
class InputContainer : public BView {
public:
	explicit InputContainer(const char* name);

	// Adopt the scroll view (added as a plain child, not via a layout) and
	// give it explicit min/preferred so it has a sensible starting size; the
	// container's FrameResized then keeps it filling the container.
	void	SetContent(BView* content, float minHeight);

	void	FrameResized(float w, float h) override;
	void	AttachedToWindow() override;

private:
	BView*	fContent   = nullptr;  // the BScrollView (not owned beyond AddChild)
	float	fMinHeight = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// ChatTextView — the scrolling chat transcript. A bare BTextView reports
// HasHeightForWidth() == true and feeds its current (frame-derived) text
// width up the layout tree; the window's BGroupLayout latches that as its
// minimum width, which freezes the window content at the current width while
// the frame shrinks under it — clipping the bottom input bar's right-edge
// buttons off-screen. Disabling height-for-width and pinning preferred width
// to the (small) minimum keeps the window freely shrinkable; the enclosing
// BScrollView handles overflow.
class ChatTextView : public BTextView {
public:
	ChatTextView(BRect frame, const char* name, BRect textRect,
			uint32 resizeMode, uint32 flags)
		: BTextView(frame, name, textRect, resizeMode, flags) {}

	// Never let text content drive the width axis of the layout.
	bool HasHeightForWidth() override { return false; }

	BSize MinSize() override
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(), BSize(80, 40));
	}

	BSize MaxSize() override
	{
		// Unlimited max lets the view fill (and shrink with) the frame
		// instead of being treated as a fixed-width preferred block.
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	}

	// Do NOT return the content-derived text-rect width as preferred; that
	// is the other path by which the current width gets latched as the
	// window's minimum.
	BSize PreferredSize() override { return MinSize(); }

	void FrameResized(float w, float h) override
	{
		BTextView::FrameResized(w, h);
		// Keep word-wrap tracking the (shrinking) frame width without
		// reporting that width back to the layout.
		SetTextRect(Bounds().InsetByCopy(4.0f, 4.0f));
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// TokenBar — thin view below the output area showing token usage as a
// coloured fill bar and a compact "used / max" label.
class TokenBar : public BView {
public:
	static const int kBarHeight = 22;

	TokenBar();

	void	Draw(BRect updateRect) override;
	void	SetTokens(int used, int maxCtx);

	// Per-session counters mirrored from the CLI status row:
	// turn number plus cumulative upstream (↑) / downstream (↓) tokens.
	void	SetStats(int turn, int sessionInput, int sessionOutput);

	// Per-million-token pricing for the active model, so the bar can
	// show a running cost estimate (mirrors the CLI's /cost).
	void	SetPrice(double inputPerM, double outputPerM);

	// Ludicrous mode indicator: when on, the bar draws a yellow
	// "⚡ LUDICROUS" badge so the auto-approve state is always visible.
	void	SetLudicrous(bool on);

	// Plan mode indicator: when on, the bar draws a green "📋 PLAN"
	// badge so the read-only research state is always visible.
	void	SetPlan(bool on);

	// Remote control indicator: when on, the bar draws a cyan
	// "📡 REMOTE" badge so the active Telegram bridge is always visible.
	void	SetRemote(bool on);

private:
	int    fUsed    = 0;
	int    fMax     = 200000; // default until first real value arrives
	int    fTurn    = 0;
	int    fInput   = 0;
	int    fOutput  = 0;
	double fPriceIn  = 0.0;   // $ per 1M input tokens
	double fPriceOut = 0.0;   // $ per 1M output tokens
	bool   fLudicrous = false; // Tools > Ludicrous Mode state
	bool   fRemote    = false; // Tools > Remote Control state
	bool   fPlan      = false; // Tools > Plan Mode state
};

// ─────────────────────────────────────────────────────────────────────────────
// WelcomeView — a splash panel shown above the chat output on a fresh window.
//
// Draws the application's HVIF icon (loaded from the running binary via
// BAppFileInfo, the same source as the About box) alongside a title and a
// short hint line — the GUI counterpart to the CLI's ASCII-art banner.
// Collapses itself once the first turn begins so it never crowds the chat.
class WelcomeView : public BView {
public:
	WelcomeView();
	~WelcomeView() override;

	void Draw(BRect updateRect) override;

private:
	BBitmap* fIcon = nullptr;  // owned; 64x64 RGBA app icon, may be nullptr
};

#endif // HAIKU_CLAUDE_CLI_GUI_VIEWS_H
