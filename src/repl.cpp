#include "repl.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <editline/readline.h>
#include "tui.h"

// Soft-newline: accept the current line with a trailing backslash so
// that ReadMessage()'s backslash-continuation logic re-prompts for the
// next line.  Using rl_insert_text("\n") doesn't work because libedit
// treats a \n in the edit buffer as "accept line", not a literal
// newline character — so pressing Ctrl+J or Alt+Enter would just submit
// the message instead of continuing it.  Appending '\\' and then
// calling rl_newline() (accept) achieves real multi-line input without
// any libedit internals hacks.
extern "C" {
static int soft_newline(int /*count*/, int /*key*/) {
	rl_insert_text("\\");   // append trailing backslash
	rl_newline(1, '\n');    // accept the line
	return 0;
}

// Bracketed-paste handler.  Registered as the action for \e[200~ so
// that when the terminal wraps a paste in the bracketed-paste markers
// (\e[200~ ... \e[201~) we receive the whole block atomically instead
// of just the first line.
//
// Strategy:
//   1. Read raw bytes from stdin until the closing marker \e[201~.
//   2. Split on \n.  Strip a trailing empty segment (paste that ended
//      with \n shouldn't add a spurious blank continuation line).
//   3. Transform into the soft-newline wire format: each line except
//      the last gets a trailing backslash appended, then a real \n.
//      This mirrors exactly what soft_newline() produces so
//      ReadMessage()'s backslash-continuation loop assembles the
//      lines correctly without any changes to that function.
//   4. Stuff the transformed bytes into libedit's input queue via
//      rl_stuff_char().  rl_stuff_char() is LIFO, so we push in
//      reverse byte order so the queue drains left-to-right.
//
// Terminal support: \e[?2004h is sent once during init().  Terminals
// that don't support bracketed paste simply ignore that escape and
// never send \e[200~, so users on those terminals experience no
// change from today's behaviour (first-line-only paste).
static int handle_bracketed_paste(int /*count*/, int /*key*/) {
	// Read stdin until we see the closing marker \e[201~ or EOF.
	// We use a simple state machine rather than a circular buffer so
	// that partial marker matches that turn out to be false alarms are
	// correctly re-emitted into `paste`.
	static const char kEnd[] = "\x1b[201~";
	constexpr int kEndLen    = sizeof(kEnd) - 1; // 6 bytes

	std::string paste;
	paste.reserve(256);
	int matched = 0;

	while (matched < kEndLen) {
		char c;
		if (::read(fileno(stdin), &c, 1) <= 0) break; // EOF / error

		if (c == kEnd[matched]) {
			++matched;
		} else {
			// False alarm: flush the partially matched prefix, then
			// re-process `c` from the start of the marker.
			paste.append(kEnd, matched);
			matched = 0;
			if (c == kEnd[0]) {
				matched = 1;
			} else {
				paste += c;
			}
		}
	}

	if (paste.empty()) return 0;

	// Remove a trailing \n — a paste that ends with a newline would
	// otherwise produce a spurious blank continuation line.
	if (!paste.empty() && paste.back() == '\n') paste.pop_back();

	if (paste.empty()) return 0;

	// Build the wire string: each internal \n becomes \\\n so that
	// ReadMessage()'s backslash-continuation loop sees one logical
	// line per readline() call and assembles them in order.
	std::string wire;
	wire.reserve(paste.size() + 16);
	for (char ch : paste) {
		if (ch == '\n') {
			wire += "\\\n"; // backslash-continuation marker
		} else {
			wire += ch;
		}
	}
	// The final line needs no trailing backslash — it terminates
	// the message normally.

	// rl_stuff_char() is LIFO: push in reverse so the queue is
	// consumed left-to-right by readline().
	for (int i = static_cast<int>(wire.size()) - 1; i >= 0; --i) {
		rl_stuff_char(static_cast<unsigned char>(wire[i]));
	}

	// Return without inserting anything into the current edit buffer —
	// readline will re-enter its read loop and drain the stuffed bytes.
	return 0;
}
} // extern "C"

namespace repl {
namespace {

std::string              g_history_file;
std::vector<std::string> g_slash_commands;

void ensure_parent_dir(const std::string& path) {
	const auto slash = path.rfind('/');
	if (slash == std::string::npos) return;
	const std::string dir = path.substr(0, slash);
	std::string accum;
	for (size_t i = 0; i < dir.size(); ++i) {
		accum += dir[i];
		const bool boundary = (dir[i] == '/') || (i + 1 == dir.size());
		if (!boundary) continue;
		if (accum.empty() || accum == "/") continue;
		if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return;
	}
}

// Wrap ANSI escape sequences in \001..\002 so libedit knows to skip
// them when counting visible column width.
std::string wrap_for_readline(const std::string& prompt) {
	std::string out;
	out.reserve(prompt.size() + 16);
	size_t i = 0;
	while (i < prompt.size()) {
		if (prompt[i] == '\x1b' && i + 1 < prompt.size() && prompt[i + 1] == '[') {
			const size_t start = i;
			i += 2;
			while (i < prompt.size() && prompt[i] != 'm' && prompt[i] != 'K') ++i;
			if (i < prompt.size()) ++i; // consume terminator
			out.push_back('\001');
			out.append(prompt.substr(start, i - start));
			out.push_back('\002');
		} else {
			out.push_back(prompt[i++]);
		}
	}
	return out;
}

extern "C" char** slash_completion(const char* text, int start, int /*end*/) {
	// Only complete when the cursor is at the start of the line AND
	// the current word begins with '/'. Otherwise let the default
	// filename completion run so argument completion still works.
	if (start != 0) return nullptr;
	if (!text || text[0] != '/') return nullptr;

	const std::string prefix = text;
	std::vector<std::string> matches;
	for (const auto& cmd : g_slash_commands) {
		if (cmd.size() >= prefix.size()
			&& cmd.compare(0, prefix.size(), prefix) == 0) {
			matches.push_back(cmd);
		}
	}
	if (matches.empty()) return nullptr;

	// The libedit/readline convention: element 0 is the longest
	// common prefix (used as the "replacement" when there's more
	// than one match), elements 1..N are the actual candidates, and
	// the array is NULL-terminated.
	std::string lcp = matches[0];
	for (size_t i = 1; i < matches.size(); ++i) {
		size_t j = 0;
		while (j < lcp.size() && j < matches[i].size() && lcp[j] == matches[i][j]) ++j;
		lcp.resize(j);
	}

	char** result = static_cast<char**>(std::malloc((matches.size() + 2) * sizeof(char*)));
	if (!result) return nullptr;
	result[0] = strdup(lcp.c_str());
	for (size_t i = 0; i < matches.size(); ++i) {
		result[i + 1] = strdup(matches[i].c_str());
	}
	result[matches.size() + 1] = nullptr;
	return result;
}

} // namespace

void Init(const std::string& history_file) {
	g_history_file = history_file;
	if (!g_history_file.empty()) {
		ensure_parent_dir(g_history_file);
		read_history(g_history_file.c_str());
	}
	rl_attempted_completion_function = slash_completion;

	// Ctrl+J (0x0A) → soft newline: accept line with trailing '\' so
	// ReadMessage() re-prompts via backslash-continuation.
	//
	// rl_bind_key('\n', ...) is unreliable in libedit's readline compat
	// layer because 0x0A is hardwired as "accept-line" in libedit's
	// internal keymap before the compat shim can intercept it.
	// rl_set_key() with the explicit byte string is the correct API.
	rl_add_defun("soft-newline", soft_newline, -1);
	rl_set_key("\x0a", soft_newline, rl_get_keymap());  // Ctrl+J

	// Alt+Enter (ESC \r in most terminals) → same action.
	// emacs_meta_keymap lives at index 0x0D ('\r').
	rl_bind_key_in_map('\r', soft_newline, emacs_meta_keymap);

	// Override libedit's word-break character set so only
	// whitespace breaks words. libedit's default set includes
	// `-`, `.`, and a handful of other punctuation marks, which
	// means typing `/remote-control` and hitting Tab caused the
	// completer to see just `control` (after the `-`) as the
	// current word, compute a non-zero `start` position, and
	// fall through to default filename completion. Whitespace-
	// only breaks keep `/remote-control`, `/foo.bar`, and any
	// other hyphen/dot-containing slash command working as a
	// single word for completion purposes.
	static const char* kWordBreaks = " \t\n";
	rl_completer_word_break_characters = const_cast<char*>(kWordBreaks);
	rl_basic_word_break_characters     = const_cast<char*>(kWordBreaks);

	// Bracketed paste mode.  Sending \e[?2004h asks the terminal to
	// wrap each paste event in \e[200~ ... \e[201~.  We bind the
	// opening marker to handle_bracketed_paste(), which reads stdin
	// until the closing marker, transforms internal newlines into
	// backslash-continuation sequences, and stuffs the result back
	// into libedit's input queue.  ReadMessage()'s existing
	// backslash-continuation loop then assembles the lines normally.
	//
	// Terminals that do not support bracketed paste simply ignore
	// \e[?2004h and never emit \e[200~, so this binding is never
	// triggered and behaviour is unchanged from today.
	//
	// Only enable on TTY stdout — in piped / non-interactive sessions
	// there is no terminal to send the enable sequence to, and
	// bracketed paste is meaningless there.
	if (isatty(fileno(stdin))) {
		rl_add_defun("handle-bracketed-paste", handle_bracketed_paste, -1);
		rl_set_key("\x1b[200~", handle_bracketed_paste, rl_get_keymap());
		// Tell the terminal to start sending bracketed-paste markers.
		// Write directly to the terminal fd so the escape doesn't get
		// buffered behind other stdout content.
		::write(fileno(stdout), "\x1b[?2004h", 8);
	}
}

void Deinit() {
	// Disable bracketed paste mode so the terminal is left clean.
	// Mirror the isatty() guard from init() — no-op on non-TTY.
	if (isatty(fileno(stdin))) {
		::write(fileno(stdout), "\x1b[?2004l", 8);
	}
}

void SetSlashCommands(const std::vector<std::string>& names) {
	g_slash_commands = names;
	std::sort(g_slash_commands.begin(), g_slash_commands.end());
}

bool ReadLine(const std::string& prompt, std::string& out) {
	const std::string wrapped = wrap_for_readline(prompt);
	char* line = readline(wrapped.c_str());
	if (!line) return false;
	out.assign(line);
	std::free(line);
	return true;
}

bool ReadMessage(const std::string& prompt,
				  const std::string& ContinuationPrompt,
				  std::string&       out) {
	out.clear();

	std::string first;
	if (!ReadLine(prompt, first)) return false;

	// Fenced block: bare `"""` or `'''` on its own.
	if (first == "\"\"\"" || first == "'''") {
		const std::string fence = first;
		while (true) {
			std::string next;
			// Park the cursor back in the fixed input row (N-2) so
			// libedit draws the continuation prompt there, not in the
			// scroll region above it.
			tui::PositionCursorForInput();
			if (!ReadLine(ContinuationPrompt, next)) {
				return !out.empty();
			}
			if (next == fence) return true;
			if (!out.empty()) out.push_back('\n');
			out.append(next);
		}
	}

	// Backslash continuation.
	if (!first.empty() && first.back() == '\\') {
		first.pop_back();
		out.append(first);
		out.push_back('\n');
		while (true) {
			std::string next;
			// Park the cursor back in the fixed input row (N-2) so
			// libedit draws the continuation prompt there, not in the
			// scroll region above it.
			tui::PositionCursorForInput();
			if (!ReadLine(ContinuationPrompt, next)) {
				return true;
			}
			if (!next.empty() && next.back() == '\\') {
				next.pop_back();
				out.append(next);
				out.push_back('\n');
				continue;
			}
			out.append(next);
			return true;
		}
	}

	out = std::move(first);
	return true;
}

void Record(const std::string& line) {
	if (line.empty()) return;
	add_history(line.c_str());
	if (!g_history_file.empty()) {
		write_history(g_history_file.c_str());
	}
}

} // namespace repl
