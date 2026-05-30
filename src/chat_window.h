#ifndef HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
#define HAIKU_CLAUDE_CLI_CHAT_WINDOW_H

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <Button.h>
#include <CheckBox.h>
#include <ListView.h>
#include <MenuField.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
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
#include "session_store.h"

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
constexpr uint32_t MSG_SESSIONS     = 'SESS'; // toggle session panel
constexpr uint32_t MSG_SESSION_LOAD = 'SLOD'; // load a session (int32 "index")
constexpr uint32_t MSG_COMPLETE_CMD = 'CCMD'; // slash command selected (string "cmd")
constexpr uint32_t MSG_POPUP_UPDATE = 'PUPT'; // InputView → ChatWindow: update popup
constexpr uint32_t MSG_POPUP_NEXT   = 'PNXT'; // InputView → ChatWindow: next item
constexpr uint32_t MSG_POPUP_PREV   = 'PPRV'; // InputView → ChatWindow: prev item
constexpr uint32_t MSG_POPUP_CONF   = 'PCNF'; // InputView → ChatWindow: confirm
constexpr uint32_t MSG_POPUP_HIDE   = 'PDIS'; // InputView → ChatWindow: hide
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
	void	SetVisible(bool v) { fVisible = v; }

private:
	BHandler* fTarget  = nullptr;
	bool      fVisible = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// InputView — multi-line BTextView that sends on Enter, inserts newline on
// Shift+Enter. Auto-sizes vertically up to kMaxInputLines. Ctrl+Up/Down
// navigate prompt history. Escape forwards MSG_CANCEL to the window.
// ─────────────────────────────────────────────────────────────────────────────
class InputView : public BTextView {
public:
	static const int kMinLines = 2;  // minimum visible lines
	static const int kMaxLines = 10; // maximum before scrolling

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

	// Adjust height to fit content up to kMaxLines lines. Returns the new
	// preferred height in pixels; caller must resize the container.
	float	PreferredHeight() const;

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

public:
	bool                     fPopupOpen  = false; // tracks popup visibility
};

// ─────────────────────────────────────────────────────────────────────────────
// TokenBar — thin view below the output area showing token usage as a
// coloured fill bar and a compact "used / max" label.
// ─────────────────────────────────────────────────────────────────────────────
class TokenBar : public BView {
public:
	static const int kBarHeight = 14;

	TokenBar();

	void	Draw(BRect updateRect) override;
	void	SetTokens(int used, int maxCtx);

private:
	int  fUsed = 0;
	int  fMax  = 200000; // default until first real value arrives
};

// ─────────────────────────────────────────────────────────────────────────────
// SettingsPanel — slide-in panel docked on the right side of the window.
// Contains system-prompt editor, max-tokens field, and a close button.
// Hidden width = 0; shown width = kPanelWidth.
// ─────────────────────────────────────────────────────────────────────────────
class SettingsPanel : public BView {
public:
	static constexpr float kPanelWidth = 280.0f;

	SettingsPanel(const std::string& systemPrompt, int maxTokens);

	// Populate fields from current config.
	void	SetValues(const std::string& systemPrompt, int maxTokens);

	// Read back edited values.
	std::string	SystemPrompt() const;
	int         MaxTokens() const;
	bool        NotificationsEnabled() const;

	bool	IsOpen() const { return fOpen; }
	void	Toggle();

private:
	void	_BuildLayout(const std::string& systemPrompt, int maxTokens);

	BTextView*    fSysPromptView  = nullptr;
	BTextControl* fMaxTokensCtl   = nullptr;
	BCheckBox*    fNotifyChk      = nullptr;
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
// SessionPanel — slide-in panel docked on the LEFT side of the window.
// Lists saved sessions sorted by most-recent-first. Click a row to load it.
// ─────────────────────────────────────────────────────────────────────────────
class BListView;
class BScrollView;

class SessionPanel : public BView {
public:
	static constexpr float kPanelWidth = 240.0f;

	explicit SessionPanel(BHandler* target);

	// Reload session list from disk and repopulate the BListView.
	void	Refresh();

	bool	IsOpen() const { return fOpen; }
	void	Toggle();

	// Return the SessionInfo for row index, or nullptr if out of range.
	const session::SessionInfo* InfoAt(int32_t index) const;

	// Public so ChatWindow::MessageReceived can query the selection.
	BListView*                      fList    = nullptr;

private:
	void	_BuildLayout();

	BScrollView*                    fScroll  = nullptr;
	BHandler*                       fTarget  = nullptr;
	std::vector<session::SessionInfo> fSessions;
	bool                            fOpen    = false;
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
//   │  [spinner] [input BTextView]  [Send] [Stop]     │
//   │  [model menu] [New] [Clear] [⚙ Settings]        │
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
	           int maxTokens, const std::string& systemPrompt);
	~ChatWindow() override;

	void MessageReceived(BMessage* msg) override;
	void _RefsReceived(BMessage* msg); // drag-drop / file open handler
	bool QuitRequested() override;
	void FrameResized(float w, float h) override;

private:
	// ── Layout ──────────────────────────────────────────────────────────────
	void _BuildLayout();
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
	void _CancelWorker();
	void _NewChat();
	void _ClearOutput();

	// ── Toolbar / UI state ───────────────────────────────────────────────────
	void _SetBusy(bool busy);    // swap Send↔Stop, enable/disable input
	void _UpdateTitle();         // set window title from model + state
	void _SaveSession();         // persist current conversation to BFS
	void _LoadSession(const std::string& path); // restore a saved session
	void _InsertFileContent(const std::string& path); // drag-drop helper

	// ── Widgets ─────────────────────────────────────────────────────────────
	BTextView*     fOutput        = nullptr;
	BScrollView*   fScroll        = nullptr;
	BButton*       fJumpBtn       = nullptr;  // floating "↓" overlay button
	TokenBar*      fTokenBar      = nullptr;
	SpinnerView*   fSpinner       = nullptr;
	InputView*     fInput         = nullptr;
	BScrollView*   fInputScroll   = nullptr;
	BButton*       fSend          = nullptr;
	BButton*       fStop          = nullptr;  // replaces Send while busy
	BMenuField*    fModelField    = nullptr;
	BPopUpMenu*    fModelMenu     = nullptr;
	BButton*       fNewBtn        = nullptr;
	BButton*       fClearBtn      = nullptr;
	BButton*       fSettingsBtn   = nullptr;
	SettingsPanel* fSettings      = nullptr;

	// ── Conversation state ───────────────────────────────────────────────────
	config::Auth   fAuth;
	std::string    fModel;
	int            fMaxTokens;
	std::string    fSystemPrompt;
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
	SessionPanel*  fSessionPanel  = nullptr;
	BButton*       fSessionBtn    = nullptr;

	// ── Slash-command autocomplete ───────────────────────────────────────────
	CommandPopup*  fCommandPopup  = nullptr;

	// ── Scroll tracking (sticky-scroll) ─────────────────────────────────────
	bool           fUserScrolled  = false;

	// ── Spinner timer ────────────────────────────────────────────────────────
	BMessageRunner* fSpinnerTimer  = nullptr;

	// ── Turn timing & tool tracking ───────────────────────────────────────────
	bigtime_t       fTurnStartTime        = 0;
	int             fToolsUsed            = 0;
	bool            fNotificationsEnabled = true; // per-window, toggled in settings

	// ── Worker thread ────────────────────────────────────────────────────────
	std::thread         fWorker;
	std::atomic<bool>   fWorkerRunning { false };
	gui::GuiSink*       fSink          = nullptr;
	std::string         fPendingUserText;
	std::string         fPendingAssistantText;
};

#endif // HAIKU_CLAUDE_CLI_CHAT_WINDOW_H
