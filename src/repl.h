#ifndef HAIKU_CLAUDE_CLI_REPL_H
#define HAIKU_CLAUDE_CLI_REPL_H

#include <string>

// Thin wrapper around libedit's readline-compatible API. Provides line
// editing (arrow keys, emacs bindings) and persistent in-memory +
// file-backed history for the interactive REPL.
namespace repl {

// Load previously saved history from `history_file`. Missing or
// unreadable files are silently ignored. Safe to call more than once.
void init(const std::string& history_file);

// Read one line from stdin with editing. `prompt` may contain ANSI
// escape sequences — they're wrapped in \001/\002 internally so libedit
// counts columns correctly. Returns false on EOF (Ctrl+D).
bool read_line(const std::string& prompt, std::string& out);

// Read a logical message from stdin. Supports two continuation modes:
//   1. A bare `"""` line starts a fenced block that ends on another
//      `"""` line (matching `'''` also works).
//   2. A line ending with a trailing `\` continues onto the next line,
//      concatenated with a newline.
// The continuation prompt is shown for subsequent lines. Returns false
// on EOF; partial input collected so far is returned in `out`.
bool read_message(const std::string& prompt,
                  const std::string& continuation_prompt,
                  std::string&       out);

// Append `line` to history and flush to disk. No-op on empty lines.
void record(const std::string& line);

} // namespace repl

#endif
