#include "tui.h"

#include <cstdlib>
#include <cstdio>
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

} // namespace tui
