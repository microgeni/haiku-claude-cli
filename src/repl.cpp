#include "repl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <editline/readline.h>
#include "paths.h"
#include "tui.h"

// ---------------------------------------------------------------------------
// Bracketed-paste support
//
// When the terminal has bracketed paste enabled (\e[?2004h) it wraps any
// paste with \e[200~ ... \e[201~.  libedit's internal trie sees the raw
// escape bytes and either inserts garbage characters or silently drops
// the sequence, so we intercept them ourselves in a custom rl_getc_function
// that sits between the terminal and libedit.
//
// Strategy:
//   • Normal characters are returned one-at-a-time as usual.
//   • When we see \e[200~ we enter "paste mode": we read the entire paste
//     body up to \e[201~ into g_paste_buf and set g_paste_pos = 0.
//   • While g_paste_pos < g_paste_buf.size() we return characters from
//     the buffer.  Newlines in the paste (\n or \r\n) are converted to
//     the two-character sequence '\\' '\n' so that libedit calls
//     soft_newline for each one, triggering backslash-continuation and
//     keeping the existing read_message() logic happy.
// ---------------------------------------------------------------------------

namespace {

std::string g_paste_buf;
size_t      g_paste_pos = 0;

// Self-pipe used to wake the blocking poll() in raw_getc_or_wake().
// g_wake_pipe[0] = read end (polled alongside stdin).
// g_wake_pipe[1] = write end (written by tui::WakeReadMessage()).
// Both set to -1 until Init() creates them.
int g_wake_pipe[2] = {-1, -1};

// When true, stdin fd (0) is redirected to a blocking pipe so libedit's
// internal read() calls block instead of consuming tty bytes while
// SelectOption() is running.
std::atomic<bool> g_stdin_blocked{false};

// File descriptors used for stdin blocking.
int g_real_tty_fd   = -1;           // dup() of the original stdin (the real tty)
int g_block_pipe[2] = {-1, -1};     // pipe: [0]=read end (used as fd 0 when blocking),
                                     //       [1]=write end (written to unblock libedit)

// Original terminal settings saved at Init() time — before libedit or
// EscInterruptGuard touch them.  Restored by Deinit() as a safety net
// so the shell always inherits a sane (cooked+echo) tty.
struct termios g_saved_termios {};
bool           g_saved_termios_valid = false;

// Read one raw byte from stdin, also watching g_wake_pipe[0].
// When the wake pipe fires, flush pending output and check
// g_turn_just_completed; if set and the edit buffer is empty,
// return '\r' to simulate Enter and unblock ReadMessage().
// Returns EOF on real EOF or unrecoverable error.
static int raw_getc_or_wake(FILE* f) {
    while (true) {
        const int stdin_fd   = fileno(f);
        const int wake_fd    = g_wake_pipe[0];

        if (wake_fd < 0) {
            // No wake pipe — fall back to plain fgetc.
            int c;
            do { c = fgetc(f); } while (c == EOF && errno == EINTR);
            return c;
        }

        // When stdin is blocked for SelectOption(), only poll the wake
        // pipe so we never race with SelectOption's read() on the same fd.
        const bool blocked = g_stdin_blocked.load();
        const int  nfds    = blocked ? 1 : 2;

        struct pollfd pfds[2];
        pfds[0].fd      = stdin_fd;
        pfds[0].events  = blocked ? 0 : POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd      = wake_fd;
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        const int r = ::poll(pfds + (blocked ? 1 : 0), nfds, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return EOF;
        }

        // Wake pipe fired — drain it, flush output, maybe inject '\r'.
        const struct pollfd& wake_pfd = pfds[1];
        if (wake_pfd.revents & (POLLIN | POLLHUP)) {
            char discard[64];
            ::read(wake_fd, discard, sizeof(discard));
            tui::FlushTurnOutput();
            if (tui::g_turn_just_completed.load() && rl_end == 0) {
                tui::g_turn_just_completed.store(false);
                return '\r';
            }
            tui::g_turn_just_completed.store(false);
            // POLLHUP without POLLIN means the write-end was closed — this
            // should never happen (parent always holds g_wake_pipe[1]) but
            // if it does, sleep briefly to avoid a tight spin loop before
            // Deinit() closes g_wake_pipe[0] and terminates readline().
            if (!(wake_pfd.revents & POLLIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // If stdin is also ready and not blocked, fall through to read it.
            if (blocked || !(pfds[0].revents & POLLIN)) continue;
        }

        // Stdin ready — read only when not blocked for SelectOption().
        if (!blocked && (pfds[0].revents & POLLIN)) {
            unsigned char buf;
            const ssize_t n = ::read(stdin_fd, &buf, 1);
            if (n == 1) return static_cast<int>(buf);
            if (n == 0) return EOF;
            if (errno == EINTR || errno == EAGAIN) continue;
            return EOF;
        }
    }
}

// Convenience wrapper keeping the old name for callers inside this file.
static int raw_getc(FILE* f) {
    return raw_getc_or_wake(f);
}

// Consume characters from `f` until the CSI terminator sequence `seq`
// has been fully matched or we time out / hit EOF.  Returns true if the
// sequence was consumed, false on EOF.  We read char-by-char matching
// seq[matched..]; unmatched characters are appended to `buf`.
static bool consume_until(FILE* f, const char* seq, std::string& buf) {
    const size_t seqlen = strlen(seq);
    size_t matched = 0;
    while (matched < seqlen) {
        const int c = raw_getc(f);
        if (c == EOF) return false;
        if (static_cast<char>(c) == seq[matched]) {
            ++matched;
        } else {
            // False start — flush partially matched prefix into buf.
            buf.append(seq, matched);
            matched = 0;
            buf.push_back(static_cast<char>(c));
            // Re-check this character against seq[0].
            if (static_cast<char>(c) == seq[0]) matched = 1;
        }
    }
    return true;
}

// Custom rl_getc_function: intercepts \e[200~ and replays the paste
// body with newlines converted to backslash-continuation sequences.
//
// Key design constraint: we must NOT speculatively over-read bytes from
// any ESC[ sequence that turns out not to be a bracketed-paste start.
// The old code read exactly 4 bytes after ESC[ and compared them to
// "200~".  This caused two problems:
//   1. Short sequences (e.g. \e[2h, 2 bytes after [) left the last
//      raw_getc() blocking on user input.
//   2. Long sequences (e.g. cursor-position reports \e[12;34R, 6 bytes
//      after [) had their trailing bytes permanently lost — they were
//      never stashed in g_paste_buf — which corrupted libedit's key
//      FSM and caused it to swallow the user's first Enter keypress.
//
// Fix: read one byte at a time, matching against "200~" as a prefix.
// Stop as soon as the accumulated bytes either:
//   (a) fully match "200~"                   → handle paste
//   (b) diverge from "200~" AND we hit a CSI → stash all bytes, return ESC
//       final byte (0x40–0x7E per ECMA-48)
//   (c) diverge from "200~" on a non-final   → keep reading until we
//       byte (e.g. '1' in a CPR \e[12;34R)     hit a final byte, then
//                                               stash everything & return ESC
//
// This ensures every byte of any CSI sequence that isn't \e[200~ is
// faithfully preserved in g_paste_buf and replayed to libedit.
extern "C" int bracketed_getc(FILE* f) {
    // If we have buffered paste characters, drain them first.
    if (g_paste_pos < g_paste_buf.size())
        return static_cast<unsigned char>(g_paste_buf[g_paste_pos++]);

    // Flush any pending worker output to the scroll region before
    // blocking on read. This is the safe window: libedit has finished
    // its previous redraw and will not touch the terminal until after
    // this read returns. DECSC/DECRC in FlushTurnOutput() preserves
    // libedit's cursor on the input row.
    tui::FlushTurnOutput();

    // If the flush timer detected the turn just completed, return a
    // synthetic '\r' (Enter) so ReadMessage() returns an empty line
    // and the main loop's drain_turn() fires — but only when the
    // edit buffer is empty (rl_end == 0) so we don't submit partial input.
    if (tui::g_turn_just_completed.load()) {
        if (rl_end == 0) {
            tui::g_turn_just_completed.store(false);
            return '\r';
        }
        // Edit buffer non-empty — drain_turn() will fire when the
        // user next presses Enter.
        tui::g_turn_just_completed.store(false);
    }

    const int c = raw_getc(f);
    if (c == EOF) return c;

    // Fast path: not the start of an escape sequence.
    if (c != '\x1b') return c;

    // Peek at the next byte.
    const int c2 = raw_getc(f);
    if (c2 == EOF) return '\x1b';  // lone ESC

    if (c2 != '[') {
        // Not a CSI sequence — return ESC now, stash c2 for next call.
        g_paste_buf.clear();
        g_paste_buf.push_back(static_cast<char>(c2));
        g_paste_pos = 0;
        return '\x1b';
    }

    // We have ESC '['.  Read bytes one at a time, accumulating into
    // `acc`, until we can determine whether this is \e[200~ or not.
    // "200~" is the bracketed-paste opener; anything else is a CSI
    // sequence we must replay verbatim (cursor-position reports,
    // terminal responses, etc.).
    static constexpr char kTarget[] = "200~";
    static constexpr int  kTargetLen = 4;

    std::string acc;
    acc.reserve(8);
    bool matched = false;

    while (true) {
        const int cx = raw_getc(f);
        if (cx == EOF) break;
        acc.push_back(static_cast<char>(cx));

        const int n = static_cast<int>(acc.size());

        // Check prefix match against "200~".
        if (n <= kTargetLen && strncmp(acc.data(), kTarget, n) == 0) {
            if (n == kTargetLen) {
                matched = true; // full match
                break;
            }
            // Still a prefix — keep reading.
            continue;
        }

        // Not a prefix of "200~".  Keep reading until we hit a CSI
        // final byte (0x40–0x7E) so the full sequence is captured.
        if (cx >= 0x40 && cx <= 0x7E) break;
    }

    if (matched) {
        // ── Bracketed paste start ──────────────────────────────────
        // Slurp everything up to \e[201~ into a temporary buffer,
        // then convert newlines to backslash-continuation sequences
        // and store in g_paste_buf.
        std::string raw;
        raw.reserve(256);
        consume_until(f, "\x1b[201~", raw);

        // Normalise \r\n → \n, then convert each \n to '\\''\n' so
        // soft_newline fires for every pasted newline.
        g_paste_buf.clear();
        g_paste_buf.reserve(raw.size() + 16);
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\r') {
                if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
                g_paste_buf.push_back('\\');
                g_paste_buf.push_back('\n');
            } else if (raw[i] == '\n') {
                g_paste_buf.push_back('\\');
                g_paste_buf.push_back('\n');
            } else {
                g_paste_buf.push_back(raw[i]);
            }
        }
        g_paste_pos = 0;

        // Return the first character of the paste (NUL if empty,
        // which libedit ignores).
        if (g_paste_pos < g_paste_buf.size())
            return static_cast<unsigned char>(g_paste_buf[g_paste_pos++]);
        return 0;
    }

    // Not a bracketed-paste sequence.  Before replaying to libedit,
    // check whether the accumulated bytes look like a CPR (Cursor
    // Position Report): \e[row;colR.  CPR responses are emitted by
    // the terminal in reply to \x1b[6n (DSR) sent by SelectOption's
    // pre_lines adjustment logic.  If they arrive here it means a
    // previous SelectOption call didn't fully drain its CPR response
    // from the input queue.  Returning \e to libedit would corrupt
    // its key FSM (it would wait for a continuation byte and swallow
    // the user's next real keystroke as an escape sequence, causing
    // the prompt to vanish).  Discard silently and loop.
    //
    // A CPR ends with 'R' and contains only digits and ';' after '['.
    // acc already holds everything after "ESC ["; check that it ends
    // with 'R' and contains only digits/semicolons.
    {
        bool looks_like_cpr = !acc.empty() && acc.back() == 'R';
        if (looks_like_cpr) {
            for (size_t ci = 0; ci + 1 < acc.size(); ++ci) {
                const char ch = acc[ci];
                if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != ';') {
                    looks_like_cpr = false;
                    break;
                }
            }
        }
        if (looks_like_cpr) {
            // Silently discard the CPR and ask for the next real byte.
            g_paste_buf.clear();
            g_paste_pos = 0;
            // Tail-call: re-enter bracketed_getc so the next byte is
            // returned normally.  We can't just `continue` from here
            // (we're not in a loop), so recurse once.  Stack depth is
            // bounded because a second CPR in a row is astronomically
            // unlikely and only real input follows.
            return bracketed_getc(f);
        }
    }

    // Not a bracketed-paste sequence and not a CPR — stash ESC '[' +
    // everything we accumulated into g_paste_buf so libedit sees the
    // full sequence.
    g_paste_buf.clear();
    g_paste_buf.push_back('[');
    g_paste_buf.append(acc);
    g_paste_pos = 0;
    return '\x1b';
}

} // anon namespace

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
		paths::EnsureParentDir(g_history_file);
		read_history(g_history_file.c_str());
	}

	// Snapshot the tty settings right now, before libedit or
	// EscInterruptGuard touch them.  Deinit() restores this as
	// a safety net so the shell always inherits a sane tty.
	if (isatty(fileno(stdin))) {
		if (tcgetattr(fileno(stdin), &g_saved_termios) == 0)
			g_saved_termios_valid = true;
	}

	// Create the self-pipe used by WakeReadMessage() to interrupt the
	// blocking poll() in raw_getc_or_wake().
	if (::pipe(g_wake_pipe) == 0) {
		// Set non-blocking on the write end so the timer thread never stalls.
		::fcntl(g_wake_pipe[1], F_SETFL,
			::fcntl(g_wake_pipe[1], F_GETFL) | O_NONBLOCK);
		// Close-on-exec: children (Bash, Grep, notify, popen, etc.) must
		// not inherit these pipe ends.  If a grandchild exits while holding
		// a copy of g_wake_pipe[0], poll() in raw_getc_or_wake() would see
		// POLLHUP; if it holds g_wake_pipe[1] and we close ours, read()
		// would return EOF — causing readline() to return nullptr and the
		// REPL to exit unexpectedly.
		::fcntl(g_wake_pipe[0], F_SETFD, FD_CLOEXEC);
		::fcntl(g_wake_pipe[1], F_SETFD, FD_CLOEXEC);
	}

	// Save the real tty fd and create a blocking pipe so BlockStdin() can
	// redirect fd 0 to g_block_pipe[0].  A pipe read() blocks when empty,
	// so libedit's internal read(0,...) won't consume tty bytes.
	if (isatty(fileno(stdin))) {
		g_real_tty_fd = ::dup(fileno(stdin));
		// Close-on-exec on the real tty dup so children don't open a second
		// handle to the terminal and disrupt termios/signal state.
		::fcntl(g_real_tty_fd, F_SETFD, FD_CLOEXEC);
		::pipe(g_block_pipe);
		// Keep write end non-blocking so UnblockStdin() never stalls.
		::fcntl(g_block_pipe[1], F_SETFL,
			::fcntl(g_block_pipe[1], F_GETFL) | O_NONBLOCK);
		// Close-on-exec: same rationale as g_wake_pipe above.  Also
		// prevents stale '\r' bytes (written by UnblockStdin) from being
		// read by a child that inherits the read-end while it is fd 0.
		::fcntl(g_block_pipe[0], F_SETFD, FD_CLOEXEC);
		::fcntl(g_block_pipe[1], F_SETFD, FD_CLOEXEC);
	}

	rl_attempted_completion_function = slash_completion;

	// Wrap libedit's completion-list display so the flush timer is
	// paused while the match list is on screen.  Without this, the
	// 16 ms flush-timer thread fires during a turn, writes Claude's
	// streaming output via DECSC/CUP/DECRC, and either overwrites the
	// completion list or corrupts the cursor position so the list
	// appears at the wrong row.  PauseFlushTimer()/ResumeFlushTimer()
	// are the same primitives used by SelectOption() — they drain any
	// pending turn output first, then give libedit exclusive access to
	// stdout for the duration of the completion display.
	//
	// rl_completion_display_matches_hook, when non-null, is called by
	// libedit instead of its built-in rl_display_match_list(), so we
	// call that function ourselves after pausing the timer.
	rl_completion_display_matches_hook = [](char** matches, int num, int max) {
		tui::PauseFlushTimer();
		rl_display_match_list(matches, num, max);
		tui::ResumeFlushTimer();
	};

	// Install our custom getc function.  The bracketed-paste enable
	// sequence (\e[?2004h) is sent later, after all keybindings are
	// set up, via a single unbuffered ::write() call so there is no
	// race between stdio-buffered and raw writes.
	rl_getc_function = bracketed_getc;

	// Install an event hook that fires while readline is waiting for
	// input.  We use it to clear libedit's internal edit buffer when a
	// tool permission menu has just dismissed — the approval keystroke
	// may be in libedit's buffer even if rl_getc_function is not honoured.
	rl_event_hook = []() -> int {
		// Only clear when no menu is active (g_stdin_blocked=true means
		// SelectOption is running and we must not touch libedit state).
		if (!g_stdin_blocked.load() && ConsumeClearEditBufferRequest()) {
			rl_replace_line("", 0);
			rl_point = 0;
		}
		return 0;
	};

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
		// Flush any stdio-buffered output (welcome text, status bar)
		// before enabling bracketed paste so the single ::write() below
		// is not interleaved with buffered writes.  Sending \e[?2004h
		// exactly once via an unbuffered write minimises the chance of
		// the terminal emitting a spurious response sequence into stdin
		// before the first readline() call.
		fflush(stdout);
		::write(fileno(stdout), "\x1b[?2004h", 8);
	}
}

void Deinit() {
	// If BlockStdin() redirected fd 0 to the blocking pipe (e.g. the
	// process is exiting while a tool permission menu was showing),
	// restore the real tty as stdin so the shell inherits a sane fd 0.
	if (g_real_tty_fd >= 0 && !isatty(STDIN_FILENO)) {
		::dup2(g_real_tty_fd, STDIN_FILENO);
		g_stdin_blocked.store(false);
	}

	// Disable bracketed paste mode so the terminal is left clean.
	// Mirror the isatty() guard from init() — no-op on non-TTY.
	if (isatty(fileno(stdin))) {
		::write(fileno(stdout), "\x1b[?2004l", 8);
	}

	// Restore the original tty settings saved at Init() time.
	// libedit and EscInterruptGuard both modify termios during normal
	// operation; each is supposed to restore on its own clean exit,
	// but as a belt-and-suspenders safety net we explicitly restore
	// the pre-session state here so the shell always inherits a
	// sane cooked/echo tty regardless of how we exited.
	if (g_saved_termios_valid && isatty(fileno(stdin))) {
		tcsetattr(fileno(stdin), TCSAFLUSH, &g_saved_termios);
		g_saved_termios_valid = false;
	}

	// Drain any bytes the terminal queued in response to our escape
	// sequences (scroll-region setup, bracketed-paste enable, etc.)
	// so the shell doesn't see stale bytes after we exit.
	DrainStaleInput();

	// Close the internal pipe fds so they don't leak into child processes
	// spawned by the shell after we exit.
	if (g_wake_pipe[0] >= 0) { ::close(g_wake_pipe[0]); g_wake_pipe[0] = -1; }
	if (g_wake_pipe[1] >= 0) { ::close(g_wake_pipe[1]); g_wake_pipe[1] = -1; }
	if (g_block_pipe[0] >= 0) { ::close(g_block_pipe[0]); g_block_pipe[0] = -1; }
	if (g_block_pipe[1] >= 0) { ::close(g_block_pipe[1]); g_block_pipe[1] = -1; }
	if (g_real_tty_fd >= 0) { ::close(g_real_tty_fd); g_real_tty_fd = -1; }
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

void BlockStdin() {
	g_stdin_blocked.store(true);
	// Drain any stale '\r' bytes written by previous UnblockStdin() calls
	// so that libedit's internal read(0,...) can't consume them and return
	// a spurious empty line (which in Haiku's libedit maps to a readline()
	// null return, causing the REPL to exit as if Ctrl+D was pressed).
	if (g_block_pipe[0] >= 0) {
		char discard[64];
		while (::read(g_block_pipe[0], discard, sizeof(discard)) > 0) {}
	}
	// Replace fd 0 with the read end of g_block_pipe so that libedit's
	// internal read(0,...) blocks (empty pipe) instead of consuming bytes
	// from the tty while SelectOption() runs.
	if (g_block_pipe[0] >= 0)
		::dup2(g_block_pipe[0], STDIN_FILENO);
	// Wake any poll() in raw_getc_or_wake that's watching the old fd 0.
	WakeReadMessage();
}

void UnblockStdin() {
	// Restore the real tty as fd 0 before unblocking so libedit's next
	// read comes from the terminal, not the pipe.
	if (g_real_tty_fd >= 0)
		::dup2(g_real_tty_fd, STDIN_FILENO);
	// Flush any tty bytes that arrived while stdin was blocked (e.g. the
	// approval keystroke that SelectOption read directly from g_real_tty_fd).
	if (isatty(STDIN_FILENO))
		tcflush(STDIN_FILENO, TCIFLUSH);
	g_stdin_blocked.store(false);
	// Ask the rl_event_hook to clear libedit's edit buffer on its next call.
	RequestClearEditBuffer();
	// Unblock any libedit read() currently blocking on the pipe by injecting
	// a CR.  The rl_event_hook will clear the edit buffer before libedit
	// processes the CR, so the submitted line will be empty.
	if (g_block_pipe[1] >= 0) {
		const char cr = '\r';
		::write(g_block_pipe[1], &cr, 1);
	}
}

int RealTtyFd() {
	return g_real_tty_fd;
}

void ClearEditBuffer() {
	// Haiku's libedit may not honour rl_getc_function, meaning it reads
	// stdin directly and can capture keystrokes (e.g. the approval digit
	// from a tool menu) into its internal edit buffer.  rl_replace_line("")
	// clears that buffer so those stale bytes don't appear in the next prompt.
	if (!isatty(fileno(stdin))) return;
	rl_replace_line("", 0);
	rl_point = 0;
}

namespace {
std::atomic<bool> g_clear_edit_buffer_requested{false};
} // anon namespace

void RequestClearEditBuffer() {
	g_clear_edit_buffer_requested.store(true);
	// Wake the main readline loop so it processes the request promptly.
	WakeReadMessage();
}

bool ConsumeClearEditBufferRequest() {
	return g_clear_edit_buffer_requested.exchange(false);
}

void DrainStaleInput() {
	if (!isatty(fileno(stdin))) return;

	// Switch stdin to non-blocking so we can drain whatever bytes the
	// terminal sent in response to our init sequences (bracketed-paste
	// enable, DECSTBM, cursor-position requests) without hanging.
	const int fd    = fileno(stdin);
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) return;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return;

	char discard[64];
	while (::read(fd, discard, sizeof(discard)) > 0)
		; // drain all available bytes

	// Restore blocking mode before handing stdin to readline.
	fcntl(fd, F_SETFL, flags);
}

void Record(const std::string& line) {
	if (line.empty()) return;
	add_history(line.c_str());
	if (!g_history_file.empty()) {
		write_history(g_history_file.c_str());
	}
}

void RemoveLastRecord() {
	if (!isatty(fileno(stdin))) return;
	if (history_length <= 0) return;
	// remove_history() takes a 0-based offset from history_base.
	// The most recent entry is at index history_length - 1.
	HIST_ENTRY* removed = remove_history(history_length - 1);
	if (removed) {
		free(const_cast<char*>(removed->line));
		free(removed->data);
		free(removed);
	}
	// Flush the trimmed history so the removed entry is not
	// re-read on the next session start.
	if (!g_history_file.empty()) {
		write_history(g_history_file.c_str());
	}
}

void RestoreInput(const std::string& text) {
	if (text.empty() || !isatty(fileno(stdin))) return;
	// rl_stuff_char() is LIFO — push bytes in reverse order so the
	// queue drains left-to-right into libedit's edit buffer at the
	// start of the next readline() call.
	for (int i = static_cast<int>(text.size()) - 1; i >= 0; --i) {
		rl_stuff_char(static_cast<unsigned char>(text[i]));
	}
}

void WakeReadMessage() {
	if (g_wake_pipe[1] >= 0) {
		const char b = 1;
		const ssize_t n = ::write(g_wake_pipe[1], &b, 1);
		(void)n;
	}
}

} // namespace repl
