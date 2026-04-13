#ifndef HAIKU_CLAUDE_CLI_TUI_H
#define HAIKU_CLAUDE_CLI_TUI_H

#include <string>

namespace tui {

// Called once at startup to snapshot whether stdout supports ANSI colors,
// honoring NO_COLOR, CLICOLOR=0, and isatty(stdout). User flags
// (--plain / --color) then override the snapshot via set_color_enabled.
void init();

bool color_enabled();
void set_color_enabled(bool on);

std::string bold(const std::string& s);
std::string dim(const std::string& s);
std::string italic(const std::string& s);

std::string red(const std::string& s);
std::string green(const std::string& s);
std::string yellow(const std::string& s);
std::string blue(const std::string& s);
std::string magenta(const std::string& s);
std::string cyan(const std::string& s);
std::string gray(const std::string& s);

// Semantic wrappers. These return the full string to print (including
// trailing reset), or a plain equivalent when color is off.
std::string user_prompt();            // "you> "
std::string claude_prompt();          // "claude> "
std::string meta(const std::string& s); // dim bracketed note
std::string error_label();            // bold red "error:"

} // namespace tui

#endif
