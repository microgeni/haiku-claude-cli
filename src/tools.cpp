#include "tools.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <glob.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace tools {

namespace {

ToolResult run_read(const json& input) {
    const std::string path = input.value("path", std::string{});
    if (path.empty()) {
        return {"error: Read requires a `path` argument", true};
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        return {"error: cannot open " + path, true};
    }

    const int start = input.value("start_line", 0);
    const int end   = input.value("end_line",   0);

    std::ostringstream out;
    std::string        line;
    int                ln = 0;
    bool               first = true;
    while (std::getline(f, line)) {
        ++ln;
        if (start > 0 && ln < start) continue;
        if (end   > 0 && ln > end)   break;
        if (!first) out << '\n';
        out << line;
        first = false;
    }

    if (first) {
        return {"(empty or out-of-range read at " + path + ")", false};
    }
    return {out.str(), false};
}

ToolResult run_glob(const json& input) {
    const std::string pattern = input.value("pattern", std::string{});
    if (pattern.empty()) {
        return {"error: Glob requires a `pattern` argument", true};
    }

    glob_t results {};
    const int rc = glob(pattern.c_str(), GLOB_NOSORT, nullptr, &results);
    if (rc == GLOB_NOMATCH) {
        globfree(&results);
        return {"(no matches for " + pattern + ")", false};
    }
    if (rc != 0) {
        globfree(&results);
        return {std::string("error: glob failed (") + std::strerror(errno) + ")", true};
    }

    struct Entry {
        std::string path;
        time_t      mtime = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(results.gl_pathc);
    for (size_t i = 0; i < results.gl_pathc; ++i) {
        Entry e;
        e.path = results.gl_pathv[i];
        struct stat st {};
        if (stat(e.path.c_str(), &st) == 0) {
            e.mtime = st.st_mtime;
        }
        entries.push_back(std::move(e));
    }
    globfree(&results);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });

    std::ostringstream out;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) out << '\n';
        out << entries[i].path;
    }
    return {out.str(), false};
}

} // namespace

json definitions() {
    return json::array({
        {
            {"name", "Read"},
            {"description",
                "Read a text file from the local filesystem and return its contents. "
                "Use start_line and end_line (both 1-indexed, inclusive) to read a "
                "specific range; omit them to read the whole file. Returns an error "
                "message on failure."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "Path to read, absolute or relative to the CLI's current working directory."},
                    }},
                    {"start_line", {
                        {"type", "integer"},
                        {"description", "Optional 1-indexed inclusive start line."},
                    }},
                    {"end_line", {
                        {"type", "integer"},
                        {"description", "Optional 1-indexed inclusive end line."},
                    }},
                }},
                {"required", json::array({"path"})},
            }},
        },
        {
            {"name", "Glob"},
            {"description",
                "Find files matching a shell-style glob pattern (e.g. 'src/*.cpp', "
                "'*.md', '/boot/home/**'). Returns the matching paths one per line, "
                "sorted by modification time with the most recently changed first. "
                "Uses POSIX glob; recursive '**' support depends on the platform."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"pattern", {
                        {"type", "string"},
                        {"description", "Glob pattern, e.g. 'src/*.cpp' or '/boot/home/*.md'."},
                    }},
                }},
                {"required", json::array({"pattern"})},
            }},
        },
    });
}

ToolResult run(const std::string& name, const json& input) {
    if (name == "Read") return run_read(input);
    if (name == "Glob") return run_glob(input);
    return {"error: unknown tool " + name, true};
}

} // namespace tools
