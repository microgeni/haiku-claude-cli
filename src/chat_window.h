#ifndef HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
#define HAIKU_CLAUDE_CLI_CHAT_WINDOW_H

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Button.h>
#include <Bitmap.h>
#include <GroupView.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <Layout.h>
#include <LayoutUtils.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <nlohmann/json.hpp>

#include "code_styler.h"
#include "config.h"
#include "gui_sink.h"
#include "md_renderer.h"
#include "tool_bar.h"

class BFilePanel;
class BSplitView;

namespace telegram { class RemoteControl; }

// Additional MSG_ codes beyond those in gui_sink.h.
namespace gui {
constexpr uint32_t MSG_NEW_CHAT     = 'NCVT'; // clear conversation + display
constexpr uint32_t MSG_CLEAR_OUTPUT = 'CLRO'; // clear display only (keep ctx)
constexpr uint32_t MSG_CANCEL       = 'CNCL'; // cancel in-flight turn
constexpr uint32_t MSG_MODEL_PICK   = 'MPCK'; // model menu item selected
constexpr uint32_t MSG_SETTINGS     = 'STNG'; // toggle settings panel
constexpr uint32_t MSG_TOKENS       = 'TOKN'; // int32 "input","output","max"
constexpr uint32_t MSG_JUMP_BOTTOM  = 'JBOT'; // jump-to-bottom button
constexpr uint32_t MSG_TICK         = 'TICK'; // 80-ms spinner tick
// MSG_SESSIONS / MSG_SESSION_LOAD reserved for future project.
constexpr uint32_t MSG_MODELS_READY = 'MDLS'; // background model fetch complete
constexpr uint32_t MSG_ABOUT        = 'ABUT'; // Help > About Claude
constexpr uint32_t MSG_HELP_DOCS    = 'HDOC'; // Help > Documentation
constexpr uint32_t MSG_DEMO_MARKDOWN = 'DMMD'; // Help > Show Markdown Demo
constexpr uint32_t MSG_LUDICROUS     = 'LUDC'; // Tools > Ludicrous Mode toggle
constexpr uint32_t MSG_REMOTE_CONTROL = 'RMTC'; // Tools > Remote Control toggle
constexpr uint32_t MSG_REMOTE_APPEND  = 'RMAP'; // remote turn finished: append to history
constexpr uint32_t MSG_BROWSE_WORKDIR = 'BRWD'; // Settings: browse for working dir
constexpr uint32_t MSG_EXPORT         = 'EXPT'; // File > Export Transcript…
constexpr uint32_t MSG_EXPORT_SAVE    = 'EXPS'; // B_SAVE_REQUESTED from export panel
constexpr uint32_t MSG_FIND           = 'FIND'; // Cmd-F: toggle the find bar
constexpr uint32_t MSG_FIND_NEXT      = 'FNXT'; // find bar: next match / Enter
constexpr uint32_t MSG_FIND_PREV      = 'FPRV'; // find bar: previous match
constexpr uint32_t MSG_FIND_CLOSE     = 'FCLO'; // find bar: close / Esc
constexpr uint32_t MSG_FIND_LIVE      = 'FLIV'; // find bar: query text changed
constexpr uint32_t MSG_ZOOM_IN        = 'ZMIN'; // Cmd-+ : larger chat font
constexpr uint32_t MSG_ZOOM_OUT       = 'ZMOT'; // Cmd-- : smaller chat font
constexpr uint32_t MSG_ZOOM_RESET     = 'ZMRS'; // Cmd-0 : reset chat font
constexpr uint32_t MSG_TOGGLE_SESSIONS = 'TSES'; // View: toggle session sidebar
constexpr uint32_t MSG_TOGGLE_INPUT    = 'TINP'; // View: toggle the input bar
constexpr uint32_t MSG_SESSION_SELECT  = 'SSEL'; // sidebar: load selected session
constexpr uint32_t MSG_SESSION_DELETE  = 'SDEL'; // sidebar: delete selected session
constexpr uint32_t MSG_SESSION_NEW     = 'SNEW'; // sidebar: start a new chat
constexpr uint32_t MSG_SESSION_RENAME  = 'SRNM'; // sidebar: rename selected session
constexpr uint32_t MSG_NEW_WINDOW      = 'NWIN'; // File > New Session: spawn a window
constexpr uint32_t MSG_COMPACT         = 'CMPT'; // Edit: compact conversation context
} // namespace gui

// ─────────────────────────────────────────────────────────────────────────────
// InputView — multi-line BTextView that sends on Enter (Shift+Enter inserts a
// newline). Up/Down arrow keys navigate prompt history. Escape forwards
// MSG_CANCEL to the window. It is sized by its enclosing InputContainer (see
// below), the Genio TerminalTab way, so it carries no layout-size overrides.
// ─────────────────────────────────────────────────────────────────────────────
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
// ─────────────────────────────────────────────────────────────────────────────
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
// ─────────────────────────────────────────────────────────────────────────────
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
// ─────────────────────────────────────────────────────────────────────────────
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
};

// ─────────────────────────────────────────────────────────────────────────────
// SettingsDialog — a free-floating dialog window for system prompt / model
// config. Contains the model picker, system-prompt editor, max-tokens field,
// a notification-delay slider, the working-directory field, and a Close
// button. Created once (hidden) and shown/hidden on demand; Browse and Close
// actions are forwarded to the parent ChatWindow so the existing handlers
// stay shared.
// ─────────────────────────────────────────────────────────────────────────────
class SettingsDialog : public BWindow {
public:
	SettingsDialog(BWindow* parent, const std::string& systemPrompt,
	               int maxTokens, int notifyMinSec,
	               const std::string& workingDir = {},
	               BMenuField* modelField = nullptr);

	// Populate fields from current config.
	void	SetValues(const std::string& systemPrompt, int maxTokens,
	                  int notifyMinSec, const std::string& workingDir = {});

	// Read back edited values.
	std::string	SystemPrompt() const;
	int         MaxTokens() const;
	bool        NotificationsEnabled() const;
	int         NotifyMinSeconds() const;
	std::string WorkingDir() const;

	bool	IsOpen() const { return fOpen; }
	void	Toggle();

	// Closing the dialog (X button) defers to the parent's MSG_SETTINGS
	// handler so edited values get read back, then hides instead of quitting.
	bool	QuitRequested() override;

private:
	void	_BuildLayout(const std::string& systemPrompt, int maxTokens,
	                     int notifyMinSec, const std::string& workingDir,
	                     BMenuField* modelField);

	BWindow*      fParent         = nullptr;
	BTextView*    fSysPromptView  = nullptr;
	BTextControl* fMaxTokensCtl   = nullptr;
	BSlider*      fNotifyDelay    = nullptr;
	BTextControl* fWorkingDirCtl  = nullptr;
	bool          fOpen           = false;
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

// ─────────────────────────────────────────────────────────────────────────────
// ChatWindow — the main application window.
//
// Layout (simplified):
//
//   ┌──────────────────────────────────┐
//   │  [output BTextView + scrollbar]  │
//   │                                  │
//   ├──────────────────────────────────┤
//   │  [token bar 14px]                │
//   ├──────────────────────────────────┤
//   │  [spinner] [input BTextView]  [Send  ]
//   │                               [Clear ]
//   └──────────────────────────────────┘
//   (Settings opens as a separate dialog window via File ▸ Settings.
//    The model picker lives inside that dialog.)
//
// Worker thread and message protocol:
//   Worker calls api::SendWithTools with a GuiSink. The sink marshals
//   all events to the main thread via BMessenger::SendMessage (non-blocking).
//   AskPermission is the sole blocker (semaphore). MessageReceived on the
//   main thread mutates all views — the only thread-safe approach.
// ─────────────────────────────────────────────────────────────────────────────
class ChatWindow : public BWindow {
public:
	ChatWindow(const config::Auth& auth, const std::string& model,
	           int maxTokens, const std::string& systemPrompt,
	           int notifyMinSec = 5,
	           const std::string& workingDir = {});
	~ChatWindow() override;

	void MessageReceived(BMessage* msg) override;
	void _RefsReceived(BMessage* msg); // drag-drop / file open handler
	bool QuitRequested() override;
	void FrameResized(float w, float h) override;
	// Re-assert the window's size limits after the base class has shown the
	// window (and the layout has run its first pass). A window-level BLayout
	// otherwise clobbers the limits we set in _BuildLayout with the layout's
	// own computed MinSize, which is too wide and blocks shrinking.
	void Show() override;

private:
	// ── Layout ──────────────────────────────────────────────────────────────
	void _BuildLayout();
	void _BuildMenuBar();       // native BMenuBar (File / Edit / Help)
	void _PopulateModelMenu();
	void _RepositionOverlays();  // floating jump-to-bottom button

	// ── Output helpers ───────────────────────────────────────────────────────
	void _AppendText(const std::string& text);
	void _AppendToolLine(const std::string& text);
	void _ProcessChunk(const std::string& chunk);
	void _FlushCodeBlock();
	void _ScrollToBottom();
	bool _IsNearBottom() const;

	// ── Turn lifecycle ───────────────────────────────────────────────────────
	void _SendTurn();
	void _LaunchWorker(const std::string& userText);
	void _LaunchCompact();       // summarize + replace context, keep scrollback
	void _SpawnWorker();         // shared: start spinner + worker thread on fWorkerMessages
	void _HandlePermRequest(BMessage* msg);
	void _HandleChoiceRequest(BMessage* msg);
	void _CancelWorker();
	void _NewChat();
	void _ClearOutput();

	// ── Inline thinking spinner (rendered in the chat transcript) ─────────────
	void _SpinnerStart();        // append the spinner line after "claude ▸"
	void _SpinnerTick();         // rewrite the spinner line in place (per tick)
	void _SpinnerStop();         // erase the spinner line (first token / done)

	// ── Toolbar / UI state ───────────────────────────────────────────────────
	void _SetBusy(bool busy);    // swap Send↔Stop, enable/disable input
	void _UpdateTitle();         // set window title from model + state
	void _UpdateTokenBarPrice(); // push the active model's pricing to the bar
	void _SaveSession();         // persist current conversation to BFS
	void _LoadSession(const std::string& path); // restore a saved session
	void _InsertFileContent(const std::string& path); // drag-drop helper
	void _ShowMarkdownDemo();    // render a rich markdown example into the chat output
	void _DismissWelcome();      // hide the startup splash once a turn begins
	void _ExportTranscript(const std::string& path); // write fMessages as Markdown

	// ── Find in conversation (Cmd-F) ──────────────────────────────────────────
	void _ToggleFindBar();              // show/hide + focus the find field
	void _FindNext(bool forward);       // select+scroll to next/prev match (wraps)

	// ── Font zoom (Cmd +/-/0) ──────────────────────────────────────────────────
	void _Zoom(int delta);              // delta: +1 in, -1 out, 0 reset
	void _ApplyZoom();                  // rescale all output runs to fZoomFactor

	// ── Session sidebar ────────────────────────────────────────────────────────
	void _ToggleSessionList();          // show/hide the left session panel
	void _RefreshSessionList();         // repopulate from session::List()
	void _LoadSelectedSession();        // load the highlighted session
	void _DeleteSelectedSession();      // delete the highlighted session (confirm)
	void _RenameSelectedSession();      // rename the highlighted session (prompt)

	// ── Global GUI preferences (persist across launches) ───────────────────────
	void _LoadGuiPrefs();               // restore window frame / zoom / model
	void _SaveGuiPrefs();               // write current frame / zoom / model

	// ── Widgets ─────────────────────────────────────────────────────────────
	BMenuBar*      fMenuBar       = nullptr;  // native top menu bar
	BMenuItem*     fLudicrousItem = nullptr;  // Tools > Ludicrous Mode (checkmark)
	BMenuItem*     fRemoteItem    = nullptr;  // Tools > Remote Control (checkmark)
	BMenuItem*     fInputItem     = nullptr;  // View > Input (checkmark)
	BTextView*     fOutput        = nullptr;
	WelcomeView*   fWelcome       = nullptr;  // startup splash, collapsed on first turn
	BScrollView*   fScroll        = nullptr;
	BButton*       fJumpBtn       = nullptr;  // floating "↓" overlay button
	TokenBar*      fTokenBar      = nullptr;
	InputView*     fInput         = nullptr;
	BScrollView*   fInputScroll   = nullptr;  // vertical scrollbar around fInput
	InputContainer* fInputHost    = nullptr;  // Genio TerminalTab-style host for fInputScroll
	BButton*       fSend          = nullptr;  // bottom input pane button column
	BButton*       fStop          = nullptr;  // shown in Send's place while busy
	BMenuField*    fModelField    = nullptr;
	BPopUpMenu*    fModelMenu     = nullptr;
	BButton*       fClearBtn      = nullptr;  // bottom input pane button column
	SettingsDialog* fSettings      = nullptr;

	// ── Bottom input pane + find bar ───────────────────────────────────────────
	BGroupView*    fInputPane     = nullptr;  // bottom split item: find bar + input bar
	BGroupView*    fInputBar      = nullptr;  // bottom strip: input + button toolbar
	BView*         fFindBar       = nullptr;  // hidden container, between chat + input
	BTextControl*  fFindField     = nullptr;  // query input
	BStringView*   fFindStatus    = nullptr;  // "3 / 12" match counter
	int32          fFindMatchStart = -1;      // offset of the current match

	// ── Font zoom ──────────────────────────────────────────────────────────────
	float          fZoomFactor    = 1.0f;     // desired output font multiplier
	float          fAppliedZoom   = 1.0f;     // factor applied to text up to fZoomedLen
	int32          fZoomedLen     = 0;        // output length already scaled to fAppliedZoom

	// ── Session sidebar ────────────────────────────────────────────────────────
	BView*         fSessionPanel  = nullptr;  // left dock container (hidden by default)
	BListView*     fSessionList   = nullptr;  // titles of saved sessions
	BScrollView*   fSessionScroll = nullptr;  // scroller around fSessionList
	BSplitView*    fSplit         = nullptr;  // draggable divider: sidebar | chat
	BSplitView*    fVSplit        = nullptr;  // draggable divider: chat | input pane

	// Window minimum size (computed in _BuildLayout from the fixed row
	// heights); used to clamp a restored frame so it never starts below
	// the floor that keeps the input bar visible.
	float          fWindowMinW    = 360.0f;
	float          fWindowMinH    = 300.0f;

	// ── Conversation state ───────────────────────────────────────────────────
	config::Auth   fAuth;
	std::string    fModel;
	int            fMaxTokens;
	std::string    fSystemPrompt;
	std::string    fWorkingDir;   // working directory shown to Claude; empty = getcwd()
	nlohmann::json fMessages;
	int            fTurnCount     = 0;
	std::string    fConvTopic;    // first user message (used for window title)

	// ── Token accounting ─────────────────────────────────────────────────────
	int            fSessionInputTokens  = 0;
	int            fSessionOutputTokens = 0;

	// ── Code-block streaming state ───────────────────────────────────────────
	bool           fInCodeBlock   = false;
	std::string    fCodeLang;
	std::string    fCodeBuffer;
	std::string    fLineBuffer;

	// ── WebFetch HTML stripping ──────────────────────────────────────────────
	bool           fInWebFetch    = false;
	std::string    fWebFetchBuf;

	// ── Styling ─────────────────────────────────────────────────────────────
	styling::Theme       fTheme;
	styling::LanguageSet fLangSet;
	styling::CodeStyler* fStyler      = nullptr;
	md::MdRenderer*      fMdRenderer  = nullptr;

	// Code views kept for cleanup.
	std::vector<BView*>  fCodeViews;

	// ── Session persistence ──────────────────────────────────────────────────
	std::string    fSessionPath;   // path of the current saved session file

	// ── Export transcript ─────────────────────────────────────────────────────
	BFilePanel*    fExportPanel   = nullptr;  // lazily created B_SAVE_PANEL

	// ── Scroll tracking (sticky-scroll) ─────────────────────────────────────
	bool           fUserScrolled  = false;

	// ── Spinner timer ────────────────────────────────────────────────────────
	BMessageRunner* fSpinnerTimer  = nullptr;

	// ── Inline thinking spinner state ─────────────────────────────────────────
	// While waiting for the first token, a spinner line is the last text in
	// fOutput. fSpinnerActive guards the tick/erase; fSpinnerOffset is where
	// the spinner text begins (everything from there to end is rewritten or
	// erased). A fresh verb + clock are chosen per turn.
	bool       fSpinnerActive = false;
	int32      fSpinnerOffset = 0;
	int        fSpinnerStep   = 0;
	int        fSpinnerVerb   = 0;
	bigtime_t  fSpinnerStart  = 0;

	// ── Turn timing & tool tracking ───────────────────────────────────────────
	bigtime_t       fTurnStartTime        = 0;
	int             fToolsUsed            = 0;
	bool            fNotificationsEnabled = true; // per-window, toggled in settings
	int             fNotifyMinSec         = 5;    // delay before a turn notifies

	// ── Worker thread ────────────────────────────────────────────────────────
	std::thread         fWorker;
	std::atomic<bool>   fWorkerRunning { false };
	gui::GuiSink*       fSink          = nullptr;
	std::string         fPendingUserText;
	std::string         fPendingAssistantText;
	// Image files dropped onto the window for the next turn. Each entry
	// is { media_type, base64_data } and becomes an `image` content block
	// (base64 source) in the outgoing user message so Claude can see it.
	// Drained when the turn is launched.
	std::vector<std::pair<std::string, std::string>> fPendingImages;
	// The messages array the worker hands to api::SendWithTools. The worker
	// mutates it in place (appending tool_use / tool_result / assistant
	// blocks), so after the turn is joined the main thread adopts it as the
	// canonical conversation history — preserving tool context across turns.
	nlohmann::json      fWorkerMessages;
	// True when fWorkerMessages was built but the result wasn't committed
	// (cancelled): the MSG_WORKER_DONE handler then keeps history.
	bool                fTurnCommitted = false;
	// True while a /compact turn is in flight: MSG_WORKER_DONE replaces
	// fMessages with the compacted two-entry array instead of adopting
	// fWorkerMessages, and leaves the on-screen transcript intact.
	bool                fCompactPending = false;

	// ── Remote control (Telegram bridge) ──────────────────────────────────────
	// Guards fMessages reads taken by the remote-control poller thread
	// against the window thread that swaps/appends to it. Held briefly
	// around the snapshot in the SetSharedHistory provider and around the
	// window-thread mutations that the provider could observe. Declared
	// BEFORE fRemote so it outlives the poller during destruction (members
	// destruct in reverse declaration order; fRemote's dtor joins the
	// poller thread, which may still touch this mutex).
	std::mutex          fMessagesMutex;
	// Background poller that lets allowed Telegram users drive turns on this
	// machine. Null until Tools > Remote Control is enabled. _ToggleRemote()
	// owns the lifecycle; the destructor stops it cleanly.
	std::unique_ptr<telegram::RemoteControl> fRemote;
	void _ToggleRemote();  // Tools > Remote Control: start/stop the poller

	// ── Multi-session (Phase 2a) ─────────────────────────────────────────
	// Each window registers itself as a live remote-control session in the
	// process-wide telegram::SessionRegistry so the phone can /sessions
	// list and /session N switch between desktop windows. The registry ID
	// is assigned in the constructor and released in the destructor. The
	// window supplies a history provider/appender and a sink factory that
	// builds a GuiSink bound to this window, so a routed remote reply
	// streams into this window's transcript.
	int  fRegistrySessionId = 0;
	void _RegisterSession();
	void _UnregisterSession();
	// A short label for /sessions (conversation topic or "GUI session N").
	std::string _SessionLabel() const;

	// Extract displayable plain text from an Anthropic message "content"
	// field, which may be a bare string or an array of typed blocks. Used
	// to render Telegram-origin turns into the transcript; non-text blocks
	// (image, tool_use) are skipped.
	static std::string _PlainTextFromContent(const nlohmann::json& content);
};

#endif // HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
