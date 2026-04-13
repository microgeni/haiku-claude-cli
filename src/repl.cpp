#include "repl.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include <editline/readline.h>

namespace repl {
namespace {

std::string g_history_file;

void ensure_parent_dir(const std::string& path) {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return;
    const std::string dir = path.substr(0, slash);
    std::string accum;
    for (size_t i = 0; i < dir.size(); ++i) {
        accum += dir[i];
        const bool boundary = (dir[i] == '/') || (i + 1 == dir.size());
        if (!boundary) continue;
        if (accum.empty() || accum == "/") continue;
        if (mkdir(accum.c_str(), 0700) != 0 && errno != EEXIST) return;
    }
}

// Wrap ANSI escape sequences in \001..\002 so libedit knows to skip
// them when counting visible column width.
std::string wrap_for_readline(const std::string& prompt) {
    std::string out;
    out.reserve(prompt.size() + 16);
    size_t i = 0;
    while (i < prompt.size()) {
        if (prompt[i] == '\x1b' && i + 1 < prompt.size() && prompt[i + 1] == '[') {
            const size_t start = i;
            i += 2;
            while (i < prompt.size() && prompt[i] != 'm' && prompt[i] != 'K') ++i;
            if (i < prompt.size()) ++i; // consume terminator
            out.push_back('\001');
            out.append(prompt.substr(start, i - start));
            out.push_back('\002');
        } else {
            out.push_back(prompt[i++]);
        }
    }
    return out;
}

} // namespace

void init(const std::string& history_file) {
    g_history_file = history_file;
    if (!g_history_file.empty()) {
        ensure_parent_dir(g_history_file);
        read_history(g_history_file.c_str());
    }
}

bool read_line(const std::string& prompt, std::string& out) {
    const std::string wrapped = wrap_for_readline(prompt);
    char* line = readline(wrapped.c_str());
    if (!line) return false;
    out.assign(line);
    std::free(line);
    return true;
}

void record(const std::string& line) {
    if (line.empty()) return;
    add_history(line.c_str());
    if (!g_history_file.empty()) {
        write_history(g_history_file.c_str());
    }
}

} // namespace repl
