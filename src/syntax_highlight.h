#ifndef HAIKU_CLAUDE_CLI_SYNTAX_HIGHLIGHT_H
#define HAIKU_CLAUDE_CLI_SYNTAX_HIGHLIGHT_H

// syntax_highlight.h — language-aware tokeniser for the GUI code-block renderer.
//
// Produces a flat list of TokenSpan values for a single source line.
// Each span carries the raw text slice and a TokenKind tag; the caller
// maps the kind to an rgb_color / bold flag and feeds md::Run objects
// into MdRenderer::AppendRun().
//
// Supported languages (via the fence-tag string):
//   C / C++   — cpp, c++, cxx, c, h, hpp, hxx
//   Bash / sh — bash, sh, zsh, shell
//   Python    — python, py
//   Rust      — rust, rs
//   JSON      — json
//
// Unknown / empty lang tags → every span is TokenKind::Plain.
//
// Design note: this module is intentionally self-contained (no BeAPI,
// no ANSI, no tui.h dependency) so it can be unit-tested and shared
// between the GUI and any future syntax-aware terminal path.

#include <string>
#include <vector>

namespace syntax {

// ── Token kinds ──────────────────────────────────────────────────────────────

enum class TokenKind {
	Plain,       // ordinary identifier / punctuation — default text colour
	Keyword,     // control-flow keyword  (bold magenta)
	Type,        // built-in type         (bold cyan)
	Preprocessor,// C/C++ # directive     (magenta)
	String,      // string / char literal (green)
	Number,      // numeric literal       (cyan)
	Comment,     // line or block comment (dim grey)
	Operator,    // C++ operator          (yellow)
	Constant,    // UPPER_CASE macro/enum (bold yellow)
	Builtin,     // shell builtin/command (bold green)
	Variable,    // $VAR shell expansion  (cyan)
	Special,     // $? $# $@ shell specials (bold red)
};

// A contiguous slice of the source line with a colour tag.
struct TokenSpan {
	std::string text;
	TokenKind   kind = TokenKind::Plain;
};

// Tokenise one line of source code for the given language tag.
// Returns a non-empty vector; unknown lang → single Plain span for the line.
// The spans concatenate to exactly the original `line` string.
std::vector<TokenSpan> Tokenise(const std::string& lang,
                                const std::string& line);

} // namespace syntax

#endif // HAIKU_CLAUDE_CLI_SYNTAX_HIGHLIGHT_H
