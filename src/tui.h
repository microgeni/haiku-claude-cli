#ifndef HAIKU_CLAUDE_CLI_TUI_H
#define HAIKU_CLAUDE_CLI_TUI_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

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
class MarkdownRenderer {
public:
    MarkdownRenderer();
    void set_spinner(Spinner* s) { spinner_ = s; }
    void write(const std::string& chunk);
    void flush();
private:
    void emit(const std::string& s);
    void render_line(const std::string& line);
    void render_inline(const std::string& text);

    std::string line_buffer_;
    std::string code_block_lang_;
    bool        in_code_block_     = false;
    bool        first_output_done_ = false;
    Spinner*    spinner_           = nullptr;
};

// Animated "thinking..." indicator. Spawns a background thread that
// writes dimmed spinner frames to stdout until stop() (or destruction).
// No-op when color is disabled or stdout is not a TTY.
class Spinner {
public:
    explicit Spinner(std::string label);
    ~Spinner();
    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;
    void stop();
private:
    void run();
    std::string             label_;
    std::atomic<bool>       stopping_{false};
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             thread_;
    bool                    active_ = false;
};

} // namespace tui

#endif
