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
class SessionWindow;

namespace telegram { class RemoteControl; }

// Additional MSG_ codes beyond those in gui_sink.h live in gui_messages.h,
// included here so existing users of "chat_window.h" still see them.
#include "gui_messages.h"

// The custom chat views (InputView, InputContainer, ChatTextView, TokenBar,
// WelcomeView) are declared in gui_views.h (extracted). Included so
// ChatWindow's members and construction sites keep compiling.
#include "gui_views.h"


// ─────────────────────────────────────────────────────────────────────────────
// SettingsDialog is declared in settings_dialog.h (extracted). Included here
// so ChatWindow's fSettings member and construction site keep compiling.
#include "settings_dialog.h"


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
	           const std::string& workingDir = {},
	           const std::string& initialPrompt = {},
	           bool autoSend = false);
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
	void _LaunchLearn();         // Tools > Learn a Skill…: distil a SKILL.md
	void _RefreshSkillMenu();    // rebuild Tools > Skills from disk
	void _RunSkill(const std::string& name);  // expand a skill, send as a turn
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
	void _ShowDiagnostics();     // open the Help ▸ Diagnostics report window
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
	BMenuItem*     fPlanItem      = nullptr;  // Tools > Plan Mode (checkmark)
	BMenuItem*     fRemoteItem    = nullptr;  // Tools > Remote Control (checkmark)
	BMenu*         fSkillMenu    = nullptr;  // Tools > Skills (rebuilt on demand)
	BMenuItem*     fInputItem     = nullptr;  // View > Input (checkmark)
	BTextView*     fOutput        = nullptr;
	WelcomeView*   fWelcome       = nullptr;  // startup splash, collapsed on first turn
	BScrollView*   fScroll        = nullptr;
	BButton*       fJumpBtn       = nullptr;  // "↓" on the spinner row, enabled when needed
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
	float          fAppliedZoom   = 1.0f;     // uniform zoom currently applied to the buffer

	// ── Session window (separate floating window) ────────────────────────────
	SessionWindow* fSessionWindow = nullptr;  // standalone session list (lazy)
	BListView*     fSessionList   = nullptr;  // list inside fSessionWindow
	BSplitView*    fSplit         = nullptr;  // (unused; chat has no sidebar now)
	BSplitView*    fVSplit        = nullptr;  // draggable divider: chat | input pane

	// Window minimum size (computed in _BuildLayout from the fixed row
	// heights); used to clamp a restored frame so it never starts below
	// the floor that keeps the input bar visible.
	float          fWindowMinW    = 360.0f;
	float          fWindowMinH    = 300.0f;

	// ── Conversation state ───────────────────────────────────────────────────
	config::Auth   fAuth;
	std::string    fModel;
	// The model this window was constructed with (from config.json / CLI).
	// When it differs from the built-in default the user has explicitly
	// pinned a model, so the auto-saved GUI last-model must not override it.
	std::string    fConfigModel;
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
	// True once a thinking chunk has arrived this turn, so the first real
	// text chunk can insert a separator after the dim reasoning block.
	bool           fInThinking    = false;
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

	// ── Working-directory picker ─────────────────────────────────────────────
	// Cached like fExportPanel. BFilePanel does NOT delete itself when
	// dismissed (hideWhenDone defaults to true — it only hides), and each
	// instance owns a BWindow costing 3 semaphores, so creating one per
	// Browse click leaked them until the app quit.
	BFilePanel*    fBrowsePanel   = nullptr;  // lazily created B_OPEN_PANEL

	// ── Scroll tracking (sticky-scroll) ─────────────────────────────────────
	// No flag is kept: every append site samples _IsNearBottom() before
	// inserting and follows the output only if the user was already at the
	// bottom. The previous fUserScrolled member was never set to true, so
	// every "don't scroll" check passed and appends always jumped to the
	// end — which is what defeated scrolling back to read.

	// ── Spinner timer ────────────────────────────────────────────────────────
	BMessageRunner* fSpinnerTimer  = nullptr;

	// ── Inline thinking spinner state ─────────────────────────────────────────
	// The spinner frame is drawn in fSpinnerView, a fixed-height strip
	// directly below the chat. It is deliberately NOT written into fOutput:
	// rewriting trailing text every 80 ms reflowed the document and fought
	// the scroll position, which made the view jitter during a reply.
	// fSpinnerActive guards the tick. A fresh verb + clock per turn.
	BStringView* fSpinnerView   = nullptr;
	bool       fSpinnerActive = false;
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
	// Set for a /learn turn so MSG_WORKER_DONE rescans the skills dir —
	// the turn may have written a new SKILL.md that the menu should show.
	bool                fLearnPending   = false;

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
