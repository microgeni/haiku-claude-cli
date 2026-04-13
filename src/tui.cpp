#include "tui.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <unistd.h>

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

std::string user_prompt() {
    return wrap("\x1b[1;36m", "you> ");
}

std::string claude_prompt() {
    return wrap("\x1b[1;35m", "claude> ");
}

std::string meta(const std::string& s) {
    return wrap("\x1b[2m", s);
}

std::string error_label() {
    return wrap("\x1b[1;31m", "error:");
}

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

void MarkdownRenderer::render_inline(const std::string& text) {
    if (!g_color_enabled) { emit(text); return; }

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
                out += ansi("\x1b[38;5;81m");
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
    emit(out);
}

void MarkdownRenderer::render_line(const std::string& line) {
    // Inside a code block, everything passes through with a dim tint
    // until we see the closing fence.
    if (in_code_block_) {
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            in_code_block_ = false;
            emit(dim("```") + "\n");
            return;
        }
        emit("\x1b[38;5;114m" + line + "\x1b[0m\n");
        return;
    }

    // Opening code fence.
    if (line.size() >= 3 && line.substr(0, 3) == "```") {
        in_code_block_ = true;
        const std::string lang = line.substr(3);
        if (lang.empty()) {
            emit(dim("```") + "\n");
        } else {
            emit(dim("``` " + lang) + "\n");
        }
        return;
    }

    // Headings.
    size_t hash_count = 0;
    while (hash_count < line.size() && line[hash_count] == '#') ++hash_count;
    if (hash_count > 0 && hash_count <= 3 && hash_count < line.size() && line[hash_count] == ' ') {
        const std::string rest = line.substr(hash_count + 1);
        const char* color = hash_count == 1 ? "\x1b[1;95m"
                          : hash_count == 2 ? "\x1b[1;94m"
                          :                    "\x1b[1;96m";
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
}

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
    static const char* kFrames[] = {
        "\u2819", "\u2839", "\u2838", "\u283c", "\u2834",
        "\u2826", "\u2827", "\u2807", "\u280f", "\u280b",
    };
    constexpr int kFrameCount = sizeof(kFrames) / sizeof(kFrames[0]);
    int idx = 0;

    while (!stopping_.load()) {
        {
            std::string frame = std::string(kFrames[idx]) + " " + label_;
            std::cout << "\r\x1b[2K" << dim(frame) << std::flush;
        }
        idx = (idx + 1) % kFrameCount;

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(80),
                     [this] { return stopping_.load(); });
    }
    // Clear the spinner line so the next write starts at column 0.
    std::cout << "\r\x1b[2K" << std::flush;
}

} // namespace tui
