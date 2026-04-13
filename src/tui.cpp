#include "tui.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <iostream>
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
        return "\x1b[38;5;114m" + line + "\x1b[0m";
    }
    return highlight_with(spec, line);
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

    // Opening code fence.
    if (line.size() >= 3 && line.substr(0, 3) == "```") {
        in_code_block_   = true;
        code_block_lang_ = line.substr(3);
        if (code_block_lang_.empty()) {
            emit(dim("```") + "\n");
        } else {
            emit(dim("``` " + code_block_lang_) + "\n");
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
