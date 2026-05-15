#include "tui.h"
#include "repl.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <streambuf>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <editline/readline.h>

namespace tui {
namespace {

bool g_color_enabled = false;

std::string wrap(const char* on, const std::string& s, const char* off = "\x1b[0m") {
	if (!g_color_enabled) return s;
	std::string out;
	out.reserve(s.size() + 16);
	out.append(on);
	out.append(s);
	out.append(off);
	return out;
}

bool detect_color_support() {
	if (!isatty(fileno(stdout))) return false;

	if (const char* v = std::getenv("NO_COLOR"); v && *v) return false;  // flawfinder: ignore

	if (const char* v = std::getenv("CLICOLOR"); v && std::string(v) == "0") return false;  // flawfinder: ignore

	if (const char* v = std::getenv("TERM"); v) {  // flawfinder: ignore
		if (std::string(v) == "dumb") return false;
	}
	return true;
}

} // namespace

void Init() {
	g_color_enabled = detect_color_support();
}

// ---------------------------------------------------------------------------
// Concurrent-output pending buffer.
//
// When a turn is in progress the main thread is inside libedit's
// readline() call while the worker thread is streaming the response.
// Both write to stdout (libedit on row N-2; the worker to the scroll
// region).  Writing concurrently corrupts the terminal state.
//
// Solution: while a turn is active, std::cout is redirected through
// TurnOutputBuf which accumulates worker output in g_turn_pending
// (under g_turn_pending_mu).  The repl getcfn hook (called by libedit
// before every keypress read) flushes g_turn_pending to the scroll
// region using DECSC/DECRC so libedit's cursor on row N-2 is
// undisturbed.
//
// BeginTurn() installs the interceptor; EndTurn() removes it and
// flushes any remaining buffered output.
// ---------------------------------------------------------------------------

namespace {

std::mutex   g_turn_pending_mu;
std::string  g_turn_pending;
std::streambuf* g_cout_orig_buf = nullptr;

// Custom streambuf: while active, all cout writes go to g_turn_pending.
class TurnOutputBuf : public std::streambuf {
public:
	// Write a sequence of characters into the pending buffer.
	std::streamsize xsputn(const char* s, std::streamsize n) override {
		std::lock_guard<std::mutex> lk(g_turn_pending_mu);
		g_turn_pending.append(s, static_cast<size_t>(n));
		return n;
	}
	// Single-character write (fallback path used by some libc impls).
	int overflow(int c) override {
		if (c == EOF) return c;
		std::lock_guard<std::mutex> lk(g_turn_pending_mu);
		g_turn_pending.push_back(static_cast<char>(c));
		return c;
	}
};

TurnOutputBuf g_turn_buf;

// Flush timer: wakes every ~16 ms while a turn is active and calls
// FlushTurnOutput() so the response appears even when the user is not
// typing (keystroke-driven flushing alone produces no output at idle).
std::atomic<bool> g_flush_timer_running{false};
std::thread       g_flush_timer_thread;

// Pause/resume flag for the flush timer.  When g_flush_timer_paused is
// true the timer loop spins on a short sleep without calling
// FlushTurnOutput() or touching stdout, giving SelectOption() exclusive
// access to the terminal.  g_flush_timer_paused_ack is set by the
// timer loop once it has acknowledged the pause.
std::atomic<bool> g_flush_timer_paused    {false};
std::atomic<bool> g_flush_timer_paused_ack{false};

// Scroll-region cursor tracking across flushes. Set to scroll-region
// bottom col 1 on the first flush; updated by simulating cursor movement
// through each chunk so subsequent flushes CUP to the correct position.
bool g_turn_started = false;
int  g_turn_row2    = 0;
int  g_turn_col     = 1;

// Callback installed by session.cpp so the flush timer can detect when
// the worker has finished and inject a synthetic keypress to unblock
// ReadMessage(). Cleared by EndTurn().
std::function<bool()> g_turn_done_check;

// Extra rows currently allocated to the expanding multi-line input area.
// Declared here (before FlushTurnOutput) so the scroll_bottom
// calculation in FlushTurnOutput can account for the expanded geometry.
// Managed by ExpandInputArea() and CollapseInputArea().
int g_extra_input_rows = 0;

// Completed continuation lines accumulated during multi-line input.
// ExpandInputArea() appends each line here so the block can be
// repainted correctly on every expansion (oldest line at index 0).
std::vector<std::string> g_completed_lines;

} // namespace

// Exposed so session.cpp can clear it at the top of the main loop.
std::atomic<bool> g_turn_just_completed{false};

// Write directly to the real terminal fd, bypassing the interceptor.
// Used by SetStatusBar and other fixed-frame drawing functions that
// must reach the terminal even while a turn is active.
static void DirectWrite(const std::string& s) {
	if (s.empty()) return;
	if (g_cout_orig_buf) {
		g_cout_orig_buf->sputn(s.data(), static_cast<std::streamsize>(s.size()));
		g_cout_orig_buf->pubsync();
	} else {
		const ssize_t n = ::write(fileno(stdout), s.data(), s.size());
		(void)n;
	}
}

// Flush buffered turn output to the scroll region.
// Uses absolute CUP positioning to place output correctly in the scroll
// region, then returns the cursor to the input row (N-2).
// Safe to call from any thread; must NOT be called with g_turn_pending_mu held.
void FlushTurnOutput() {
	std::string chunk;
	{
		std::lock_guard<std::mutex> lk(g_turn_pending_mu);
		if (g_turn_pending.empty()) return;
		chunk.swap(g_turn_pending);
	}

	// Build the output sequence:
	//   1. DECSC — save libedit's cursor (input row N-2 col C)
	//   2. CUP to g_turn_col/row — resume from where last flush ended
	//   3. chunk
	//   4. DECRC — restore libedit's cursor
	//
	// After step 3 the terminal cursor is at some position inside the
	// scroll region.  We can't query it, so we track g_turn_col/row by
	// simulating cursor movement from the chunk content.  Newlines
	// within the scroll region scroll normally; \r resets column to 1;
	// printable chars advance the column.  We don't handle all CSI
	// sequences but handle the common ones the spinner and renderer emit.

	// Simulate cursor movement through chunk to update g_turn_col/row.
	const int rows = TerminalRows();
	// scroll_bottom shrinks when the input area is expanded for multi-line
	// input (g_extra_input_rows > 0).
	const int scroll_bottom = rows > (4 + g_extra_input_rows)
		? rows - 4 - g_extra_input_rows
		: rows - 1;

	// On the very first flush, start at scroll-region bottom col 1.
	if (!g_turn_started) {
		g_turn_col = 1;
		g_turn_row2 = scroll_bottom;
		g_turn_started = true;
	}

	std::string out;
	out.reserve(32 + chunk.size());
	out += "\x1b""7";    // DECSC — save libedit's cursor

	// CUP to tracked scroll-region position.
	out += "\x1b[" + std::to_string(g_turn_row2) + ";" + std::to_string(g_turn_col) + "H";
	out += chunk;

	// Simulate cursor movement through chunk.
	bool in_esc = false;
	bool in_csi = false;
	for (unsigned char c : chunk) {
		if (in_csi) {
			if (c >= 0x40 && c <= 0x7E) in_csi = in_esc = false;
		} else if (in_esc) {
			if (c == '[') in_csi = true; else in_esc = false;
		} else if (c == '\x1b') {
			in_esc = true;
		} else if (c == '\r') {
			g_turn_col = 1;
		} else if (c == '\n') {
			g_turn_col = 1;
			if (g_turn_row2 < scroll_bottom) ++g_turn_row2;
			// else scroll region scrolls — row stays at bottom
		} else if (c >= 0x20 && c < 0x7F) {
			++g_turn_col; // approximate; ignores UTF-8 multi-byte
		}
	}

	out += "\x1b""8";    // DECRC — restore libedit's cursor

	if (g_cout_orig_buf) {
		g_cout_orig_buf->sputn(out.data(), static_cast<std::streamsize>(out.size()));
		g_cout_orig_buf->pubsync();
	} else {
		const ssize_t n = ::write(fileno(stdout), out.data(), out.size());
		(void)n;
	}
}

void BeginTurn() {
	if (!isatty(fileno(stdout))) return;
	if (g_cout_orig_buf) return; // already installed
	{
		std::lock_guard<std::mutex> lk(g_turn_pending_mu);
		g_turn_pending.clear();
	}
	g_turn_started = false;  // reset first-flush sentinel
	g_turn_row2    = 0;
	g_turn_col     = 1;
	// Redirect cout through the interceptor.
	g_cout_orig_buf = std::cout.rdbuf(&g_turn_buf);

	// Start the flush timer so output appears without requiring keystrokes.
	// The timer also watches for turn completion and injects a synthetic
	// newline into libedit's input queue so the main loop wakes up and
	// calls drain_turn() without requiring a real keypress.
	g_flush_timer_running.store(true);
	g_flush_timer_paused.store(false);
	g_flush_timer_paused_ack.store(false);
	g_flush_timer_thread = std::thread([]() {
		while (g_flush_timer_running.load()) {
			// When paused, spin without touching stdout so SelectOption()
			// has exclusive access to the terminal.
			if (g_flush_timer_paused.load()) {
				g_flush_timer_paused_ack.store(true);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			g_flush_timer_paused_ack.store(false);

			// Flush first, then sleep.  The original order (sleep→flush)
			// delayed every batch of output by one full 16 ms period
			// because the buffer sat untouched for the entire sleep before
			// the first write to the terminal.  Flushing first means each
			// chunk is visible within microseconds of being written by the
			// worker thread; the sleep is purely a rate-limiter that
			// prevents busy-spinning between batches.
			FlushTurnOutput();

			// Check turn completion immediately after flushing so the
			// synthetic wake fires as soon as the worker is done, rather
			// than being deferred by another full sleep period.  Any output
			// that arrived during this cycle has already been flushed above.
			if (g_turn_done_check && g_turn_done_check()) {
				g_turn_just_completed.store(true);
				// Wake the blocking poll() in raw_getc_or_wake() so
				// bracketed_getc can return a synthetic '\r' and the
				// main loop's drain_turn() fires without a real keypress.
				repl::WakeReadMessage();
				g_flush_timer_running.store(false);
				break;
			}

			// Rate-limit: sleep between flush cycles so we don't busy-spin.
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			if (!g_flush_timer_running.load()) break;
			if (g_flush_timer_paused.load())   continue; // re-check after sleep
		}
	});
}

void EndTurn() {
	// Stop the flush timer first so it doesn't race with the restore.
	g_flush_timer_running.store(false);
	if (g_flush_timer_thread.joinable())
		g_flush_timer_thread.join();
	g_turn_done_check = nullptr;

	if (!g_cout_orig_buf) return;
	// Restore cout to the original buffer, then flush remaining output.
	std::cout.rdbuf(g_cout_orig_buf);
	g_cout_orig_buf = nullptr;
	FlushTurnOutput();
	std::cout.flush();
}

void SetTurnDoneCheck(std::function<bool()> fn) {
	g_turn_done_check = std::move(fn);
}

namespace {

// Cached terminal dimensions. Set to dirty initially so the first
// call to TerminalWidth()/TerminalRows() triggers an ioctl. The
// SIGWINCH handler flips both dirty flags so the next read re-
// populates the cache. sig_atomic_t because a signal handler
// writes to them.
volatile std::sig_atomic_t g_term_dirty        = 1;
volatile std::sig_atomic_t g_resize_pending    = 0;
int                        g_cached_term_cols  = 0;
int                        g_cached_term_rows  = 0;

// Fixed-bottom frame state.
bool                       g_status_bar_active = false;
std::string                g_status_bar_text;
// Number of rows reserved at the bottom of the terminal for the
// fixed frame:
//
//   row N-3  top separator  (─── between scroll region and input)
//   row N-2  input row      (persistent "> " prompt, drawn by libedit)
//   row N-1  bottom separator (─── between input and status)
//   row N    status content  (model · counts · Remote Control)
//
// The scroll region is rows 1..N-4. Chat history, spinner, and
// streamed responses all scroll within that region. libedit draws
// the prompt inside the fixed input row (N-2) which never scrolls.
constexpr int              kStatusBarRows      = 4;

extern "C" void sigwinch_handler(int) {
	g_term_dirty     = 1;
	g_resize_pending = 1;
}

void refresh_dims() {
	struct winsize ws{};
	if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0
		&& ws.ws_col > 0 && ws.ws_row > 0) {
		g_cached_term_cols = ws.ws_col;
		g_cached_term_rows = ws.ws_row;
	} else {
		if (g_cached_term_cols == 0) g_cached_term_cols = 80;
		if (g_cached_term_rows == 0) g_cached_term_rows = 24;
	}
	g_term_dirty = 0;
}

} // namespace

int TerminalWidth() {
	if (!isatty(fileno(stdout))) return 0;
	if (g_term_dirty) refresh_dims();
	return g_cached_term_cols;
}

int TerminalRows() {
	if (!isatty(fileno(stdout))) return 0;
	if (g_term_dirty) refresh_dims();
	return g_cached_term_rows;
}

int ConsumeResizePending() {
	const int v = g_resize_pending;
	g_resize_pending = 0;
	return v;
}

void InstallSigwinchHandler() {
	if (!isatty(fileno(stdout))) return;
	struct sigaction sa{};
	sa.sa_handler = sigwinch_handler;
	sigemptyset(&sa.sa_mask);
	// SA_RESTART so a SIGWINCH during curl_easy_perform or a blocking
	// read doesn't abort the syscall — we only want to mark the width
	// dirty, not interrupt in-flight work.
	sa.sa_flags   = SA_RESTART;
	sigaction(SIGWINCH, &sa, nullptr);
}

namespace {

// Build the ANSI sequences for drawing the fixed frame. Pulled out
// so both InstallStatusBar and RedrawStatusBar share the logic.
// Writes directly to the real terminal (bypasses the TurnOutputBuf
// interceptor) so status bar updates are visible during active turns.
// Four rows are drawn:
//   row N-3 : top separator   (─── between scroll chat and input)
//   row N-2 : input row       (blank here; libedit draws "> " on demand)
//   row N-1 : bottom separator(─── between input and status)
//   row N   : status text
void draw_fixed_frame(int rows, int cols, const std::string& status) {
	if (rows < kStatusBarRows + 1) return;

	std::string rule;
	rule.reserve(cols * 3);
	for (int i = 0; i < cols; ++i) rule += "\xE2\x94\x80"; // ─

	// Build into a string then DirectWrite so the interceptor does
	// not swallow status-bar updates into the pending buffer.
	std::string out;
	out.reserve(256 + cols * 6);

	out += "\x1b""7";                              // DECSC

	out += "\x1b[";
	out += std::to_string(rows - 3);
	out += ";1H\x1b[2K";
	out += Muted(rule);                            // row N-3 separator

	out += "\x1b[";
	out += std::to_string(rows - 2);
	out += ";1H\x1b[2K";                          // row N-2 input (blank)

	out += "\x1b[";
	out += std::to_string(rows - 1);
	out += ";1H\x1b[2K";
	out += Muted(rule);                            // row N-1 separator

	out += "\x1b[";
	out += std::to_string(rows);
	out += ";1H\x1b[2K";
	out += status;                                 // row N status

	out += "\x1b""8";                             // DECRC

	DirectWrite(out);
}

// Set DECSTBM scroll region to rows 1..(rows - kStatusBarRows)
// so the fixed rows stay outside the scroll area. Places the cursor
// at the bottom of the scroll region ready for chat output.
//
// Uses DirectWrite (not std::cout) so the DECSTBM escape reaches the
// terminal immediately — even when a turn is active and std::cout is
// redirected through the TurnOutputBuf interceptor.  Buffering a
// DECSTBM command would delay the scroll-region update by up to 16 ms
// and cause FlushTurnOutput() to write inside the wrong region during
// that window.
void apply_scroll_region(int rows) {
	if (rows < kStatusBarRows + 1) return;
	const int top    = 1;
	const int bottom = rows - kStatusBarRows;
	const std::string s =
		"\x1b[" + std::to_string(top) + ";" + std::to_string(bottom) + "r"
		+ "\x1b[" + std::to_string(bottom) + ";1H";
	DirectWrite(s);
}

} // namespace

void InstallStatusBar(const std::string& initial_status) {
	if (!isatty(fileno(stdout))) return;
	if (!g_color_enabled) return;

	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 2) return; // tiny terminal

	g_status_bar_active = true;
	g_status_bar_text   = initial_status;

	// Clear the screen before installing the fixed frame so that any
	// output from a previous session (e.g. an earlier ./build/claude
	// run in the same tmux pane or terminal window) doesn't bleed into
	// the new session's scroll region and show as phantom prompt lines.
	// \x1b[H positions cursor at row 1 col 1; \x1b[2J erases the
	// entire screen. Both happen before the DECSTBM scroll region is
	// set, so the erase covers the full terminal.
	std::cout << "\x1b[H\x1b[2J" << std::flush;

	apply_scroll_region(g_cached_term_rows);
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
	std::cout.flush();
}

void SetStatusBar(const std::string& status) {
	g_status_bar_text = status;
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
}

void RedrawStatusBar() {
	if (!g_status_bar_active) return;
	refresh_dims();

	// On resize the expanded input area is lost (terminal reflow).
	// Reset so geometry is consistent with a fresh single-row input.
	g_extra_input_rows = 0;
	g_completed_lines.clear();

	apply_scroll_region(g_cached_term_rows);
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);

	// After a resize the scroll-region bottom has moved.  Update the
	// turn-output position tracker so the next FlushTurnOutput() CUPs
	// to the new chat_bottom rather than a stale pre-resize row.
	// Without this, worker output lands at the old row (which may now
	// be inside the status bar) until the next turn starts fresh.
	if (g_turn_started) {
		const int chat_bottom =
			g_cached_term_rows > kStatusBarRows
			? g_cached_term_rows - kStatusBarRows
			: 1;
		g_turn_row2 = chat_bottom;
		g_turn_col  = 1;
	}
}

void TeardownStatusBar() {
	if (!g_status_bar_active) return;
	g_status_bar_active = false;

	const int rows = g_cached_term_rows > 0 ? g_cached_term_rows : 24;
	std::cout << "\x1b[r"                         // reset scroll region to full terminal
			  << "\x1b[" << (rows - 3) << ";1H"
			  << "\x1b[2K"                        // clear top separator row
			  << "\x1b[" << (rows - 2) << ";1H"
			  << "\x1b[2K"                        // clear input row
			  << "\x1b[" << (rows - 1) << ";1H"
			  << "\x1b[2K"                        // clear bottom separator row
			  << "\x1b[" << rows << ";1H"
			  << "\x1b[2K"                        // clear status row
			  << "\x1b[?25h"                      // restore cursor visibility
			  << "\n"                              // advance past the cleared area so
			                                      // the shell prompt starts on a fresh line
			  << std::flush;
}

void EmitChatRule() {
	// Emit a turn-separator rule into the scroll region. Always
	// position at the scroll-region bottom (N-4) first so the \n
	// triggers a DECSTBM scroll on every turn.
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	const int width = TerminalWidth();
	if (width <= 0) return;
	std::string rule;
	rule.reserve(width * 3);
	for (int i = 0; i < width; ++i) rule += "\xE2\x94\x80"; // ─

	if (g_status_bar_active) {
		if (g_term_dirty) refresh_dims();
		if (g_cached_term_rows >= kStatusBarRows + 1) {
			const int bottom = g_cached_term_rows - kStatusBarRows; // N-4
			std::cout << "\x1b[" << bottom << ";1H";
		}
	}
	std::cout << Dim(rule) << "\n" << std::flush;
}

void PositionCursorForInput() {
	// Move cursor to the fixed input row (N-2) so libedit can draw
	// the prompt there. This row never moves regardless of how many
	// continuation lines are in the expanded area above it.
	// Uses DirectWrite so it works even during an active turn.
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	const int input_row = g_cached_term_rows - 2; // N-2, always fixed
	DirectWrite("\x1b[" + std::to_string(input_row) + ";1H\x1b[2K");
}

void ClearInputRow() {
	// Erase the fixed input row (N-2).
	// Uses DirectWrite so it works even during an active turn.
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	const int input_row = g_cached_term_rows - 2; // N-2, always fixed
	DirectWrite("\x1b[" + std::to_string(input_row) + ";1H\x1b[2K");
}

void RepaintInputRow(const std::string& prompt) {
	// Write the prompt string directly onto the fixed input row (N-2).
	// Uses DirectWrite so it works even during an active turn.
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	const int input_row = g_cached_term_rows - 2; // N-2, always fixed
	DirectWrite("\x1b[" + std::to_string(input_row) + ";1H\x1b[2K" + prompt);
}

void PositionCursorForChat() {
	// Move cursor to the scroll-region bottom so spinner and response
	// output scrolls into chat history above the fixed frame.
	// The scroll-region bottom shrinks when the input area is expanded
	// (g_extra_input_rows > 0), so we compute it dynamically.
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	const int chat_bottom = g_cached_term_rows - kStatusBarRows - g_extra_input_rows;
	std::cout << "\x1b[" << chat_bottom << ";1H" << std::flush;
}

// Expand the input area upward by one row for a new continuation line.
// Called by repl::ReadMessage() after each soft-newline.
//
// The input row (N-2) stays FIXED — libedit always draws there.
// Each expansion shifts the top separator one row higher and inserts
// the just-completed line in the slot immediately above the input row.
//
// Layout with `extra` completed lines:
//   rows 1..(N-4-extra)       : scroll region  (shrinks upward)
//   row  (N-3-extra)          : top separator ───
//   rows (N-2-extra)..(N-3)   : completed lines (oldest→newest, top→bottom)
//   row  (N-2)                : active input row  ← libedit, never moves
//   row  (N-1)                : bottom separator
//   row  (N)                  : status bar
void ExpandInputArea(const std::string& completedLine) {
	if (!g_status_bar_active) return;
	if (!g_color_enabled) return;
	if (g_term_dirty) refresh_dims();
	const int rows = g_cached_term_rows;
	const int cols = g_cached_term_cols;
	if (rows < kStatusBarRows + 2) return; // too small to expand

	// Check there is room for one more row before committing.
	const int new_extra         = g_extra_input_rows + 1;
	const int new_chat_bottom   = rows - kStatusBarRows - new_extra;
	const int new_separator_row = new_chat_bottom + 1;

	if (new_chat_bottom < 1 || new_separator_row < 1) return; // no room

	// Store the completed line and commit the counter.
	g_completed_lines.push_back(completedLine);
	g_extra_input_rows = new_extra;

	const int input_row = rows - 2; // N-2, fixed

	// Build the horizontal rule.
	std::string rule;
	rule.reserve(cols * 3);
	for (int i = 0; i < cols; ++i) rule += "\xE2\x94\x80"; // ─

	std::string out;
	out.reserve(512 + cols * 6);
	out += "\x1b""7"; // DECSC

	// Repaint the entire completed-lines block from scratch.
	// First blank every row between the new separator and the input row,
	// then write the completed lines.  This handles the case where
	// a previous expansion left stale content in rows that are now
	// above the lines block.
	for (int r = new_separator_row + 1; r < input_row; ++r) {
		out += "\x1b[" + std::to_string(r) + ";1H\x1b[2K";
	}
	// Line i goes on row (new_separator_row + 1 + i).
	for (int i = 0; i < (int)g_completed_lines.size(); ++i) {
		const int r = new_separator_row + 1 + i;
		out += "\x1b[" + std::to_string(r) + ";1H\x1b[2K";
		out += "\x1b[2m\xC2\xB7 ";  // dim "· "
		out += g_completed_lines[i];
		out += "\x1b[0m";
	}

	// Draw the top separator at its new (higher) position.
	out += "\x1b[" + std::to_string(new_separator_row) + ";1H\x1b[2K";
	out += Muted(rule);

	// Clear the input row so libedit starts with a blank line.
	out += "\x1b[" + std::to_string(input_row) + ";1H\x1b[2K";

	// Shrink the scroll region.
	out += "\x1b[1;" + std::to_string(new_chat_bottom) + "r";

	out += "\x1b""8"; // DECRC
	DirectWrite(out);

	if (g_turn_started && g_turn_row2 > new_chat_bottom)
		g_turn_row2 = new_chat_bottom;
}

// Collapse the expanded input area back to the single default input row.
// Called by repl::ReadMessage() after the user sends (Enter) or cancels.
// Clears every expanded row, redraws the top separator at its canonical
// position (N-3), and restores the full DECSTBM scroll region.
void CollapseInputArea() {
	if (!g_status_bar_active) return;
	if (g_extra_input_rows == 0) return;
	if (g_term_dirty) refresh_dims();
	const int rows = g_cached_term_rows;
	const int cols = g_cached_term_cols;
	if (rows < kStatusBarRows + 1) {
		g_extra_input_rows = 0;
		return;
	}

	const int prev_extra              = g_extra_input_rows;
	g_extra_input_rows                = 0;
	g_completed_lines.clear();

	const int canonical_chat_bottom   = rows - kStatusBarRows;   // N-4
	const int canonical_separator_row = rows - 3;
	const int canonical_input_row     = rows - 2;

	std::string rule;
	rule.reserve(cols * 3);
	for (int i = 0; i < cols; ++i) rule += "\xE2\x94\x80"; // ─

	std::string out;
	out.reserve(256 + cols * 6);

	out += "\x1b""7"; // DECSC

	// Wipe every row in the expanded input area.
	for (int r = canonical_separator_row - prev_extra; r <= canonical_input_row; ++r) {
		out += "\x1b[" + std::to_string(r) + ";1H\x1b[2K";
	}

	// Redraw the canonical top separator and blank the input row.
	out += "\x1b[" + std::to_string(canonical_separator_row) + ";1H\x1b[2K";
	out += Muted(rule);
	out += "\x1b[" + std::to_string(canonical_input_row) + ";1H\x1b[2K";

	// Restore the full scroll region.
	out += "\x1b[1;" + std::to_string(canonical_chat_bottom) + "r";

	out += "\x1b""8"; // DECRC

	DirectWrite(out);
}

void HideCursor() {
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	DirectWrite("\x1b[?25l");
}

void ShowCursor() {
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	DirectWrite("\x1b[?25h");
}

void PauseFlushTimer() {
	if (!g_flush_timer_running.load()) return;
	g_flush_timer_paused.store(true);
	// Wait until the timer loop acknowledges the pause so we know it
	// is no longer mid-FlushTurnOutput() when SelectOption() starts.
	// Worst-case wait: one 16 ms sleep + one 10 ms spin cycle ≈ 30 ms.
	for (int i = 0; i < 50 && !g_flush_timer_paused_ack.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	// Flush any pending turn output so it appears above the menu.
	// FlushTurnOutput wraps output in DECSC/CUP/DECRC; after it returns
	// the physical cursor is back at the libedit input row (DECRC).
	FlushTurnOutput();

	// Bypass the interceptor so SelectOption()'s std::cout writes go
	// directly to the terminal.  SuspendScrollRegion() will reset the
	// DECSTBM region and position the cursor correctly; here we only
	// swap the streambuf.
	if (g_cout_orig_buf) {
		std::cout.rdbuf(g_cout_orig_buf);
	}
}

void ResumeFlushTimer() {
	// RestoreScrollRegion() has already re-established DECSTBM and
	// redrawn the status bar.  Here we only need to reconnect the
	// turn-output interceptor and resume the flush timer.
	if (g_cout_orig_buf && g_status_bar_active) {
		if (g_term_dirty) refresh_dims();
		const int rows        = TerminalRows();
		const int chat_bottom = rows > kStatusBarRows ? rows - kStatusBarRows : 1;
		const int input_row   = rows > 2 ? rows - 2 : rows;

		// Update the output-position tracker to chat_bottom so the next
		// FlushTurnOutput() CUPs there rather than a stale pre-menu position.
		g_turn_row2    = chat_bottom;
		g_turn_col     = 1;
		g_turn_started = true;

		// Park the physical cursor at the input row so DECSC inside
		// FlushTurnOutput() saves N-2 and DECRC restores there.
		const std::string cup = "\x1b[" + std::to_string(input_row) + ";1H";
		g_cout_orig_buf->sputn(cup.data(), static_cast<std::streamsize>(cup.size()));
		g_cout_orig_buf->pubsync();
	}

	// Reconnect the interceptor so worker output is buffered again.
	if (g_cout_orig_buf)
		std::cout.rdbuf(&g_turn_buf);
	g_flush_timer_paused_ack.store(false);
	g_flush_timer_paused.store(false);
}

// SuspendScrollRegion / RestoreScrollRegion
//
// Park the cursor at the scroll-region bottom (chat_bottom = N-4) before
// a permission menu renders, and redraw the fixed frame afterward.
// SuspendScrollRegion() does NOT reset the scroll region to full-screen;
// keeping the restricted region (1..chat_bottom) ensures SelectOption()'s
// opening \n emissions scroll content within the chat area rather than
// pushing into the status-bar rows (N-3..N) and overwriting the "> " prompt.
//
// Both functions are safe no-ops when no status bar is installed.
//
// Call order:
//   PauseFlushTimer()        — stop concurrent output (parks cursor at N-2)
//   SuspendScrollRegion()    — CUP to chat_bottom (N-4)
//   ... SelectOption() ...
//   RestoreScrollRegion()    — re-establish \x1b[1;chat_bottom r + redraw status bar
//   ResumeFlushTimer()       — re-enable turn output
void SuspendScrollRegion() {
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	const int rows        = g_cached_term_rows;
	const int chat_bottom = rows > kStatusBarRows ? rows - kStatusBarRows : 1;
	// Park the cursor at chat_bottom (N-4, the bottom of the restricted
	// scroll region) so SelectOption()'s opening \n emissions scroll
	// content within the chat area and the menu renders entirely above
	// the "> " input line.
	//
	// We deliberately do NOT reset to full-screen (\x1b[r) here: with
	// the scroll region restricted to 1..chat_bottom, \n at chat_bottom
	// scrolls correctly.  Resetting to full-screen would allow \n from
	// chat_bottom to push into the status-bar rows and overwrite the
	// "> " prompt.
	//
	// Use DirectWrite so the CUP reaches the terminal even while the
	// turn-output interceptor may still be draining.
	const std::string seq =
		"\x1b[" + std::to_string(chat_bottom) + ";1H"; // CUP to scroll-region bottom
	DirectWrite(seq);
}

void RestoreScrollRegion() {
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	const int rows        = g_cached_term_rows;
	const int chat_bottom = rows > kStatusBarRows ? rows - kStatusBarRows : 1;
	const int input_row   = rows > 2 ? rows - 2 : rows;

	// Re-establish the scroll region and park the cursor at the fixed
	// input row so any subsequent DECSC (inside FlushTurnOutput or
	// draw_fixed_frame) saves the correct row.
	std::string restore;
	restore += "\x1b[1;" + std::to_string(chat_bottom) + "r";
	restore += "\x1b[" + std::to_string(input_row) + ";1H";
	DirectWrite(restore);

	// Redraw the status bar rows (SelectOption's menu teardown erases
	// rows top-to-bottom and may have overwritten the separator/status rows).
	draw_fixed_frame(rows, g_cached_term_cols, g_status_bar_text);

	// Update turn-output position tracker so the next FlushTurnOutput
	// CUPs to chat_bottom rather than a stale pre-menu row.
	g_turn_row2    = chat_bottom;
	g_turn_col     = 1;
	g_turn_started = true;
}

// SelectOption — interactive single-keypress menu.
//
// SECURITY / AUTOMATION NOTE
// --------------------------
// The menu reads directly from the real tty fd returned by
// repl::RealTtyFd() rather than from STDIN_FILENO.  This is intentional:
//
//   • BlockStdin() redirects STDIN_FILENO to an empty pipe while the
//     menu is active so libedit cannot race with SelectOption() on the
//     same fd.  Using the dup()'d tty fd bypasses that pipe and reaches
//     the actual terminal device, giving the menu exclusive input access.
//
//   • As a consequence the menu is NOT reachable via tmux send-keys
//     automation.  tmux send-keys injects bytes into the terminal
//     emulator's input queue; by the time they arrive at the slave-pty
//     read end, BlockStdin() has already redirected fd 0 and SelectOption
//     is draining the real tty fd directly.  The timing window means the
//     injected bytes land in the pipe (and are discarded) rather than in
//     the SelectOption read().  The menu renders correctly and is visible
//     in the pane, but the keystroke is not consumed by it.
//
//   • This behaviour is by design — a non-interactive automation path
//     should never silently approve destructive tool calls on behalf of
//     the user.  The sanctioned workaround for fully-automated sessions
//     is `/ludicrous` mode, which bypasses permission prompts entirely
//     at the user's explicit opt-in.
//
// TL;DR: menu works correctly for human users; tmux send-keys won't
// reach it — use /ludicrous for scripted automation.
int SelectOption(const std::vector<std::string>& options,
				  const std::string& heading,
				  std::atomic<bool>* cancel,
				  int                pre_lines) {
	if (options.empty()) return 0;
	const int n = static_cast<int>(options.size());

	// Use the real tty fd for all tty reads and termios operations.
	// When BlockStdin() has redirected STDIN_FILENO to /dev/null,
	// repl::RealTtyFd() still points to the actual terminal.
	const int tty = (repl::RealTtyFd() >= 0) ? repl::RealTtyFd() : fileno(stdin);

	// Non-TTY fallback: print numbered list and read a line.
	if (!g_color_enabled || !isatty(fileno(stdout)) || !isatty(tty)) {
		if (!heading.empty()) std::cout << heading << "\n";
		for (int i = 0; i < n; ++i) {
			std::cout << "  " << (i + 1) << ". " << options[i] << "\n";
		}
		std::cout << "choice [1-" << n << "]: " << std::flush;
		std::string line;
		if (!std::getline(std::cin, line) || line.empty()) return n - 1;
		char* end = nullptr;
		const long choice = std::strtol(line.c_str(), &end, 10);
		if (choice >= 1 && choice <= n) return static_cast<int>(choice) - 1;
		return n - 1;
	}

	// Put the tty into raw mode for single-keypress reads.
	struct termios orig {}, raw {};
	tcgetattr(tty, &orig);
	raw = orig;
	raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
	raw.c_cc[VMIN]  = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(tty, TCSANOW, &raw);

	int sel = 0; // 0-based selected index

	// Total rows owned by this widget: 1 heading row (if any) + n option rows
	// + 1 footer row ("Esc to cancel · Tab to amend").
	const int heading_rows = heading.empty() ? 0 : 1;
	constexpr int kFooterRows = 1;
	const int menu_rows = heading_rows + n + kFooterRows;

	// Reserve space for the menu rows by emitting blank lines, then
	// cursor-up back to the start. This ensures the menu always fits
	// within the scroll region without displacing the preview above it.
	for (int i = 0; i < menu_rows; ++i) std::cout << "\n";
	std::cout << "\x1b[" << menu_rows << "A\r" << std::flush;

	// All renders use \x1b[1B\r to step between rows (no scrolling).
	// The space has already been reserved above.
	bool first_render = true;
	auto render = [&]() {
		std::cout << "\r";
		if (!heading.empty()) {
			if (first_render) {
				std::cout << "\x1b[2K" << Bold(heading);
				first_render = false;
			}
			std::cout << "\x1b[1B\r"; // advance past heading row
		}
		for (int i = 0; i < n; ++i) {
			std::cout << "\x1b[2K";
			const std::string num = std::to_string(i + 1) + ". ";
			if (i == sel)
				std::cout << " \xE2\x9D\xAF " << Bold(Cyan(num)) << Bold(options[i]);
			else
				std::cout << "   " << Dim(num) << Dim(options[i]);
			std::cout << "\x1b[1B\r";
		}
		// Footer hint row.
		std::cout << "\x1b[2K"
		          << " " << Dim("Tab: amend \xC2\xB7 Shift+Tab: allow all \xC2\xB7 Esc: cancel");
		// Return cursor to heading row.
		const int rows_up = heading_rows + n + kFooterRows - 1;
		if (rows_up > 0) std::cout << "\x1b[" << rows_up << "A";
		std::cout << "\r" << std::flush;
	};

	// If the cancel flag is already set before we even render (e.g. a
	// Telegram hook answered while we were setting up raw mode), skip
	// drawing the menu entirely and return immediately so no flash occurs.
	if (cancel && cancel->load()) {
		tcsetattr(tty, TCSAFLUSH, &orig);
		return -1;
	}

	// Hide the cursor while the menu is on screen to reduce visual noise.
	std::cout << "\x1b[?25l" << std::flush;

	render();

	// After the first render the cursor is at the heading row.  If the
	// terminal is small, printing the preview + heading + options may
	// have caused the DECSTBM scroll region to scroll, pushing some
	// pre_lines rows off the top.  In that case \x1b[nA is clamped to
	// row 1 and the teardown erase starts from the wrong position,
	// leaving stale option lines on screen.
	//
	// Safe cap: the scroll region occupies rows 1..(N-kStatusBarRows).
	// Inside that region the menu itself takes (menu_rows) lines.  Any
	// pre_lines content that was scrolled in from above the menu can
	// occupy at most (scroll_region_height - menu_rows) rows.  Cap
	// pre_lines to that value so the upward cursor move in teardown can
	// never overshoot the scroll-region top.
	//
	// Note: we previously queried the cursor row via DSR (\x1b[6n) to
	// get the exact row, but the CPR response bytes leaked into libedit's
	// input buffer on some terminals (particularly tmux), corrupting the
	// next prompt.  The conservative cap below is always correct.
	{
		const int rows = TerminalRows();
		if (rows > kStatusBarRows) {
			const int scroll_height = rows - kStatusBarRows;
			const int max_pre = scroll_height - menu_rows;
			if (max_pre < 0)
				pre_lines = 0;
			else if (pre_lines > max_pre)
				pre_lines = max_pre;
		}
	}

	// When a cancel flag is supplied, use VMIN=0/VTIME=1 so read()
	// returns after ~100 ms even with no keypress, letting us check
	// *cancel between reads.  Without a cancel flag keep VMIN=1 for
	// efficient blocking reads.
	if (cancel) {
		struct termios poll_raw = raw;
		poll_raw.c_cc[VMIN]  = 0;
		poll_raw.c_cc[VTIME] = 1; // 100 ms
		tcsetattr(tty, TCSANOW, &poll_raw);
	}

	int chosen = n - 1; // default: last option (deny)
	bool done  = false;
	while (!done) {
		// Check the cancel flag before each read attempt.
		if (cancel && cancel->load()) {
			std::cout << "\x1b[?25h" << std::flush; // restore cursor
			tcsetattr(tty, TCSAFLUSH, &orig);
			return -1;
		}
		unsigned char c = 0;
		if (read(tty, &c, 1) != 1) {
			// VMIN=0 timeout or EOF — loop to re-check cancel.
			continue;
		}

		if (c == 0x1b) {
			// Escape sequence or bare Esc.
			//
			// Use VMIN=1 VTIME=1 so read() blocks until a byte arrives
			// or 100 ms elapses.  VMIN=0 VTIME=1 can return immediately
			// even when bytes are already in the kernel buffer because
			// tcsetattr flushes pending settings first.
			struct termios nb = raw;
			nb.c_cc[VMIN]  = 1;
			nb.c_cc[VTIME] = 1; // 100 ms inter-byte timeout
			tcsetattr(tty, TCSANOW, &nb);

			// Read the byte after ESC.
			unsigned char intro = 0;
			const int r1 = read(tty, &intro, 1);

			// If it is '[' this is a CSI sequence.  Read bytes until the
			// CSI final byte (0x40–0x7E) so that multi-byte sequences
			// like CPR \x1b[1;1R are fully consumed and never left in
			// the tty buffer to corrupt libedit's input on return.
			std::string csi; // bytes after '['
			bool is_csi = (r1 == 1 && intro == '[');
			if (is_csi) {
				while (csi.size() < 16) {
					unsigned char b = 0;
					if (read(tty, &b, 1) != 1) break;
					csi += static_cast<char>(b);
					if (b >= 0x40 && b <= 0x7e) break; // final byte consumed
				}
			}

			// Restore the correct mode: poll mode if cancel is set,
			// blocking mode otherwise.
			if (cancel) {
				struct termios poll_raw = raw;
				poll_raw.c_cc[VMIN]  = 0;
				poll_raw.c_cc[VTIME] = 1;
				tcsetattr(tty, TCSANOW, &poll_raw);
			} else {
				tcsetattr(tty, TCSANOW, &raw);
			}

			if (r1 <= 0) {
				// Bare Esc → deny (last option).
				chosen = n - 1;
				done   = true;
			} else if (is_csi && !csi.empty()) {
				const unsigned char final_byte =
					static_cast<unsigned char>(csi.back());
				if (final_byte == 'A') { // Up arrow   \x1b[A
					if (sel > 0) --sel;
					render();
				} else if (final_byte == 'B') { // Down arrow  \x1b[B
					if (sel < n - 1) ++sel;
					render();
				} else if (final_byte == 'Z') { // Shift+Tab   \x1b[Z
					if (n > 1) {
						sel    = 1;
						chosen = 1;
						render();
					} else {
						chosen = 0;
					}
					done = true;
				}
				// Any other CSI (CPR \x1b[r;cR, focus events, etc.)
				// has already been fully consumed above — silently ignore.
			}
		} else if (c == '\r' || c == '\n') {
			chosen = sel;
			done   = true;
		} else if (c == '\t') { // Tab → amend (cancel-and-retype sentinel)
			chosen = -2;
			done   = true;
		} else if (c >= '1' && c <= '9') {
			const int idx = static_cast<int>(c - '1');
			if (idx < n) {
				sel    = idx;
				chosen = idx;
				render();
				done   = true;
			}
		} else if (c == 3) { // Ctrl+C
			chosen = n - 1;
			done   = true;
		}
	}

	// Collapse the entire owned block (pre_lines + heading + all option rows
	// + footer row) down to a single compact summary line.
	const int block_rows = pre_lines + heading_rows + n + kFooterRows;

	// Step 1: move up pre_lines rows to reach the first row of the block.
	if (pre_lines > 0)
		std::cout << "\x1b[" << pre_lines << "A\r";

	// Step 2: erase all block rows from top to bottom (using \x1b[1B\r to
	// move down without triggering a DECSTBM scroll at the region boundary).
	for (int i = 0; i < block_rows; ++i) {
		std::cout << "\x1b[2K"; // erase this row
		if (i < block_rows - 1)
			std::cout << "\x1b[1B\r"; // move to next row (no scroll)
	}

	// Step 3: return to the first row of the block.
	if (block_rows > 1)
		std::cout << "\x1b[" << (block_rows - 1) << "A";
	std::cout << "\r";

	// Step 4: write the compact summary on the first (now blank) row.
	// chosen == -2 means Tab/amend: display a special label instead of
	// indexing options[] with a negative value (which would be UB).
	// Erase the line first as a safety measure before writing the summary.
	std::cout << "\x1b[2K";
	if (chosen == -2) {
		std::cout << Dim(heading.empty() ? "  -> [amend]" : heading + " \xe2\x86\x92 [amend]");
	} else if (!heading.empty()) {
		std::cout << Dim(heading + " \xe2\x86\x92 " + options[chosen]);
	} else {
		std::cout << Dim("  -> " + options[chosen]);
	}

	// Step 5: erase the rows that were scrolled in below the summary
	// by the opening \n emissions, then restore cursor to the summary
	// row so PositionCursorForChat() can reposition cleanly.
	for (int i = 0; i < menu_rows; ++i)
		std::cout << "\x1b[1B\r\x1b[2K";
	if (menu_rows > 0)
		std::cout << "\x1b[" << menu_rows << "A\r";

	std::cout << "\x1b[?25h" << std::flush; // restore cursor

	tcsetattr(tty, TCSAFLUSH, &orig);
	// Paranoid drain: read any bytes that survived TCSAFLUSH/tcflush.
	// Any echoed character from the drain might corrupt the summary
	// row, so re-erase and reprint the summary after the drain.
	{
		const int fd = tty;
		const int fl = fcntl(fd, F_GETFL);
		if (fl != -1) {
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
			char tmp[64];
			while (::read(fd, tmp, sizeof(tmp)) > 0) {}
			fcntl(fd, F_SETFL, fl);
		}
	}
	tcflush(tty, TCIFLUSH);
	// Re-erase and reprint the summary row now that the tty is back in
	// cooked mode and any race-echoed bytes have been drained.
	std::cout << "\r\x1b[2K";
	if (chosen == -2) {
		std::cout << Dim(heading.empty() ? "  -> [amend]" : heading + " \xe2\x86\x92 [amend]");
	} else if (!heading.empty()) {
		std::cout << Dim(heading + " \xe2\x86\x92 " + options[chosen]);
	} else {
		std::cout << Dim("  -> " + options[chosen]);
	}
	std::cout << std::flush;
	return chosen;
}

bool ColorEnabled() { return g_color_enabled; }

void SetColorEnabled(bool on) { g_color_enabled = on; }

std::string Bold(const std::string& s)    { return wrap("\x1b[1m",  s); }
std::string Dim(const std::string& s)     { return wrap("\x1b[2m",  s); }
std::string Italic(const std::string& s)  { return wrap("\x1b[3m",  s); }

std::string Red(const std::string& s)     { return wrap("\x1b[31m", s); }
std::string Green(const std::string& s)   { return wrap("\x1b[32m", s); }
std::string Yellow(const std::string& s)  { return wrap("\x1b[33m", s); }
std::string Blue(const std::string& s)    { return wrap("\x1b[34m", s); }
std::string Magenta(const std::string& s) { return wrap("\x1b[35m", s); }
std::string Cyan(const std::string& s)    { return wrap("\x1b[36m", s); }
std::string Gray(const std::string& s)    { return wrap("\x1b[90m", s); }
std::string Muted(const std::string& s)   { return wrap("\x1b[38;5;244m", s); }

std::string UserPrompt() {
	return wrap("\x1b[1;36m", "> ");
}

std::string ClaudePrompt() {
	// Prepend DECTCEM show-cursor (\e[?25h) so that printing the
	// prompt always restores cursor visibility — no matter which
	// code path arrives here after a Spinner or streaming output
	// may have hidden it with \e[?25l.  The escape is only emitted
	// when color/TTY mode is active (same guard as HideCursor /
	// ShowCursor), so non-TTY / pipe output is unaffected.
	const std::string show = g_color_enabled ? "\x1b[?25h" : "";
	return show + wrap("\x1b[1;35m", "claude> ");
}

std::string ContinuationPrompt() {
	return wrap("\x1b[2m", "... ");
}

std::string Meta(const std::string& s) {
	// Same 256-color gray-244 as Muted() so every piece of
	// secondary chrome (tool lines, turn counters, [resumed N],
	// permission-prompt labels) reads as one consistent shade.
	return wrap("\x1b[38;5;244m", s);
}

std::string ErrorLabel() {
	return wrap("\x1b[1;31m", "error:");
}

namespace {

struct LangSpec {
	const std::unordered_set<std::string>* keywords = nullptr;
	bool has_slash_comment = false;  // //
	bool has_hash_comment  = false;  // #
	bool has_block_comment = false;  // /* */
	bool has_single_string = false;  // '...'
	bool has_numbers       = true;
	bool has_preprocessor  = false;  // C/C++ #include etc.
};

const std::unordered_set<std::string>& cpp_keywords() {
	static const std::unordered_set<std::string> k = {
		"auto","break","case","catch","char","class","const","constexpr","continue",
		"default","delete","do","double","else","enum","explicit","extern","false",
		"float","for","friend","goto","if","inline","int","long","namespace","new",
		"noexcept","nullptr","operator","private","protected","public","return",
		"short","signed","sizeof","static","static_cast","struct","switch","template",
		"this","throw","true","try","typedef","typename","union","unsigned","using",
		"virtual","void","volatile","while","bool","wchar_t","char16_t","char32_t",
		"size_t","ssize_t","int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t",
		"uint32_t","uint64_t","std"
	};
	return k;
}

const std::unordered_set<std::string>& py_keywords() {
	static const std::unordered_set<std::string> k = {
		"False","None","True","and","as","assert","async","await","break","class",
		"continue","def","del","elif","else","except","finally","for","from","global",
		"if","import","in","is","lambda","nonlocal","not","or","pass","raise","return",
		"try","while","with","yield","self"
	};
	return k;
}

const std::unordered_set<std::string>& sh_keywords() {
	static const std::unordered_set<std::string> k = {
		"if","then","else","elif","fi","case","esac","for","while","do","done","in",
		"function","select","until","return","break","continue","local","export",
		"readonly","unset","set","shift","trap","true","false"
	};
	return k;
}

const std::unordered_set<std::string>& rust_keywords() {
	static const std::unordered_set<std::string> k = {
		"as","async","await","break","const","continue","crate","dyn","else","enum",
		"extern","false","fn","for","if","impl","in","let","loop","match","mod",
		"move","mut","pub","ref","return","self","Self","static","struct","super",
		"trait","true","type","unsafe","use","where","while"
	};
	return k;
}

const std::unordered_set<std::string>& json_keywords() {
	static const std::unordered_set<std::string> k = { "true","false","null" };
	return k;
}

bool lookup_lang(const std::string& lang, LangSpec& spec) {
	const std::string l = [&]{
		std::string s = lang;
		for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}();

	if (l == "cpp" || l == "c++" || l == "cxx" || l == "hpp" || l == "hxx" ||
		l == "c"   || l == "h") {
		spec.keywords        = &cpp_keywords();
		spec.has_slash_comment = true;
		spec.has_block_comment = true;
		spec.has_single_string = true;
		spec.has_preprocessor  = true;
		return true;
	}
	if (l == "py" || l == "python") {
		spec.keywords          = &py_keywords();
		spec.has_hash_comment  = true;
		spec.has_single_string = true;
		return true;
	}
	if (l == "sh" || l == "bash" || l == "zsh" || l == "shell") {
		spec.keywords          = &sh_keywords();
		spec.has_hash_comment  = true;
		spec.has_single_string = true;
		spec.has_numbers       = false;
		return true;
	}
	if (l == "rust" || l == "rs") {
		spec.keywords          = &rust_keywords();
		spec.has_slash_comment = true;
		spec.has_block_comment = true;
		return true;
	}
	if (l == "json") {
		spec.keywords = &json_keywords();
		return true;
	}
	return false;
}

std::string highlight_with(const LangSpec& spec, const std::string& line) {
	// C/C++ preprocessor: whole line if the first non-ws char is #.
	if (spec.has_preprocessor) {
		size_t k = 0;
		while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
		if (k < line.size() && line[k] == '#') {
			return std::string("\x1b[35m") + line + "\x1b[39m";
		}
	}

	std::string out;
	out.reserve(line.size() + 32);
	size_t i = 0;

	auto starts_at = [&](const char* lit) {
		const size_t n = std::char_traits<char>::length(lit);
		return i + n <= line.size() && line.compare(i, n, lit) == 0;
	};

	while (i < line.size()) {
		// Line comments
		if (spec.has_slash_comment && starts_at("//")) {
			out += "\x1b[2;90m";
			out += line.substr(i);
			out += "\x1b[0m";
			break;
		}
		if (spec.has_hash_comment && line[i] == '#') {
			out += "\x1b[2;90m";
			out += line.substr(i);
			out += "\x1b[0m";
			break;
		}
		// Block comment opener — color to end of line (line-local).
		if (spec.has_block_comment && starts_at("/*")) {
			out += "\x1b[2;90m";
			const size_t close = line.find("*/", i + 2);
			if (close == std::string::npos) {
				out += line.substr(i);
				out += "\x1b[0m";
				break;
			}
			out += line.substr(i, close + 2 - i);
			out += "\x1b[0m";
			i = close + 2;
			continue;
		}
		// Double-quoted string
		if (line[i] == '"') {
			const size_t start = i++;
			while (i < line.size() && line[i] != '"') {
				if (line[i] == '\\' && i + 1 < line.size()) ++i;
				++i;
			}
			if (i < line.size()) ++i;
			out += "\x1b[32m";
			out += line.substr(start, i - start);
			out += "\x1b[39m";
			continue;
		}
		// Single-quoted string / char literal
		if (spec.has_single_string && line[i] == '\'') {
			const size_t start = i++;
			while (i < line.size() && line[i] != '\'') {
				if (line[i] == '\\' && i + 1 < line.size()) ++i;
				++i;
			}
			if (i < line.size()) ++i;
			out += "\x1b[32m";
			out += line.substr(start, i - start);
			out += "\x1b[39m";
			continue;
		}
		// Numbers
		if (spec.has_numbers && std::isdigit(static_cast<unsigned char>(line[i]))) {
			const size_t start = i;
			while (i < line.size()
				   && (std::isalnum(static_cast<unsigned char>(line[i]))
					   || line[i] == '.' || line[i] == '_')) ++i;
			out += "\x1b[36m";
			out += line.substr(start, i - start);
			out += "\x1b[39m";
			continue;
		}
		// Identifiers → keywords
		if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
			const size_t start = i;
			while (i < line.size()
				   && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) ++i;
			const std::string word = line.substr(start, i - start);
			if (spec.keywords && spec.keywords->count(word)) {
				out += "\x1b[1;35m";
				out += word;
				out += "\x1b[22;39m";
			} else {
				out += word;
			}
			continue;
		}
		out += line[i++];
	}
	return out;
}

std::string highlight_code(const std::string& lang, const std::string& line) {
	LangSpec spec;
	if (!lookup_lang(lang, spec)) {
		// Unknown or unspecified language — keep T3's dim green tint.
		return "\x1b[32m" + line + "\x1b[0m";
	}
	return highlight_with(spec, line);
}

} // namespace

namespace {

// Count display columns in a string that may contain ANSI SGR
// escapes and UTF-8 multi-byte sequences. Escape sequences (0x1b
// up to 'm') are treated as zero width; every other UTF-8 lead
// byte counts as one column. Good enough for the table column
// width math since cells are short and the markdown content is
// mostly ASCII or narrow symbols.
int display_width(const std::string& s) {
	int cols = 0;
	bool in_esc = false;
	for (size_t i = 0; i < s.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (in_esc) {
			if (c == 'm') in_esc = false;
			continue;
		}
		if (c == 0x1b) { in_esc = true; continue; }
		if (c < 0x80) { ++cols; continue; }
		++cols;
		if      ((c & 0xE0) == 0xC0) i += 1;
		else if ((c & 0xF0) == 0xE0) i += 2;
		else if ((c & 0xF8) == 0xF0) i += 3;
	}
	return cols;
}

} // namespace (anonymous)

// Public wrapper so external translation units can call
// tui::DisplayWidth() without duplicating the ANSI-skip logic.
int DisplayWidth(const std::string& s) {
	return display_width(s);
}

// Map a file-path extension to a canonical language tag understood
// by lookup_lang() / HighlightCode(). Returns empty string for
// unrecognised extensions.
std::string LangFromPath(const std::string& path) {
	const auto dot = path.rfind('.');
	if (dot == std::string::npos) return {};
	std::string ext = path.substr(dot + 1);
	for (auto& c : ext)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (ext == "cpp" || ext == "cxx" || ext == "cc"  ||
	    ext == "c"   || ext == "h"   || ext == "hpp" ||
	    ext == "hxx" || ext == "hh")          return "cpp";
	if (ext == "py")                           return "python";
	if (ext == "sh"  || ext == "bash" || ext == "zsh") return "bash";
	if (ext == "rs")                           return "rust";
	if (ext == "json")                         return "json";
	if (ext == "js"  || ext == "ts"   ||
	    ext == "jsx" || ext == "tsx")          return "js";
	if (ext == "go")                           return "go";
	if (ext == "rb")                           return "ruby";
	if (ext == "java")                         return "java";
	if (ext == "kt"  || ext == "kts")         return "kotlin";
	if (ext == "swift")                        return "swift";
	if (ext == "md"  || ext == "markdown")    return "markdown";
	if (ext == "toml" || ext == "ini" ||
	    ext == "cfg"  || ext == "conf")        return "toml";
	if (ext == "xml" || ext == "html" ||
	    ext == "htm" || ext == "svg")          return "xml";
	if (ext == "css" || ext == "scss" ||
	    ext == "sass")                         return "css";
	if (ext == "yaml" || ext == "yml")         return "yaml";
	if (ext == "lua")                          return "lua";
	if (ext == "zig")                          return "zig";
	return {};
}

// Public wrapper: apply syntax highlighting to a single source line.
// Falls back to plain text when color is disabled or the language tag
// is not in the built-in registry.
std::string HighlightCode(const std::string& lang, const std::string& line) {
	if (!g_color_enabled || lang.empty()) return line;
	return highlight_code(lang, line);
}

// Render a diff row with full-width background colour, matching
// Claude Code's style exactly:
//
//   DiffRemoved: dark-red bg rgb(61,1,0)
//     - marker portion: muted-red fg rgb(220,90,90)
//     - content portion: near-white fg rgb(248,248,242)  ← vivid/readable
//
//   DiffAdded: dark-green bg rgb(2,40,0)
//     - entire row: muted-green fg rgb(80,200,80)
//
// marker: the " N - " or " N + " prefix (line number + diff char)
// content: the source code text after the marker
// \x1b[K fills the background to the terminal right edge.
std::string DiffRemoved(const std::string& marker, const std::string& content) {
	if (!g_color_enabled) return marker + content;
	// Set dark-red bg first, then muted-red fg for the marker,
	// then near-white fg for the content, then fill+reset.
	return "\x1b[48;2;61;1;0m"
	       "\x1b[38;2;220;90;90m" + marker +
	       "\x1b[38;2;248;248;242m" + content +
	       "\x1b[K\x1b[39m\x1b[49m";
}

std::string DiffAdded(const std::string& marker, const std::string& content) {
	if (!g_color_enabled) return marker + content;
	// Dark-green bg, muted-green fg for the entire row.
	return "\x1b[48;2;2;40;0m"
	       "\x1b[38;2;80;200;80m" + marker + content +
	       "\x1b[K\x1b[39m\x1b[49m";
}

namespace {

// Strip the leading and trailing `|` from a table row, then split
// on the remaining pipes. Cell text is trimmed of whitespace at
// both ends. Escaped pipes (`\|`) are not handled — extremely rare
// in Claude's output.
std::vector<std::string> split_table_row(const std::string& line) {
	std::vector<std::string> out;
	size_t                   start = 0;
	size_t                   end   = line.size();
	while (start < end && (line[start] == ' ' || line[start] == '\t')) ++start;
	if (start < end && line[start] == '|') ++start;
	while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
	if (end > start && line[end - 1] == '|') --end;

	std::string cell;
	for (size_t i = start; i < end; ++i) {
		if (line[i] == '|') {
			size_t a = 0, b = cell.size();
			while (a < b && (cell[a] == ' ' || cell[a] == '\t')) ++a;
			while (b > a && (cell[b - 1] == ' ' || cell[b - 1] == '\t')) --b;
			out.emplace_back(cell.substr(a, b - a));
			cell.clear();
		} else {
			cell += line[i];
		}
	}
	size_t a = 0, b = cell.size();
	while (a < b && (cell[a] == ' ' || cell[a] == '\t')) ++a;
	while (b > a && (cell[b - 1] == ' ' || cell[b - 1] == '\t')) --b;
	out.emplace_back(cell.substr(a, b - a));
	return out;
}

// A line qualifies as a table row if the first non-whitespace
// character is `|` and there's at least one more `|` on the line.
// This is a loose check — false positives on literal `|...|`
// inline code are theoretically possible but never seen in
// practice since the renderer wraps code in `` ticks `` first.
bool is_table_row(const std::string& line) {
	size_t i = 0;
	while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
	if (i >= line.size() || line[i] != '|') return false;
	int pipe_count = 0;
	for (; i < line.size(); ++i) if (line[i] == '|') ++pipe_count;
	return pipe_count >= 2;
}

// A separator row consists only of `|`, `-`, `:`, and whitespace,
// with at least one `-` per cell. Parses alignment markers:
// `:---` = Left, `---:` = Right, `:---:` = Center.
bool is_table_separator(const std::string& line,
						std::vector<TableAlign>* out_aligns) {
	const auto cells = split_table_row(line);
	if (cells.empty()) return false;
	std::vector<TableAlign> aligns;
	for (const auto& cell : cells) {
		if (cell.empty()) return false;
		bool has_dash   = false;
		bool leading_c  = cell.front() == ':';
		bool trailing_c = cell.back()  == ':';
		for (char c : cell) {
			if (c == '-')      has_dash = true;
			else if (c == ':') continue;
			else if (c == ' ' || c == '\t') continue;
			else return false;
		}
		if (!has_dash) return false;
		if (leading_c && trailing_c)      aligns.push_back(TableAlign::Center);
		else if (trailing_c)              aligns.push_back(TableAlign::Right);
		else                              aligns.push_back(TableAlign::Left);
	}
	if (out_aligns) *out_aligns = std::move(aligns);
	return true;
}

// Pad `cell` (which may contain ANSI escapes) to `target_width`
// display columns on the chosen side. Padding uses spaces only.
std::string pad_cell(const std::string& cell, int target_width,
					 TableAlign align) {
	const int cur = display_width(cell);
	if (cur >= target_width) return cell;
	const int pad = target_width - cur;
	switch (align) {
		case TableAlign::Left:
			return cell + std::string(pad, ' ');
		case TableAlign::Right:
			return std::string(pad, ' ') + cell;
		case TableAlign::Center: {
			const int left = pad / 2;
			return std::string(left, ' ') + cell + std::string(pad - left, ' ');
		}
	}
	return cell;
}

} // namespace

MarkdownRenderer::MarkdownRenderer() = default;

void MarkdownRenderer::FlushPrefix() {
	if (fFirstOutputDone) return;
	fFirstOutputDone = true;
	if (fSpinner) {
		fSpinner->Stop();
		fSpinner = nullptr;
	}
	// Print the response prefix (e.g. "claude> ") immediately after
	// the spinner clears its line, so the label appears right before
	// the first streamed character — or before an empty response ends
	// — and is never overwritten by a spinner tick.
	if (!fResponsePrefix.empty()) {
		std::cout << fResponsePrefix << std::flush;
		fResponsePrefix.clear();
	}
}

void MarkdownRenderer::Emit(const std::string& s) {
	if (!fFirstOutputDone) {
		FlushPrefix();
	}
	std::cout << s << std::flush;
}

std::string MarkdownRenderer::RenderInlineToString(const std::string& text) {
	if (!g_color_enabled) return text;

	enum class Mode { Normal, Bold, Italic, Code };
	Mode mode = Mode::Normal;
	std::string out;
	out.reserve(text.size() + 32);

	auto ansi = [](const char* code) { return std::string(code); };
	auto starts_with_double_star = [&](size_t i) {
		return i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*';
	};

	for (size_t i = 0; i < text.size();) {
		const char c = text[i];
		if (mode == Mode::Normal) {
			if (starts_with_double_star(i)) {
				out += ansi("\x1b[1m");
				mode = Mode::Bold;
				i += 2;
			} else if (c == '*' || c == '_') {
				out += ansi("\x1b[3m");
				mode = Mode::Italic;
				i += 1;
			} else if (c == '`') {
				out += ansi("\x1b[1;36m");
				mode = Mode::Code;
				i += 1;
			} else {
				out += c;
				i += 1;
			}
		} else if (mode == Mode::Bold) {
			if (starts_with_double_star(i)) {
				out += ansi("\x1b[22m");
				mode = Mode::Normal;
				i += 2;
			} else {
				out += c;
				i += 1;
			}
		} else if (mode == Mode::Italic) {
			if (c == '*' || c == '_') {
				out += ansi("\x1b[23m");
				mode = Mode::Normal;
				i += 1;
			} else {
				out += c;
				i += 1;
			}
		} else { // Code
			if (c == '`') {
				out += ansi("\x1b[39m");
				mode = Mode::Normal;
				i += 1;
			} else {
				out += c;
				i += 1;
			}
		}
	}
	// Defensive reset in case a line ends mid-token.
	if (mode != Mode::Normal) out += ansi("\x1b[0m");
	return out;
}

void MarkdownRenderer::RenderInline(const std::string& text) {
	Emit(RenderInlineToString(text));
}

void MarkdownRenderer::FlushTable() {
	if (!fTableActive || fTableRows.empty()) {
		fTableRows.clear();
		fTableAligns.clear();
		fTableActive = false;
		return;
	}

	// Normalize column count: some rows may have fewer/more cells
	// than others. Pick the max and pad short rows with empty
	// strings so the width math doesn't crash.
	size_t ncols = 0;
	for (const auto& r : fTableRows) ncols = std::max(ncols, r.size());
	for (auto& r : fTableRows) r.resize(ncols);
	while (fTableAligns.size() < ncols) fTableAligns.push_back(TableAlign::Left);
	fTableAligns.resize(ncols);

	// Render each cell's inline markdown (bold/italic/code) first
	// so the width math sees the already-formatted string (ANSI
	// escapes are zero-width in display_width). First row is the
	// header — bold it.
	std::vector<std::vector<std::string>> rendered(fTableRows.size());
	for (size_t r = 0; r < fTableRows.size(); ++r) {
		rendered[r].resize(ncols);
		for (size_t c = 0; c < ncols; ++c) {
			std::string cell = RenderInlineToString(fTableRows[r][c]);
			if (r == 0 && g_color_enabled) {
				cell = "\x1b[1m" + cell + "\x1b[22m";
			}
			rendered[r][c] = std::move(cell);
		}
	}

	// Column widths: max display width across all cells in the
	// column, with a minimum of 1 to avoid zero-width separators.
	std::vector<int> widths(ncols, 1);
	for (const auto& row : rendered) {
		for (size_t c = 0; c < ncols; ++c) {
			widths[c] = std::max(widths[c], display_width(row[c]));
		}
	}

	// Emit top border, header, separator, body rows, bottom border
	// using light box-drawing. Format:
	//   ┌───┬───┐
	//   │ H │ H │
	//   ├───┼───┤
	//   │ c │ c │
	//   └───┴───┘
	auto draw_border = [&](const char* left, const char* mid, const char* right) {
		std::string out = Dim(left);
		for (size_t c = 0; c < ncols; ++c) {
			std::string dashes;
			for (int i = 0; i < widths[c] + 2; ++i) dashes += "\xE2\x94\x80"; // ─
			out += Dim(dashes);
			out += Dim(c + 1 == ncols ? right : mid);
		}
		out += "\n";
		Emit(out);
	};

	auto draw_row = [&](const std::vector<std::string>& row) {
		std::string out = Dim("\xE2\x94\x82"); // │
		for (size_t c = 0; c < ncols; ++c) {
			out += " ";
			out += pad_cell(row[c], widths[c], fTableAligns[c]);
			out += " ";
			out += Dim("\xE2\x94\x82");
		}
		out += "\n";
		Emit(out);
	};

	draw_border("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90"); // ┌ ┬ ┐
	draw_row(rendered[0]);
	draw_border("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4"); // ├ ┼ ┤
	for (size_t r = 1; r < rendered.size(); ++r) draw_row(rendered[r]);
	draw_border("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98"); // └ ┴ ┘

	fTableRows.clear();
	fTableAligns.clear();
	fTableActive = false;
}

void MarkdownRenderer::RenderLine(const std::string& line) {
	// Inside a code block, highlight per recognized language until we
	// see the closing fence.
	if (fInCodeBlock) {
		if (line.size() >= 3 && line.substr(0, 3) == "```") {
			fInCodeBlock   = false;
			fCodeBlockLang.clear();
			Emit(Dim("```") + "\n");
			return;
		}
		Emit(highlight_code(fCodeBlockLang, line) + "\n");
		return;
	}

	// Opening code fence. Tables end at any non-table line — flush
	// the buffer first so a fenced code block can't land inside an
	// unclosed table.
	if (line.size() >= 3 && line.substr(0, 3) == "```") {
		if (fTableActive) FlushTable();
		fInCodeBlock   = true;
		fCodeBlockLang = line.substr(3);
		if (fCodeBlockLang.empty()) {
			Emit(Dim("```") + "\n");
		} else {
			Emit(Dim("``` " + fCodeBlockLang) + "\n");
		}
		return;
	}

	// Table row handling. Buffer rows until we see a non-table
	// line, then flush with computed column widths. The second
	// row (index 1) is treated as the alignment separator if it
	// looks like one — otherwise it's a normal body row.
	if (is_table_row(line)) {
		if (!fTableActive) {
			fTableActive = true;
			fTableRows.push_back(split_table_row(line));
			return;
		}
		if (fTableRows.size() == 1) {
			std::vector<TableAlign> aligns;
			if (is_table_separator(line, &aligns)) {
				fTableAligns = std::move(aligns);
				return;
			}
		}
		fTableRows.push_back(split_table_row(line));
		return;
	}
	if (fTableActive) {
		FlushTable();
		// Fall through so the current (non-table) line still
		// renders normally.
	}

	// Headings.
	size_t hash_count = 0;
	while (hash_count < line.size() && line[hash_count] == '#') ++hash_count;
	if (hash_count > 0 && hash_count <= 3 && hash_count < line.size() && line[hash_count] == ' ') {
		const std::string rest = line.substr(hash_count + 1);
		const char* color = hash_count == 1 ? "\x1b[1;35m"
						  : hash_count == 2 ? "\x1b[1;34m"
						  :                    "\x1b[1;36m";
		Emit(std::string(color) + rest + "\x1b[0m\n");
		return;
	}

	// Bullet list: optional leading whitespace, then '- ' or '* '.
	size_t i = 0;
	while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
	if (i + 1 < line.size() && (line[i] == '-' || line[i] == '*') && line[i + 1] == ' ') {
		const std::string indent(i, ' ');
		Emit(indent + "\x1b[36m\u2022\x1b[0m ");
		RenderInline(line.substr(i + 2));
		Emit("\n");
		return;
	}

	// Numbered list: N. or N) at line start (optionally indented).
	{
		size_t j = i;
		while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
		if (j > i && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
			Emit(std::string(i, ' ') + "\x1b[36m" + line.substr(i, j - i + 1) + "\x1b[0m ");
			RenderInline(line.substr(j + 2));
			Emit("\n");
			return;
		}
	}

	// Regular paragraph line.
	RenderInline(line);
	Emit("\n");
}

void MarkdownRenderer::Write(const std::string& chunk) {
	if (!g_color_enabled) {
		if (!fFirstOutputDone) {
			FlushPrefix();
		}
		std::cout << chunk << std::flush;
		return;
	}

	for (char c : chunk) {
		if (c == '\n') {
			RenderLine(fLineBuffer);
			fLineBuffer.clear();
		} else {
			fLineBuffer += c;
		}
	}
}

void MarkdownRenderer::Flush() {
	// Emit the prefix (and stop the spinner) even if no text ever
	// arrived — e.g. a tool-only turn where the model's follow-up
	// response contains zero text_delta events.
	FlushPrefix();
	if (!fLineBuffer.empty()) {
		RenderLine(fLineBuffer);
		fLineBuffer.clear();
	}
	if (fTableActive) FlushTable();
}

namespace {

// Claude Code-style gerund labels. One is picked at Spinner
// construction time so each request gets a different vibe, matching
// the "Forming…", "Misting…", "Pondering…" style of the upstream CLI.
const char* kSpinnerVerbs[] = {
	"Thinking",  "Forming",   "Pondering", "Musing",
	"Brewing",   "Weaving",   "Crafting",  "Conjuring",
	"Distilling","Scheming",  "Plotting",  "Sifting",
	"Unraveling","Cooking",   "Stewing",   "Mulling",
	"Simmering", "Reckoning", "Percolating","Chewing",
};
constexpr int kVerbCount = sizeof(kSpinnerVerbs) / sizeof(kSpinnerVerbs[0]);

// Spinner frames — braille dots give a smooth 8-step rotation and are
// unambiguously 1 column wide in every terminal including Haiku Terminal.
// The U+2736–U+273D star glyphs look great but their rendered pixels
// bleed beyond the terminal cell boundary in Haiku's default font,
// causing the space printed in the next cell to clip the right side of
// the glyph.  Braille block (U+2800) glyphs are designed for terminal
// use and sit cleanly within their cell.
const char* kSpinnerGlyphs[] = {
	"\xE2\xA3\xBE",  // ⣾ U+28FE
	"\xE2\xA3\xBD",  // ⣽ U+28FD
	"\xE2\xA3\xBB",  // ⣻ U+28FB
	"\xE2\xA2\xBF",  // ⢿ U+28BF
	"\xE2\xA1\xBF",  // ⡿ U+287F
	"\xE2\xA0\xBF",  // ⠿ U+283F
	"\xE2\xA2\xAF",  // ⢯ U+28AF
	"\xE2\xA3\xB7",  // ⣷ U+28F7
};
constexpr int kGlyphCount = sizeof(kSpinnerGlyphs) / sizeof(kSpinnerGlyphs[0]);
constexpr int kGlyphCols  = 1; // braille glyphs are unambiguously 1-column wide

// Format `N seconds` as either `Xs` for short waits or `Xm Ys`
// for long ones, matching Claude Code's compact time rendering.
std::string format_elapsed(double seconds) {
	const int total = static_cast<int>(seconds);
	if (total < 60) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%ds", total);
		return buf;
	}
	const int m = total / 60;
	const int s = total % 60;
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%dm %ds", m, s);
	return buf;
}

// Simple xorshift-based random index so we don't need <random> just
// to pick a verb. Good enough — only called once per Spinner.
int pick_verb_index() {
	// Seed from a per-process steady_clock tick count so consecutive
	// requests don't always pick the same verb when built in the
	// same second.
	static std::atomic<uint32_t> state {
		static_cast<uint32_t>(
			std::chrono::steady_clock::now().time_since_epoch().count())
	};
	uint32_t x = state.load(std::memory_order_relaxed);
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	state.store(x, std::memory_order_relaxed);
	return static_cast<int>(x % kVerbCount);
}

} // namespace

Spinner::Spinner(std::string label) : fLabel(std::move(label)) {
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	fActive = true;
	fThread = std::thread(&Spinner::Run, this);
}

Spinner::~Spinner() {
	Stop();
}

void Spinner::Stop() {
	bool expected = false;
	if (!fStopping.compare_exchange_strong(expected, true)) return;
	fCv.notify_all();
	if (fThread.joinable()) fThread.join();
}

void Spinner::Run() {
	// Hide the cursor for the duration of the spinner so it doesn't
	// jump around the scroll region with every \r frame redraw.
	// The ShowGuard below restores it on every exit path, including
	// exceptions thrown from string allocation / stream I/O /
	// fCv.wait_for inside the render loop. Without the guard, an
	// exception would unwind out of the thread entry point and
	// std::terminate without running the trailing ShowCursor(),
	// leaving the user's terminal with a hidden cursor until reset.
	HideCursor();
	struct ShowGuard {
		~ShowGuard() {
			// Mirror the normal teardown: clear the spinner line so
			// the next writer starts at column 0, then restore the
			// cursor. Runs exactly once per Spinner lifetime thanks
			// to RAII, regardless of how run() exits.
			std::cout << "\r\x1b[2K" << std::flush;
			ShowCursor();
		}
	} show_guard;

	// Pick a verb once per Spinner lifetime. The incoming fLabel is
	// ignored in favor of the randomized gerund — callers used to
	// pass "thinking" but the richer rendering now wants a gerund
	// ending in -ing with no extra chrome. If fLabel happens to
	// already be a gerund (e.g., set explicitly), we could honor it,
	// but the simpler path is to always randomize here.
	const char* const verb = kSpinnerVerbs[pick_verb_index()];

	int glyph_idx = 0;
	int frame_count = 0;

	const auto start = std::chrono::steady_clock::now();

	while (!fStopping.load()) {
		const double elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - start).count();

		// Live input token count, if a pointer was wired up via
		// SetLiveInputTokens(). Cleared to 0 initially; the
		// caller writes to the atomic as soon as `message_start`
		// arrives over SSE, so there's a brief window (a few
		// hundred ms to a few seconds) where this jumps from 0 to
		// the real count and the spinner picks it up on the next
		// frame.
		int live_in = 0;
		if (fLiveInputTokens) {
			live_in = fLiveInputTokens->load(std::memory_order_relaxed);
		}

		// Build the tail block inside parens, matching Claude
		// Code's `(44s · ↑ 652 tokens · esc:cancel)` style. The
		// up-arrow indicates tokens we've sent to the model (the
		// prompt size) — we don't have live output tokens during
		// the thinking window since the spinner dies the moment
		// the first text_delta arrives via MarkdownRenderer.
		std::string tail = "(" + format_elapsed(elapsed);
		if (live_in > 0) {
			tail += " \xC2\xB7 \xE2\x86\x91 " + std::to_string(live_in)
				 +  " tokens";
		}
		tail += " \xC2\xB7 esc:cancel)";

		// Pulse the verb between normal and slightly-faint on a
		// ~1 Hz cycle so it reads as "breathing" instead of steady.
		// Each frame is ~80 ms; every 6 frames (≈ 480 ms) we flip
		// the pulse state. Rainbow hue cycles independently per
		// frame; combined effect is a shimmer that reads alive.
		const bool verb_bright = ((frame_count / 6) & 1) == 0;

		const std::string glyph = kSpinnerGlyphs[glyph_idx];
		const std::string verb_str = std::string(verb) + "\xE2\x80\xA6"; // …

		// 256-color rainbow palette. Glyph and verb cycle through
		// it per frame with a small phase offset so they don't
		// shift in lock-step — looks more organic. Muted palette
		// (not pure primaries) to stay readable on both dark and
		// light themes.
		static constexpr int kRainbow[] = {
			203, 209, 215, 221, 186, 151, 115,
			 79,  75,  68,  97, 133, 169, 205,
		};
		constexpr int kRainbowCount = sizeof(kRainbow) / sizeof(kRainbow[0]);
		const int glyph_col = kRainbow[frame_count % kRainbowCount];
		const int verb_col  = kRainbow[(frame_count + 4) % kRainbowCount];

		char glyph_wrap[16];
		char verb_wrap[16];
		std::snprintf(glyph_wrap, sizeof(glyph_wrap), "\x1b[38;5;%dm", glyph_col);
		std::snprintf(verb_wrap,  sizeof(verb_wrap),  "\x1b[38;5;%dm", verb_col);

		// Final render: rainbow glyph + rainbow verb + muted tail.
		// Tail stays gray (consistent with the rest of the frame
		// chrome) so the animated region is visually isolated.
		std::string frame;
		frame.reserve(128);
		frame += glyph_wrap;
		frame += glyph;
		frame += "\x1b[0m "; // 1 space — braille glyphs are cleanly 1-col wide
		frame += verb_wrap;
		if (!verb_bright) frame += "\x1b[2m"; // stack faint for the pulse dip
		frame += verb_str;
		frame += "\x1b[0m  ";
		frame += Muted(tail);

		// Truncate the frame so it never exceeds TerminalWidth() visible
		// columns and cannot wrap.  Strategy:
		//   • Walk the frame counting visible columns, skipping ANSI CSI
		//     escape sequences (which are zero-width).  UTF-8 multi-byte
		//     sequences are counted as 1 column each (all of our spinner
		//     glyphs and Latin text satisfy this).
		//   • If the frame fits within `width` columns → render as-is.
		//   • If the frame exceeds `width` columns → cut to `width - 1`
		//     visible columns, close any open colour escape, then append
		//     `…` (1 col).  Total rendered width is then exactly `width`.
		//
		// Two bugs fixed vs. the previous version:
		//   1. `budget = width - 1` caused content to be cut one column
		//      too early: a frame of exactly `width` columns was truncated
		//      and had its last character replaced by `…`, making the
		//      spinner appear clipped when it would have fit.
		//   2. The CSI final-byte check only exited `in_esc` on 'm'.
		//      The standard range is 0x40–0x7E; using only 'm' meant any
		//      future sequence ending differently would be mis-counted.
		const int width = TerminalWidth();
		if (width > 4) {
			int    display_cols  = 0;
			bool   in_esc        = false;
			// Byte index of the start of the last character that fit.
			// Initialised to 0; only valid when display_cols > 0.
			size_t last_char_start = 0;
			size_t cut             = std::string::npos; // set when truncation needed
			bool   needs_cut       = false;

			for (size_t i = 0; i < frame.size(); ) {
				const unsigned char c = static_cast<unsigned char>(frame[i]);
				if (in_esc) {
					// Standard CSI final byte range 0x40–0x7E (includes 'm',
					// 'K', 'H', 'J', 'A'–'D', …).
					if (c >= 0x40 && c <= 0x7E) in_esc = false;
					++i;
					continue;
				}
				if (c == 0x1b) { in_esc = true; ++i; continue; }

				// Byte-length of this UTF-8 code point.
				int char_bytes = 1;
				if      ((c & 0xE0) == 0xC0) char_bytes = 2;
				else if ((c & 0xF0) == 0xE0) char_bytes = 3;
				else if ((c & 0xF8) == 0xF0) char_bytes = 4;
				// The spinner glyph (first visible char, 3-byte UTF-8) is
				// 1 column wide — braille glyphs fit cleanly in their cell.
				const int char_cols = 1;

				if (display_cols + char_cols > width) {
					// This character would push the line past the terminal
					// edge.  Back up: cut just before the previous
					// character (so the frame ends at width-1 cols) and
					// append … (1 col) → total exactly width.
					cut = last_char_start;
					needs_cut = true;
					break;
				}

				last_char_start = i;
				display_cols   += char_cols;
				i              += char_bytes;
			}

			if (needs_cut) {
				frame.resize(cut);
				// Close any open colour escape so … isn't rainbow-tinted.
				frame += "\x1b[0m";
				frame += "\xE2\x80\xA6"; // … (1 column)
			}
		}

		std::cout << "\r\x1b[2K" << frame << "\x1b[0m" << std::flush;

		glyph_idx = (glyph_idx + 1) % kGlyphCount;
		++frame_count;

		std::unique_lock<std::mutex> lock(fMutex);
		fCv.wait_for(lock, std::chrono::milliseconds(80),
					 [this] { return fStopping.load(); });
	}
	// ShowGuard's dtor clears the spinner line and restores the
	// cursor on the way out — nothing else to do here.
}

} // namespace tui
