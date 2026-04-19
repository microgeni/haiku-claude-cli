#ifndef HAIKU_CLAUDE_CLI_TUI_H
#define HAIKU_CLAUDE_CLI_TUI_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tui {

// Called once at startup to snapshot whether stdout supports ANSI colors,
// honoring NO_COLOR, CLICOLOR=0, and isatty(stdout). User flags
// (--plain / --color) then override the snapshot via SetColorEnabled.
void Init();

bool ColorEnabled();
void SetColorEnabled(bool on);

// Current terminal width in columns (TIOCGWINSZ). Returns 0 when
// stdout isn't a TTY. Re-reads TIOCGWINSZ on every call after a
// SIGWINCH has been delivered (cheap — a single ioctl) and caches
// the result in between. Safe to call concurrently with any
// streaming output.
int TerminalWidth();

// Current terminal height in rows (TIOCGWINSZ). Same cache and
// refresh behavior as TerminalWidth().
int TerminalRows();

// Installs a SIGWINCH handler that marks the cached terminal
// dimensions dirty so the next TerminalWidth()/TerminalRows()
// call re-reads TIOCGWINSZ. Call once at startup after init();
// no-op when stdout isn't a TTY.
void InstallSigwinchHandler();

// Fixed-bottom status frame. Carves off the bottom two rows of
// the terminal via DECSTBM scroll region:
//
//     row (rows-1): dim horizontal rule ──────…──
//     row (rows):   status bar content
//
// Chat history and the libedit prompt live in the remaining
// rows (1..rows-2) and scroll normally. The status bar stays
// fixed across streaming output, resize events, and prompt
// redraws.
//
// init_status_bar is idempotent; calling it a second time
// re-reads dimensions and redraws. No-op on non-TTY.
void InstallStatusBar(const std::string& initial_status = {});

// Update the status bar content and redraw immediately. Empty
// string clears the row but leaves the frame installed.
void SetStatusBar(const std::string& status);

// Re-read terminal dimensions, re-set the DECSTBM scroll
// region, and redraw the fixed rows. Called from the main REPL
// loop when ConsumeResizePending() returns true, and also
// directly by the SIGWINCH handler path.
void RedrawStatusBar();

// Non-zero if the SIGWINCH handler has fired since the last
// call. Resets to zero on read. Used by the REPL loop to
// trigger a redraw between prompts without touching signal
// handlers directly.
int ConsumeResizePending();

// Tear down the status frame: restore the full scroll region,
// clear the fixed rows, and move the cursor below them so the
// shell's next output doesn't overwrite a stale footer.
// Safe to call even when the frame was never installed.
void TeardownStatusBar();

// Emit an in-chat dim horizontal rule the full width of the
// current terminal followed by a newline. Meant to be called
// from the REPL loop right before each prompt to visually
// separate the previous turn from the next one. No-op on
// non-TTY / non-color.
//
// When the fixed-bottom status frame is active this is a no-op
// since the frame provides fixed rules above and below the
// input row — duplicating them in-chat would clutter the
// scroll history.
void EmitChatRule();

// Position the cursor on the fixed input row of the status
// frame (row N-2) and clear it so libedit starts with a clean
// line. No-op when the frame isn't active — the caller just
// lets libedit draw wherever the cursor currently is.
void PositionCursorForInput();

// Immediately clear the fixed input row after the user presses
// Enter. Moves to the input row, erases it, then returns the
// cursor to the chat scroll region so subsequent output (spinner,
// streamed reply) lands in the right place. Call this right after
// ReadMessage() returns so the submitted text doesn't linger on
// the input row for the duration of the turn. No-op when the
// status frame isn't active.
void ClearInputRow();

// Position the cursor at the bottom of the scroll region
// (row N-4 when the 4-row frame is active) so subsequent stdout
// writes flow into the chat history area instead of bleeding
// into the fixed rows. No-op when the frame isn't active.
void PositionCursorForChat();

// Show / hide the terminal cursor via DECTCEM (\e[?25h / \e[?25l).
// Used to suppress cursor-bouncing during streaming output
// so the cursor appears steady and only reappears at the
// prompt. No-op on non-TTY / non-color.
void HideCursor();
void ShowCursor();

// Interactive vertical menu. Renders `options` as a numbered list:
//
//     1. Yes, allow once
//     2. Always allow this session
//     3. No, deny
//
// The currently selected option is highlighted in bold+cyan; others
// are dim. The user navigates with Up/Down arrows and confirms with
// Enter, or jumps directly to an option with its number key (1..N).
// Pressing Esc or 'n' (if present) selects the last option (deny).
//
// Returns the 0-based index of the chosen option, or `options.size()-1`
// on Esc / interrupt.
//
// When `heading` is non-empty it is rendered as the first line of the
// owned block (bold on TTY). On selection the entire block — heading
// plus all option rows — is erased and replaced with a single compact
// summary line:  "<heading> → <chosen label>"  so scroll history stays
// informative without the full menu cluttering it.
//
// Temporarily puts stdin into raw mode for single-keypress reads,
// restoring it on return. Works inside the fixed-bottom status frame
// (renders in the scroll region, reads at the input row) or standalone.
// Falls back to a plain numbered prompt on non-TTY stdout.
int SelectOption(const std::vector<std::string>& options,
				  const std::string& heading = {});

std::string Bold(const std::string& s);
std::string Dim(const std::string& s);
std::string Italic(const std::string& s);

std::string Red(const std::string& s);
std::string Green(const std::string& s);
std::string Yellow(const std::string& s);
std::string Blue(const std::string& s);
std::string Magenta(const std::string& s);
std::string Cyan(const std::string& s);
std::string Gray(const std::string& s);
// Muted() = dim + bright-black, the consistent "darker gray" used
// on the rule lines framing the input row and on the status-row
// fields (except the model name and the Remote-Control label,
// which get their own color so they stand out).
std::string Muted(const std::string& s);

// Semantic wrappers. These return the full string to print (including
// trailing reset), or a plain equivalent when color is off.
std::string UserPrompt();              // "you> "
std::string ClaudePrompt();            // "claude> "
std::string ContinuationPrompt();      // "... " for multi-line input
std::string Meta(const std::string& s); // dim bracketed note
std::string ErrorLabel();              // bold red "error:"

// Forward declaration so MarkdownRenderer can reference Spinner.
class Spinner;

// Incremental markdown renderer for streamed text. Accepts text chunks
// via write(), buffers by line internally, and emits ANSI-formatted
// output to stdout. Handles bold **, italic *, inline code `,
// fenced code blocks ```lang, headings #/##/###, and bullet/numbered
// lists. Nested inline formatting is not supported (scope: 80% case).
//
// Falls back to raw passthrough when color is disabled.
//
// Optionally holds a non-owning Spinner pointer: on the first byte of
// real output, the spinner is stopped so the user sees the thinking
// indicator up until something is actually visible.
enum class TableAlign { Left, Right, Center };

class MarkdownRenderer {
public:
	MarkdownRenderer();
	void SetSpinner(Spinner* s) { fSpinner = s; }
	void Write(const std::string& chunk);
	void Flush();
private:
	void Emit(const std::string& s);
	void RenderLine(const std::string& line);
	void RenderInline(const std::string& text);
	std::string RenderInlineToString(const std::string& text);

	// Markdown table buffering. Tables span multiple lines and
	// need per-column width computation, so we accumulate rows
	// here and emit the aligned output on the first non-table
	// line (or on flush()).
	void FlushTable();

	std::string fLineBuffer;
	std::string fCodeBlockLang;
	bool        fInCodeBlock     = false;
	bool        fFirstOutputDone = false;
	Spinner*    fSpinner           = nullptr;

	std::vector<std::vector<std::string>> fTableRows;
	std::vector<TableAlign>               fTableAligns;
	bool                                  fTableActive = false;
};

// Animated "thinking..." indicator. Spawns a background thread that
// writes dimmed spinner frames to stdout until Stop() (or destruction).
// No-op when color is disabled or stdout is not a TTY.
class Spinner {
public:
	explicit Spinner(std::string label);
	~Spinner();
	Spinner(const Spinner&) = delete;
	Spinner& operator=(const Spinner&) = delete;
	void Stop();

	// Optional non-owning pointer to a live input-token counter that
	// the spinner reads on each frame. When the counter is > 0, the
	// spinner appends `↑ <N> tokens` to its rendered line, matching
	// Claude Code's `(44s · ↑ 652 tokens)` style. Pointer must remain
	// valid for the spinner's lifetime. Safe to leave null.
	void SetLiveInputTokens(const std::atomic<int>* p) {
		fLiveInputTokens = p;
	}
private:
	void Run();
	std::string                   fLabel;
	std::atomic<bool>             fStopping{false};
	std::mutex                    fMutex;
	std::condition_variable       fCv;
	std::thread                   fThread;
	bool                          fActive = false;
	const std::atomic<int>*       fLiveInputTokens = nullptr;
};

} // namespace tui

#endif
