#include "repl.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <editline/readline.h>

namespace repl {
namespace {

std::string              g_history_file;
std::vector<std::string> g_slash_commands;

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

extern "C" char** slash_completion(const char* text, int start, int /*end*/) {
    // Only complete when the cursor is at the start of the line AND
    // the current word begins with '/'. Otherwise let the default
    // filename completion run so argument completion still works.
    if (start != 0) return nullptr;
    if (!text || text[0] != '/') return nullptr;

    const std::string prefix = text;
    std::vector<std::string> matches;
    for (const auto& cmd : g_slash_commands) {
        if (cmd.size() >= prefix.size()
            && cmd.compare(0, prefix.size(), prefix) == 0) {
            matches.push_back(cmd);
        }
    }
    if (matches.empty()) return nullptr;

    // The libedit/readline convention: element 0 is the longest
    // common prefix (used as the "replacement" when there's more
    // than one match), elements 1..N are the actual candidates, and
    // the array is NULL-terminated.
    std::string lcp = matches[0];
    for (size_t i = 1; i < matches.size(); ++i) {
        size_t j = 0;
        while (j < lcp.size() && j < matches[i].size() && lcp[j] == matches[i][j]) ++j;
        lcp.resize(j);
    }

    char** result = static_cast<char**>(std::malloc((matches.size() + 2) * sizeof(char*)));
    if (!result) return nullptr;
    result[0] = strdup(lcp.c_str());
    for (size_t i = 0; i < matches.size(); ++i) {
        result[i + 1] = strdup(matches[i].c_str());
    }
    result[matches.size() + 1] = nullptr;
    return result;
}

} // namespace

void init(const std::string& history_file) {
    g_history_file = history_file;
    if (!g_history_file.empty()) {
        ensure_parent_dir(g_history_file);
        read_history(g_history_file.c_str());
    }
    rl_attempted_completion_function = slash_completion;
}

void set_slash_commands(const std::vector<std::string>& names) {
    g_slash_commands = names;
    std::sort(g_slash_commands.begin(), g_slash_commands.end());
}

bool read_line(const std::string& prompt, std::string& out) {
    const std::string wrapped = wrap_for_readline(prompt);
    char* line = readline(wrapped.c_str());
    if (!line) return false;
    out.assign(line);
    std::free(line);
    return true;
}

bool read_message(const std::string& prompt,
                  const std::string& continuation_prompt,
                  std::string&       out) {
    out.clear();

    std::string first;
    if (!read_line(prompt, first)) return false;

    // Fenced block: bare `"""` or `'''` on its own.
    if (first == "\"\"\"" || first == "'''") {
        const std::string fence = first;
        while (true) {
            std::string next;
            if (!read_line(continuation_prompt, next)) {
                return !out.empty();
            }
            if (next == fence) return true;
            if (!out.empty()) out.push_back('\n');
            out.append(next);
        }
    }

    // Backslash continuation.
    if (!first.empty() && first.back() == '\\') {
        first.pop_back();
        out.append(first);
        out.push_back('\n');
        while (true) {
            std::string next;
            if (!read_line(continuation_prompt, next)) {
                return true;
            }
            if (!next.empty() && next.back() == '\\') {
                next.pop_back();
                out.append(next);
                out.push_back('\n');
                continue;
            }
            out.append(next);
            return true;
        }
    }

    out = std::move(first);
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
