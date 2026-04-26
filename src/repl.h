#ifndef HAIKU_CLAUDE_CLI_REPL_H
#define HAIKU_CLAUDE_CLI_REPL_H

#include <string>
#include <vector>

// Thin wrapper around libedit's readline-compatible API. Provides line
// editing (arrow keys, emacs bindings) and persistent in-memory +
// file-backed history for the interactive REPL.
namespace repl {

// Load previously saved history from `history_file`. Missing or
// unreadable files are silently ignored. Safe to call more than once.
void Init(const std::string& history_file);

// Restore terminal state changed by init() (currently: disable
// bracketed paste mode). Safe to call even if init() was never called
// or if stdin is not a TTY. Called automatically via atexit when
// main() uses repl::Init(), but callers that install their own signal
// handlers should call this before re-raising so the terminal is left
// clean even on SIGINT/SIGTERM.
void Deinit();

// Register the full list of slash-command Names (with the leading
// slash — "/help", "/clear", etc.) that tab completion should offer
// when the current word starts with `/`. Replaces any previous list.
void SetSlashCommands(const std::vector<std::string>& names);

// Read one line from stdin with editing. `prompt` may contain ANSI
// escape sequences — they're wrapped in \001/\002 internally so libedit
// counts columns correctly. Returns false on EOF (Ctrl+D).
bool ReadLine(const std::string& prompt, std::string& out);

// Read a logical message from stdin. Supports two continuation modes:
//   1. A bare `"""` line starts a fenced block that ends on another
//      `"""` line (matching `'''` also works).
//   2. A line ending with a trailing `\` continues onto the next line,
//      concatenated with a newline.
// The continuation prompt is shown for subsequent lines. Returns false
// on EOF; partial input collected so far is returned in `out`.
bool ReadMessage(const std::string& prompt,
				  const std::string& ContinuationPrompt,
				  std::string&       out);

// Drain any bytes that may have accumulated in stdin between terminal
// init (e.g. bracketed-paste enable, status bar drawing) and the first
// readline() call. Some terminal emulators send unsolicited responses
// (cursor-position reports, focus events) that would otherwise be
// mis-parsed by bracketed_getc and cause the first Enter keypress to
// be swallowed. Puts stdin into non-blocking mode briefly, reads until
// the buffer is empty, then restores blocking mode. No-op on non-TTYs.
void DrainStaleInput();

// Block / unblock the main readline loop from reading stdin.
// While blocked, raw_getc_or_wake() will not call read() on stdin,
// giving SelectOption() exclusive access to the tty input queue.
// Safe to call from any thread.
void BlockStdin();
void UnblockStdin();

// Returns the real tty fd that was dup()'d at Init() time.  Used by
// SelectOption() to read keypresses directly from the tty even when
// STDIN_FILENO has been redirected to /dev/null by BlockStdin().
// Returns -1 if not available (non-TTY or not yet initialised).
int RealTtyFd();

// Clear libedit's internal edit buffer.  Called after a tool permission
// menu so any keystroke (e.g. the approval digit) that libedit captured
// from stdin while the menu was active is discarded before the next prompt.
// Must be called from the main thread (the thread that owns libedit).
void ClearEditBuffer();

// Request that the main readline loop clear libedit's edit buffer on its
// next iteration.  Safe to call from any thread (worker included).
// The main thread checks this flag at the top of ReadMessage() and clears
// the buffer before libedit accepts any more input.
void RequestClearEditBuffer();
bool ConsumeClearEditBufferRequest();

// Append `line` to history and flush to disk. No-op on empty lines.
void Record(const std::string& line);

// Remove the most recently recorded history entry. Used to suppress
// a Ctrl+X–cancelled turn from appearing in up-arrow history — the
// amended re-submission is the canonical entry. No-op when history
// is empty or stdin is not a TTY.
void RemoveLastRecord();

// Write one byte to the internal wake pipe so a blocking ReadMessage()
// call returns promptly. Used by the flush timer in tui.cpp to unblock
// the poll when a turn completes, without requiring a real keypress.
void WakeReadMessage();

// Seed libedit's current edit buffer with `text` and redraw the
// prompt, as if the user had typed it. Used by the cancel-and-retype
// path to restore a cancelled turn's input so the user can amend it
// without retyping from scratch. No-op when stdin is not a TTY or
// when `text` is empty.
void RestoreInput(const std::string& text);

} // namespace repl

#endif
