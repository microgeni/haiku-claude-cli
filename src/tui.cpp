#include "tui.h"

#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <sys/ioctl.h>
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

void init() {
    g_color_enabled = detect_color_support();
}

namespace {

// Cached terminal dimensions. Set to dirty initially so the first
// call to terminal_width()/terminal_rows() triggers an ioctl. The
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

int terminal_width() {
    if (!isatty(fileno(stdout))) return 0;
    if (g_term_dirty) refresh_dims();
    return g_cached_term_cols;
}

int terminal_rows() {
    if (!isatty(fileno(stdout))) return 0;
    if (g_term_dirty) refresh_dims();
    return g_cached_term_rows;
}

int consume_resize_pending() {
    const int v = g_resize_pending;
    g_resize_pending = 0;
    return v;
}

void install_sigwinch_handler() {
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
// so both install_status_bar and redraw_status_bar share the logic.
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
              << muted(rule);

    // Row N-2: input row. Clear any leftover content so a stale
    // prompt doesn't bleed into the next turn; we don't draw
    // anything here — libedit owns this row when read_message
    // runs, and position_cursor_for_input parks the cursor at
    // column 1.
    std::cout << "\x1b[" << (rows - 2) << ";1H"
              << "\x1b[2K";

    // Row N-1: rule below the input row.
    std::cout << "\x1b[" << (rows - 1) << ";1H"
              << "\x1b[2K"
              << muted(rule);

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

void install_status_bar(const std::string& initial_status) {
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

void set_status_bar(const std::string& status) {
    g_status_bar_text = status;
    if (!g_status_bar_active) return;
    if (g_term_dirty) refresh_dims();
    draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
    std::cout.flush();
}

void redraw_status_bar() {
    if (!g_status_bar_active) return;
    refresh_dims();
    apply_scroll_region(g_cached_term_rows);
    draw_fixed_frame(g_cached_term_rows, g_cached_term_cols, g_status_bar_text);
    std::cout.flush();
}

void teardown_status_bar() {
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

void emit_chat_rule() {
    // No-op when the fixed-bottom frame is active — the rule row
    // already lives at row N-3 and is kept fresh by redraws, so
    // emitting an in-chat rule would duplicate it into the
    // scrolling history.
    if (g_status_bar_active) return;
    if (!g_color_enabled) return;
    if (!isatty(fileno(stdout))) return;
    const int width = terminal_width();
    if (width <= 0) return;
    std::string rule;
    rule.reserve(width * 3);
    for (int i = 0; i < width; ++i) rule += "\xE2\x94\x80"; // ─
    std::cout << dim(rule) << "\n" << std::flush;
}

void position_cursor_for_input() {
    if (!g_status_bar_active) return;
    if (g_term_dirty) refresh_dims();
    if (g_cached_term_rows < kStatusBarRows + 1) return;
    std::cout << "\x1b[" << (g_cached_term_rows - 2) << ";1H"
              << "\x1b[2K"
              << std::flush;
}

void position_cursor_for_chat() {
    if (!g_status_bar_active) return;
    if (g_term_dirty) refresh_dims();
    if (g_cached_term_rows < kStatusBarRows + 1) return;
    const int bottom = g_cached_term_rows - kStatusBarRows;
    std::cout << "\x1b[" << bottom << ";1H" << std::flush;
}

void hide_cursor() {
    if (!g_color_enabled) return;
    if (!isatty(fileno(stdout))) return;
    std::cout << "\x1b[?25l" << std::flush;
}

void show_cursor() {
    if (!g_color_enabled) return;
    if (!isatty(fileno(stdout))) return;
    std::cout << "\x1b[?25h" << std::flush;
}

bool color_enabled() { return g_color_enabled; }

void set_color_enabled(bool on) { g_color_enabled = on; }

std::string bold(const std::string& s)    { return wrap("\x1b[1m",  s); }
std::string dim(const std::string& s)     { return wrap("\x1b[2m",  s); }
std::string italic(const std::string& s)  { return wrap("\x1b[3m",  s); }

std::string red(const std::string& s)     { return wrap("\x1b[31m", s); }
std::string green(const std::string& s)   { return wrap("\x1b[32m", s); }
std::string yellow(const std::string& s)  { return wrap("\x1b[33m", s); }
std::string blue(const std::string& s)    { return wrap("\x1b[34m", s); }
std::string magenta(const std::string& s) { return wrap("\x1b[35m", s); }
std::string cyan(const std::string& s)    { return wrap("\x1b[36m", s); }
std::string gray(const std::string& s)    { return wrap("\x1b[90m", s); }
std::string muted(const std::string& s)   { return wrap("\x1b[38;5;244m", s); }

std::string user_prompt() {
    return wrap("\x1b[1;36m", "> ");
}

std::string claude_prompt() {
    return wrap("\x1b[1;35m", "claude> ");
}

std::string continuation_prompt() {
    return wrap("\x1b[2m", "... ");
}

std::string meta(const std::string& s) {
    return wrap("\x1b[2m", s);
}

std::string error_label() {
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

void MarkdownRenderer::emit(const std::string& s) {
    if (!first_output_done_) {
        first_output_done_ = true;
        if (spinner_) {
            spinner_->stop();
            spinner_ = nullptr;
        }
    }
    std::cout << s << std::flush;
}

std::string MarkdownRenderer::render_inline_to_string(const std::string& text) {
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

void MarkdownRenderer::render_inline(const std::string& text) {
    emit(render_inline_to_string(text));
}

void MarkdownRenderer::flush_table() {
    if (!table_active_ || table_rows_.empty()) {
        table_rows_.clear();
        table_aligns_.clear();
        table_active_ = false;
        return;
    }

    // Normalize column count: some rows may have fewer/more cells
    // than others. Pick the max and pad short rows with empty
    // strings so the width math doesn't crash.
    size_t ncols = 0;
    for (const auto& r : table_rows_) ncols = std::max(ncols, r.size());
    for (auto& r : table_rows_) r.resize(ncols);
    while (table_aligns_.size() < ncols) table_aligns_.push_back(TableAlign::Left);
    table_aligns_.resize(ncols);

    // Render each cell's inline markdown (bold/italic/code) first
    // so the width math sees the already-formatted string (ANSI
    // escapes are zero-width in display_width). First row is the
    // header — bold it.
    std::vector<std::vector<std::string>> rendered(table_rows_.size());
    for (size_t r = 0; r < table_rows_.size(); ++r) {
        rendered[r].resize(ncols);
        for (size_t c = 0; c < ncols; ++c) {
            std::string cell = render_inline_to_string(table_rows_[r][c]);
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
        std::string out = dim(left);
        for (size_t c = 0; c < ncols; ++c) {
            std::string dashes;
            for (int i = 0; i < widths[c] + 2; ++i) dashes += "\xE2\x94\x80"; // ─
            out += dim(dashes);
            out += dim(c + 1 == ncols ? right : mid);
        }
        out += "\n";
        emit(out);
    };

    auto draw_row = [&](const std::vector<std::string>& row) {
        std::string out = dim("\xE2\x94\x82"); // │
        for (size_t c = 0; c < ncols; ++c) {
            out += " ";
            out += pad_cell(row[c], widths[c], table_aligns_[c]);
            out += " ";
            out += dim("\xE2\x94\x82");
        }
        out += "\n";
        emit(out);
    };

    draw_border("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90"); // ┌ ┬ ┐
    draw_row(rendered[0]);
    draw_border("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4"); // ├ ┼ ┤
    for (size_t r = 1; r < rendered.size(); ++r) draw_row(rendered[r]);
    draw_border("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98"); // └ ┴ ┘

    table_rows_.clear();
    table_aligns_.clear();
    table_active_ = false;
}

void MarkdownRenderer::render_line(const std::string& line) {
    // Inside a code block, highlight per recognized language until we
    // see the closing fence.
    if (in_code_block_) {
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            in_code_block_   = false;
            code_block_lang_.clear();
            emit(dim("```") + "\n");
            return;
        }
        emit(highlight_code(code_block_lang_, line) + "\n");
        return;
    }

    // Opening code fence. Tables end at any non-table line — flush
    // the buffer first so a fenced code block can't land inside an
    // unclosed table.
    if (line.size() >= 3 && line.substr(0, 3) == "```") {
        if (table_active_) flush_table();
        in_code_block_   = true;
        code_block_lang_ = line.substr(3);
        if (code_block_lang_.empty()) {
            emit(dim("```") + "\n");
        } else {
            emit(dim("``` " + code_block_lang_) + "\n");
        }
        return;
    }

    // Table row handling. Buffer rows until we see a non-table
    // line, then flush with computed column widths. The second
    // row (index 1) is treated as the alignment separator if it
    // looks like one — otherwise it's a normal body row.
    if (is_table_row(line)) {
        if (!table_active_) {
            table_active_ = true;
            table_rows_.push_back(split_table_row(line));
            return;
        }
        if (table_rows_.size() == 1) {
            std::vector<TableAlign> aligns;
            if (is_table_separator(line, &aligns)) {
                table_aligns_ = std::move(aligns);
                return;
            }
        }
        table_rows_.push_back(split_table_row(line));
        return;
    }
    if (table_active_) {
        flush_table();
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
        emit(std::string(color) + rest + "\x1b[0m\n");
        return;
    }

    // Bullet list: optional leading whitespace, then '- ' or '* '.
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 1 < line.size() && (line[i] == '-' || line[i] == '*') && line[i + 1] == ' ') {
        const std::string indent(i, ' ');
        emit(indent + "\x1b[36m\u2022\x1b[0m ");
        render_inline(line.substr(i + 2));
        emit("\n");
        return;
    }

    // Numbered list: N. or N) at line start (optionally indented).
    {
        size_t j = i;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
            emit(std::string(i, ' ') + "\x1b[36m" + line.substr(i, j - i + 1) + "\x1b[0m ");
            render_inline(line.substr(j + 2));
            emit("\n");
            return;
        }
    }

    // Regular paragraph line.
    render_inline(line);
    emit("\n");
}

void MarkdownRenderer::write(const std::string& chunk) {
    if (!g_color_enabled) {
        std::cout << chunk << std::flush;
        if (!first_output_done_) {
            first_output_done_ = true;
            if (spinner_) {
                spinner_->stop();
                spinner_ = nullptr;
            }
        }
        return;
    }

    for (char c : chunk) {
        if (c == '\n') {
            render_line(line_buffer_);
            line_buffer_.clear();
        } else {
            line_buffer_ += c;
        }
    }
}

void MarkdownRenderer::flush() {
    if (!line_buffer_.empty()) {
        render_line(line_buffer_);
        line_buffer_.clear();
    }
    if (table_active_) flush_table();
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

// Rotating star-shaped glyphs for the leading spinner character.
// These match Claude Code's `✶ ... ✷ ...` feel and all render as a
// single column on monospace terminals.
const char* kSpinnerGlyphs[] = {
    "\xE2\x9C\xB6",  // ✶ U+2736 SIX POINTED BLACK STAR
    "\xE2\x9C\xB7",  // ✷ U+2737 EIGHT POINTED RECTILINEAR BLACK STAR
    "\xE2\x9C\xB8",  // ✸ U+2738 HEAVY EIGHT POINTED RECTILINEAR BLACK STAR
    "\xE2\x9C\xB9",  // ✹ U+2739 TWELVE POINTED BLACK STAR
    "\xE2\x9C\xBA",  // ✺ U+273A SIXTEEN POINTED ASTERISK
    "\xE2\x9C\xBB",  // ✻ U+273B TEARDROP-SPOKED ASTERISK
    "\xE2\x9C\xBC",  // ✼ U+273C OPEN CENTRE TEARDROP-SPOKED ASTERISK
    "\xE2\x9C\xBD",  // ✽ U+273D HEAVY TEARDROP-SPOKED ASTERISK
};
constexpr int kGlyphCount = sizeof(kSpinnerGlyphs) / sizeof(kSpinnerGlyphs[0]);

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

Spinner::Spinner(std::string label) : label_(std::move(label)) {
    if (!g_color_enabled) return;
    if (!isatty(fileno(stdout))) return;
    active_ = true;
    thread_ = std::thread(&Spinner::run, this);
}

Spinner::~Spinner() {
    stop();
}

void Spinner::stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Spinner::run() {
    // Pick a verb once per Spinner lifetime. The incoming label_ is
    // ignored in favor of the randomized gerund — callers used to
    // pass "thinking" but the richer rendering now wants a gerund
    // ending in -ing with no extra chrome. If label_ happens to
    // already be a gerund (e.g., set explicitly), we could honor it,
    // but the simpler path is to always randomize here.
    const char* const verb = kSpinnerVerbs[pick_verb_index()];

    int glyph_idx = 0;
    int frame_count = 0;

    const auto start = std::chrono::steady_clock::now();

    while (!stopping_.load()) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();

        // Live input token count, if a pointer was wired up via
        // set_live_input_tokens(). Cleared to 0 initially; the
        // caller writes to the atomic as soon as `message_start`
        // arrives over SSE, so there's a brief window (a few
        // hundred ms to a few seconds) where this jumps from 0 to
        // the real count and the spinner picks it up on the next
        // frame.
        int live_in = 0;
        if (live_input_tokens_) {
            live_in = live_input_tokens_->load(std::memory_order_relaxed);
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
        frame += "\x1b[0m ";
        frame += verb_wrap;
        if (!verb_bright) frame += "\x1b[2m"; // stack faint for the pulse dip
        frame += verb_str;
        frame += "\x1b[0m  ";
        frame += muted(tail);

        // Truncate to terminal_width() so long lines don't wrap.
        // We keep the leading spinner glyph + verb block intact
        // and drop the tail from the right if needed. Byte-count
        // truncation is close enough since everything after the
        // glyph is ASCII.
        const int width = terminal_width();
        if (width > 4) {
            const int budget = width - 1;
            // Approximate display width: treat every escape
            // sequence as zero columns, and each byte of UTF-8
            // glyph content as its byte count minus 2 (since our
            // star glyphs are 3 UTF-8 bytes but 1 column).
            int display_cols = 0;
            bool in_esc = false;
            for (size_t i = 0; i < frame.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(frame[i]);
                if (in_esc) {
                    if (c == 'm') in_esc = false;
                    continue;
                }
                if (c == 0x1b) { in_esc = true; continue; }
                if (c < 0x80) { ++display_cols; continue; }
                // UTF-8 lead byte: count as one column, skip
                // continuation bytes.
                ++display_cols;
                if ((c & 0xE0) == 0xC0) i += 1;
                else if ((c & 0xF0) == 0xE0) i += 2;
                else if ((c & 0xF8) == 0xF0) i += 3;
            }
            if (display_cols > budget) {
                // Simple tail trim: drop bytes from the end until
                // we're under budget. Cheap and rarely needed.
                while (!frame.empty() && display_cols > budget) {
                    frame.pop_back();
                    --display_cols;
                }
                frame += "\xE2\x80\xA6"; // …
            }
        }

        std::cout << "\r\x1b[2K" << dim(frame) << std::flush;

        glyph_idx = (glyph_idx + 1) % kGlyphCount;
        ++frame_count;

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(80),
                     [this] { return stopping_.load(); });
    }
    // Clear the spinner line so the next write starts at column 0.
    std::cout << "\r\x1b[2K" << std::flush;
}

} // namespace tui
