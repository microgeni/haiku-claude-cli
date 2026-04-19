#include "tui.h"

#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <unordered_set>

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

	if (const char* v = std::getenv("NO_COLOR"); v && *v) return false;

	if (const char* v = std::getenv("CLICOLOR"); v && std::string(v) == "0") return false;

	if (const char* v = std::getenv("TERM"); v) {
		if (std::string(v) == "dumb") return false;
	}
	return true;
}

} // namespace

void Init() {
	g_color_enabled = detect_color_support();
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
//   row N-3  rule above input (dimmed ─)
//   row N-2  input prompt row (libedit draws here)
//   row N-1  rule below input (dimmed ─)
//   row N    status content (model · counts · Remote Control)
//
// The scroll region is the complement: rows 1..N-4. All chat
// history and assistant streaming output flows there, with the
// bottom of the region being row N-4 — immediately above the
// fixed rule row.
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
// Caller is responsible for flushing stdout after calling.
void draw_fixed_frame(int rows, int cols, const std::string& status) {
	if (rows < kStatusBarRows + 1) return;

	// Save cursor, then walk the four fixed rows. DECSC / DECRC
	// (\e7 / \e8) are more reliable across xterm and Haiku
	// Terminal than CSI s/u.
	std::cout << "\x1b""7";

	std::string rule;
	rule.reserve(cols * 3);
	for (int i = 0; i < cols; ++i) rule += "\xE2\x94\x80"; // ─

	// Row N-3: rule above the input row.
	std::cout << "\x1b[" << (rows - 3) << ";1H"
			  << "\x1b[2K"
			  << Muted(rule);

	// Row N-2: input row. Clear any leftover content so a stale
	// prompt doesn't bleed into the next turn; we don't draw
	// anything here — libedit owns this row when ReadMessage
	// runs, and PositionCursorForInput parks the cursor at
	// column 1.
	std::cout << "\x1b[" << (rows - 2) << ";1H"
			  << "\x1b[2K";

	// Row N-1: rule below the input row.
	std::cout << "\x1b[" << (rows - 1) << ";1H"
			  << "\x1b[2K"
			  << Muted(rule);

	// Row N: status content. Truncated to cols by the caller.
	std::cout << "\x1b[" << rows << ";1H"
			  << "\x1b[2K"
			  << status;

	std::cout << "\x1b""8";
}

// Set DECSTBM scroll region to rows 1..(rows - kStatusBarRows)
// so the bottom four rows stay fixed, and place the cursor at
// the bottom of the scroll region so subsequent output lands in
// the chat history area.
void apply_scroll_region(int rows) {
	if (rows < kStatusBarRows + 1) return;
	const int top    = 1;
	const int bottom = rows - kStatusBarRows;
	std::cout << "\x1b[" << top << ";" << bottom << "r"
			  << "\x1b[" << bottom << ";1H";
}

} // namespace

void InstallStatusBar(const std::string& initial_status) {
	if (!isatty(fileno(stdout))) return;
	if (!g_color_enabled) return;

	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 2) return; // tiny terminal

	g_status_bar_active = true;
	g_status_bar_text   = initial_status;

	apply_scroll_region(g_cached_term_rows);
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
	std::cout.flush();
}

void SetStatusBar(const std::string& status) {
	g_status_bar_text = status;
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
	std::cout.flush();
}

void RedrawStatusBar() {
	if (!g_status_bar_active) return;
	refresh_dims();
	apply_scroll_region(g_cached_term_rows);
	draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
	std::cout.flush();
}

void TeardownStatusBar() {
	if (!g_status_bar_active) return;
	g_status_bar_active = false;

	// Restore the full scroll region and clear our fixed rows, then
	// move the cursor below them so anything the caller (or the
	// shell) prints next doesn't overwrite the stale footer. Also
	// make sure the cursor is visible again in case we exited mid-
	// stream with the cursor hidden.
	const int rows = g_cached_term_rows > 0 ? g_cached_term_rows : 24;
	std::cout << "\x1b[r"                         // reset scroll region
			  << "\x1b[" << (rows - 3) << ";1H"
			  << "\x1b[2K"                        // clear rule-above
			  << "\x1b[" << (rows - 2) << ";1H"
			  << "\x1b[2K"                        // clear input row
			  << "\x1b[" << (rows - 1) << ";1H"
			  << "\x1b[2K"                        // clear rule-below
			  << "\x1b[" << rows << ";1H"
			  << "\x1b[2K"                        // clear status row
			  << "\x1b[" << rows << ";1H"
			  << "\x1b[?25h"                      // show cursor (safety)
			  << std::flush;
}

void EmitChatRule() {
	// No-op when the fixed-bottom frame is active — the rule row
	// already lives at row N-3 and is kept fresh by redraws, so
	// emitting an in-chat rule would duplicate it into the
	// scrolling history.
	if (g_status_bar_active) return;
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	const int width = TerminalWidth();
	if (width <= 0) return;
	std::string rule;
	rule.reserve(width * 3);
	for (int i = 0; i < width; ++i) rule += "\xE2\x94\x80"; // ─
	std::cout << Dim(rule) << "\n" << std::flush;
}

void PositionCursorForInput() {
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	std::cout << "\x1b[" << (g_cached_term_rows - 2) << ";1H"
			  << "\x1b[2K"
			  << std::flush;
}

void ClearInputRow() {
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	// Erase the input row so the submitted text doesn't linger
	// for the duration of the turn, then park the cursor at the
	// bottom of the chat scroll region ready for spinner/output.
	const int input_row = g_cached_term_rows - 2;
	const int chat_row  = g_cached_term_rows - kStatusBarRows;
	std::cout << "\x1b[" << input_row << ";1H"  // move to input row
			  << "\x1b[2K"                        // erase entire line
			  << "\x1b[" << chat_row  << ";1H"   // park in chat area
			  << std::flush;
}

void PositionCursorForChat() {
	if (!g_status_bar_active) return;
	if (g_term_dirty) refresh_dims();
	if (g_cached_term_rows < kStatusBarRows + 1) return;
	const int bottom = g_cached_term_rows - kStatusBarRows;
	std::cout << "\x1b[" << bottom << ";1H" << std::flush;
}

void HideCursor() {
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	std::cout << "\x1b[?25l" << std::flush;
}

void ShowCursor() {
	if (!g_color_enabled) return;
	if (!isatty(fileno(stdout))) return;
	std::cout << "\x1b[?25h" << std::flush;
}

int SelectOption(const std::vector<std::string>& options,
				  const std::string& heading) {
	if (options.empty()) return 0;
	const int n = static_cast<int>(options.size());

	// Non-TTY fallback: print numbered list and read a line.
	if (!g_color_enabled || !isatty(fileno(stdout)) || !isatty(fileno(stdin))) {
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

	// Put stdin into raw mode for single-keypress reads.
	struct termios orig {}, raw {};
	tcgetattr(fileno(stdin), &orig);
	raw = orig;
	raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
	raw.c_cc[VMIN]  = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(fileno(stdin), TCSANOW, &raw);

	int sel = 0; // 0-based selected index

	// Total rows owned by this widget: 1 heading row (if any) + n option rows.
	// We track this so the teardown erase loop knows how far up to reach.
	const int heading_rows = heading.empty() ? 0 : 1;
	const int total_rows   = heading_rows + n;

	// Render the menu. Each option takes one line. We'll use ANSI
	// cursor-up to redraw in-place on each keystroke.
	//
	// On the very first call we also print the heading (if any) above
	// the options.  Subsequent renders only redraw the option rows —
	// the heading row stays fixed in scroll history until teardown.
	bool first_render = true;
	auto render = [&]() {
		// Always start from column 0 so erased+reprinted text aligns
		// correctly regardless of where the previous render left the cursor.
		std::cout << "\r";
		if (first_render && !heading.empty()) {
			std::cout << "\x1b[2K" << Bold(heading) << "\x1b[1B\r";
			first_render = false;
		}
		for (int i = 0; i < n; ++i) {
			std::cout << "\x1b[2K"; // erase line
			const std::string num = std::to_string(i + 1) + ". ";
			if (i == sel) {
				// Highlighted: bold + cyan number, bold label, then reset.
				std::cout << "  " << Bold(Cyan(num)) << Bold(options[i]);
			} else {
				std::cout << "  " << Dim(num) << Dim(options[i]);
			}
			if (i < n - 1) {
				// Use \x1b[1B\r (cursor-down + CR) instead of \n.
				// The cursor starts at the bottom of the DECSTBM scroll
				// region; emitting \n there causes the terminal to scroll
				// the region up, shifting the menu's absolute row on every
				// redraw and breaking highlight updates on arrow keypresses.
				std::cout << "\x1b[1B\r";
			}
		}
		// Move cursor back to the first option line so the next render
		// overwrites from the same position.  When a heading is present
		// the first option is one row below it, so add heading_rows to
		// the cursor-up count.  \r resets column to 0.
		const int rows_up = (n - 1) + (first_render ? 0 : heading_rows);
		if (rows_up > 0) std::cout << "\x1b[" << rows_up << "A";
		std::cout << "\r" << std::flush;
	};

	render();

	int chosen = n - 1; // default: last option (deny)
	bool done  = false;
	while (!done) {
		unsigned char c = 0;
		if (read(fileno(stdin), &c, 1) != 1) break;

		if (c == 0x1b) {
			// Escape sequence or bare Esc.
			unsigned char seq[2] = {};
			// Use VMIN=1 VTIME=1: block until a byte arrives or 100 ms
			// elapses.  VMIN=0 VTIME=1 is a polling read — on a real
			// terminal it can return 0 immediately even when the rest of
			// the CSI sequence ([ A/B) is already in the kernel buffer,
			// because tcsetattr flushes the old settings before the bytes
			// land.  VMIN=1 guarantees we wait for the byte.
			struct termios nb = raw;
			nb.c_cc[VMIN]  = 1;
			nb.c_cc[VTIME] = 1; // 100 ms inter-byte timeout
			tcsetattr(fileno(stdin), TCSANOW, &nb);
			const int r1 = read(fileno(stdin), &seq[0], 1);
			const int r2 = (r1 == 1 && seq[0] == '[')
						 ? read(fileno(stdin), &seq[1], 1) : 0;
			tcsetattr(fileno(stdin), TCSANOW, &raw);

			if (r1 <= 0) {
				// Bare Esc → deny (last option).
				chosen = n - 1;
				done   = true;
			} else if (r1 == 1 && seq[0] == '[' && r2 == 1) {
				if (seq[1] == 'A') { // Up arrow
					if (sel > 0) --sel;
					render();
				} else if (seq[1] == 'B') { // Down arrow
					if (sel < n - 1) ++sel;
					render();
				}
			}
		} else if (c == '\r' || c == '\n') {
			chosen = sel;
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

	// Erase the entire owned block (heading + all option rows) and
	// replace it with a single compact summary line so scroll history
	// stays informative without the full menu cluttering it.
	//
	// Cursor is currently at the first option row (render() left it
	// there).  We need to go up by heading_rows more to reach the very
	// top of the block, then erase downward, then print the summary.
	if (heading_rows > 0)
		std::cout << "\x1b[" << heading_rows << "A"; // up to heading row
	// Erase each owned row from top to bottom using cursor-down between
	// them (same \x1b[1B\r trick as render() to avoid DECSTBM scrolling).
	for (int i = 0; i < total_rows; ++i) {
		std::cout << "\x1b[2K"; // erase current line
		if (i < total_rows - 1) std::cout << "\x1b[1B\r";
	}
	// Back to the top of the block: move up (total_rows - 1) lines.
	if (total_rows > 1) std::cout << "\x1b[" << (total_rows - 1) << "A";
	std::cout << "\r";
	// Print summary: "heading → chosen label" (or just chosen label if
	// no heading was given, to keep the fallback behaviour the same).
	if (!heading.empty()) {
		std::cout << Dim(heading + " \xe2\x86\x92 " + options[chosen]);
	} else {
		std::cout << Dim("  -> " + options[chosen]);
	}
	std::cout << "\n" << std::flush;

	tcsetattr(fileno(stdin), TCSANOW, &orig);
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

void MarkdownRenderer::Emit(const std::string& s) {
	if (!fFirstOutputDone) {
		fFirstOutputDone = true;
		if (fSpinner) {
			fSpinner->Stop();
			fSpinner = nullptr;
		}
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
		std::cout << chunk << std::flush;
		if (!fFirstOutputDone) {
			fFirstOutputDone = true;
			if (fSpinner) {
				fSpinner->Stop();
				fSpinner = nullptr;
			}
		}
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
