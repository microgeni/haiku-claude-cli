#ifndef HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
#define HAIKU_CLAUDE_CLI_CHAT_WINDOW_H

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <Button.h>
#include <Bitmap.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Messenger.h>
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

class BFilePanel;

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
constexpr uint32_t MSG_COMPLETE_CMD = 'CCMD'; // slash command selected (string "cmd")
constexpr uint32_t MSG_POPUP_UPDATE = 'PUPT'; // InputView → ChatWindow: update popup
constexpr uint32_t MSG_POPUP_NEXT   = 'PNXT'; // InputView → ChatWindow: next item
constexpr uint32_t MSG_POPUP_PREV   = 'PPRV'; // InputView → ChatWindow: prev item
constexpr uint32_t MSG_POPUP_CONF   = 'PCNF'; // InputView → ChatWindow: confirm
constexpr uint32_t MSG_POPUP_HIDE   = 'PDIS'; // InputView → ChatWindow: hide
constexpr uint32_t MSG_MODELS_READY = 'MDLS'; // background model fetch complete
constexpr uint32_t MSG_ABOUT        = 'ABUT'; // Help > About Claude
constexpr uint32_t MSG_HELP_DOCS    = 'HDOC'; // Help > Documentation
constexpr uint32_t MSG_DEMO_MARKDOWN = 'DMMD'; // Help > Show Markdown Demo
constexpr uint32_t MSG_LUDICROUS     = 'LUDC'; // Tools > Ludicrous Mode toggle
constexpr uint32_t MSG_BROWSE_WORKDIR = 'BRWD'; // Settings: browse for working dir
constexpr uint32_t MSG_EXPORT         = 'EXPT'; // File > Export Transcript…
constexpr uint32_t MSG_EXPORT_SAVE    = 'EXPS'; // B_SAVE_REQUESTED from export panel
} // namespace gui

// ─────────────────────────────────────────────────────────────────────────────
// CommandPopup — thin wrapper around BPopUpMenu for slash-command completion.
// BPopUpMenu::Go() runs its own nested event loop safely from MessageReceived.
// ChatWindow calls Show(prefix, screenPt) which blocks until user picks or
// dismisses, then posts MSG_COMPLETE_CMD with the chosen command.
// ─────────────────────────────────────────────────────────────────────────────
class CommandPopup {
public:
	explicit CommandPopup(BHandler* target) : fTarget(target) {}

	// Show the menu at screenPt. Blocks until dismissed (BPopUpMenu::Go).
	// Posts MSG_COMPLETE_CMD to fTarget if an item was selected.
	void	Show(const std::string& prefix, BPoint screenPt);

	bool	IsPopupVisible() const { return fVisible; }

private:
	BHandler* fTarget  = nullptr;
	bool      fVisible = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// InputView — single-row BTextView that sends on Enter. Up/Down arrow keys
// navigate prompt history. Escape forwards MSG_CANCEL to the window.
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
// TokenBar — thin view below the output area showing token usage as a
// coloured fill bar and a compact "used / max" label.
// ─────────────────────────────────────────────────────────────────────────────
class TokenBar : public BView {
public:
	static const int kBarHeight = 18;

	TokenBar();

	void	Draw(BRect updateRect) override;
	void	SetTokens(int used, int maxCtx);

	// Per-session counters mirrored from the CLI status row:
	// turn number plus cumulative upstream (↑) / downstream (↓) tokens.
	void	SetStats(int turn, int sessionInput, int sessionOutput);

private:
	int  fUsed    = 0;
	int  fMax     = 200000; // default until first real value arrives
	int  fTurn    = 0;
	int  fInput   = 0;
	int  fOutput  = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SettingsPanel — slide-in panel docked on the right side of the window.
// Contains the model picker, system-prompt editor, max-tokens field, and a
// close button. Hidden width = 0; shown width = kPanelWidth.
// ─────────────────────────────────────────────────────────────────────────────
class SettingsPanel : public BView {
public:
	static constexpr float kPanelWidth = 280.0f;

	SettingsPanel(const std::string& systemPrompt, int maxTokens,
	              int notifyMinSec, const std::string& workingDir = {},
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

private:
	void	_BuildLayout(const std::string& systemPrompt, int maxTokens,
	                     int notifyMinSec, const std::string& workingDir,
	                     BMenuField* modelField);

	BTextView*    fSysPromptView  = nullptr;
	BTextControl* fMaxTokensCtl   = nullptr;
	BSlider*      fNotifyDelay    = nullptr;
	BTextControl* fWorkingDirCtl  = nullptr;
	bool          fOpen           = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// SpinnerView — small animated arc drawn while the worker is running.
// ─────────────────────────────────────────────────────────────────────────────
class SpinnerView : public BView {
public:
	static const int kSize = 18;

	SpinnerView();

	void	Draw(BRect updateRect) override;
	void	Tick();               // advance one step (called on TICK message)
	void	SetVisible(bool v);

private:
	int   fStep    = 0;
	bool  fVisible = false;
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
//   ┌──────────────────────────────────┬──────────────┐
//   │  [output BTextView + scrollbar]  │  [Settings]  │
//   │                                  │  (slide-in)  │
//   ├──────────────────────────────────┤              │
//   │  [token bar 14px]                │              │
//   ├──────────────────────────────────┘              │
//   │  [spinner] [input BTextView]  [Send  ]          │
//   │                               [Clear ]          │
//   │                               [⚙ Stng]          │
//   (the model picker now lives inside the Settings panel)
//   └─────────────────────────────────────────────────┘
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
	void _HandlePermRequest(BMessage* msg);
	void _HandleChoiceRequest(BMessage* msg);
	void _CancelWorker();
	void _NewChat();
	void _ClearOutput();

	// ── Toolbar / UI state ───────────────────────────────────────────────────
	void _SetBusy(bool busy);    // swap Send↔Stop, enable/disable input
	void _UpdateTitle();         // set window title from model + state
	void _SaveSession();         // persist current conversation to BFS
	void _LoadSession(const std::string& path); // restore a saved session
	void _InsertFileContent(const std::string& path); // drag-drop helper
	void _ShowMarkdownDemo();    // render a rich markdown example into the chat output
	void _DismissWelcome();      // hide the startup splash once a turn begins
	void _ExportTranscript(const std::string& path); // write fMessages as Markdown

	// ── Widgets ─────────────────────────────────────────────────────────────
	BMenuBar*      fMenuBar       = nullptr;  // native top menu bar
	BMenuItem*     fLudicrousItem = nullptr;  // Tools > Ludicrous Mode (checkmark)
	BTextView*     fOutput        = nullptr;
	WelcomeView*   fWelcome       = nullptr;  // startup splash, collapsed on first turn
	BScrollView*   fScroll        = nullptr;
	BButton*       fJumpBtn       = nullptr;  // floating "↓" overlay button
	TokenBar*      fTokenBar      = nullptr;
	SpinnerView*   fSpinner       = nullptr;
	InputView*     fInput         = nullptr;
	BButton*       fSend          = nullptr;
	BButton*       fStop          = nullptr;  // replaces Send while busy
	BMenuField*    fModelField    = nullptr;
	BPopUpMenu*    fModelMenu     = nullptr;
	BButton*       fClearBtn      = nullptr;
	BButton*       fSettingsBtn   = nullptr;
	SettingsPanel* fSettings      = nullptr;

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

	// ── Slash-command autocomplete ───────────────────────────────────────────
	CommandPopup*  fCommandPopup  = nullptr;

	// ── Export transcript ─────────────────────────────────────────────────────
	BFilePanel*    fExportPanel   = nullptr;  // lazily created B_SAVE_PANEL

	// ── Scroll tracking (sticky-scroll) ─────────────────────────────────────
	bool           fUserScrolled  = false;

	// ── Spinner timer ────────────────────────────────────────────────────────
	BMessageRunner* fSpinnerTimer  = nullptr;

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
	// True when the worker finished normally (not cancelled), so the
	// MSG_WORKER_DONE handler knows whether to commit fWorkerMessages.
	bool                fTurnCommitted = false;
};

#endif // HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
