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

// Append `line` to history and flush to disk. No-op on empty lines.
void Record(const std::string& line);

} // namespace repl

#endif
