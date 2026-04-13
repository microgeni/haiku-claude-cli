#include "tools.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <glob.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
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

bool ensure_parent_dir(const std::string& path) {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return true;
    const std::string dir = path.substr(0, slash);
    if (dir.empty()) return true;
    std::string accum;
    for (size_t i = 0; i < dir.size(); ++i) {
        accum += dir[i];
        const bool boundary = (dir[i] == '/') || (i + 1 == dir.size());
        if (!boundary) continue;
        if (accum.empty() || accum == "/") continue;
        if (mkdir(accum.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool path_inside_cwd(const std::string& path) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    if (path.empty()) return false;
    if (path[0] == '/') {
        return path.compare(0, std::strlen(cwd), cwd) == 0;
    }
    return true; // relative paths are always inside cwd
}

ToolResult run_write(const json& input) {
    const std::string path = input.value("path", std::string{});
    if (path.empty()) {
        return {"error: Write requires a `path` argument", true};
    }
    if (!input.contains("content")) {
        return {"error: Write requires a `content` argument", true};
    }
    const std::string content = input.value("content", std::string{});

    if (!ensure_parent_dir(path)) {
        return {"error: cannot create parent directory for " + path, true};
    }

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return {"error: cannot open " + path + " for writing", true};
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.close();
    if (!f) {
        return {"error: write to " + path + " failed", true};
    }

    return {"wrote " + std::to_string(content.size()) + " bytes to " + path, false};
}

std::string preview_write(const json& input) {
    const std::string path    = input.value("path", std::string{});
    const std::string content = input.value("content", std::string{});
    const size_t      nbytes  = content.size();
    const size_t      nlines  = std::count(content.begin(), content.end(), '\n')
                                + (content.empty() || content.back() == '\n' ? 0 : 1);

    std::ifstream existing(path);
    std::string   header;
    if (existing.is_open()) {
        existing.seekg(0, std::ios::end);
        const auto old_size = existing.tellg();
        header = "overwrite " + path
               + " (" + std::to_string(static_cast<long>(old_size)) + " -> "
               + std::to_string(nbytes) + " bytes, " + std::to_string(nlines) + " lines)";
    } else {
        header = "new file " + path
               + " (" + std::to_string(nbytes) + " bytes, " + std::to_string(nlines) + " lines)";
    }
    if (!path_inside_cwd(path)) {
        header += "  [WARNING: outside current working directory]";
    }

    // Preview the first few lines of the new content so the user sees what
    // they're approving, capped at 10 lines.
    std::ostringstream body;
    body << "  -> " << header;
    size_t lines_shown = 0;
    std::string line;
    std::istringstream iss(content);
    while (lines_shown < 10 && std::getline(iss, line)) {
        body << "\n  | " << line;
        ++lines_shown;
    }
    if (lines_shown < nlines) {
        body << "\n  | ... (" << (nlines - lines_shown) << " more lines)";
    }
    return body.str();
}

ToolResult run_bash(const json& input) {
    const std::string command  = input.value("command", std::string{});
    const int         timeout  = input.value("timeout_seconds", 60);
    (void)timeout; // not enforced in this slice — parent trusts grace of fork+wait
    if (command.empty()) {
        return {"error: Bash requires a `command` argument", true};
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return {std::string("error: pipe() failed: ") + std::strerror(errno), true};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {std::string("error: fork() failed: ") + std::strerror(errno), true};
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
        execvp("sh", const_cast<char* const*>(argv));
        _exit(127);
    }

    close(pipefd[1]);
    std::string output;
    char        buf[4096];
    ssize_t     n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    constexpr size_t kMaxBytes = 32 * 1024;
    if (output.size() > kMaxBytes) {
        output = output.substr(0, kMaxBytes) + "\n[... output truncated]";
    }

    if (!WIFEXITED(status)) {
        return {"error: shell terminated abnormally (output: " + output + ")", true};
    }
    const int code = WEXITSTATUS(status);
    if (code != 0) {
        return {"exit " + std::to_string(code) + "\n" + output, true};
    }
    return {output.empty() ? "(no output)" : output, false};
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

ToolResult run_grep(const json& input) {
    const std::string pattern = input.value("pattern", std::string{});
    const std::string path    = input.value("path",    std::string{"."});
    const bool case_insensitive = input.value("case_insensitive", false);

    if (pattern.empty()) {
        return {"error: Grep requires a `pattern` argument", true};
    }

    std::vector<const char*> argv;
    argv.push_back("grep");
    argv.push_back("-rn");
    argv.push_back("-H");
    argv.push_back("--color=never");
    if (case_insensitive) argv.push_back("-i");
    argv.push_back("-e");
    argv.push_back(pattern.c_str());
    argv.push_back("--");
    argv.push_back(path.c_str());
    argv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return {std::string("error: pipe() failed: ") + std::strerror(errno), true};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {std::string("error: fork() failed: ") + std::strerror(errno), true};
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        execvp("grep", const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    close(pipefd[1]);
    std::string output;
    char        buf[4096];
    ssize_t     n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status)) {
        return {"error: grep terminated abnormally", true};
    }
    const int code = WEXITSTATUS(status);
    if (code == 1) {
        return {"(no matches for " + pattern + " in " + path + ")", false};
    }
    if (code == 127 || code == 126) {
        return {"error: grep not found on PATH", true};
    }
    if (code != 0) {
        return {"error: grep exited with code " + std::to_string(code)
                + (output.empty() ? "" : ": " + output), true};
    }

    constexpr size_t kMaxBytes = 32 * 1024;
    if (output.size() > kMaxBytes) {
        output = output.substr(0, kMaxBytes) + "\n[... output truncated]";
    }
    return {output, false};
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
            {"name", "Write"},
            {"description",
                "Create a file or overwrite an existing one with the given content. "
                "Parent directories are created if needed. Requires user permission "
                "on first use; the preview shows byte count, line count, and the "
                "first 10 lines of the new content before the prompt. Writes outside "
                "the current working directory are permitted but flagged in the "
                "preview."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "File path to write, absolute or relative to cwd."},
                    }},
                    {"content", {
                        {"type", "string"},
                        {"description", "Full text contents to write."},
                    }},
                }},
                {"required", json::array({"path", "content"})},
            }},
        },
        {
            {"name", "Bash"},
            {"description",
                "Run a shell command via `sh -c` and return its combined stdout+stderr "
                "plus exit code. Output is truncated to 32 KiB. The user is prompted "
                "for permission on the first Bash call of each session unless they "
                "pre-approved Bash; answer (a)lways to skip subsequent prompts. "
                "Prefer Read/Glob/Grep for pure inspection."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"command", {
                        {"type", "string"},
                        {"description", "Shell command line to execute."},
                    }},
                }},
                {"required", json::array({"command"})},
            }},
        },
        {
            {"name", "Grep"},
            {"description",
                "Search for a pattern across files under a directory. Uses POSIX "
                "grep -rn internally with -H (always-show-filename). Returns matches "
                "in `path:line:match` format, one per line. Output is truncated at "
                "32 KiB with a [... output truncated] marker."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"pattern", {
                        {"type", "string"},
                        {"description", "Basic regex pattern passed to grep -e."},
                    }},
                    {"path", {
                        {"type", "string"},
                        {"description", "File or directory to search. Defaults to the current working directory."},
                    }},
                    {"case_insensitive", {
                        {"type", "boolean"},
                        {"description", "Pass -i to grep for a case-insensitive match."},
                    }},
                }},
                {"required", json::array({"pattern"})},
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
    if (name == "Read")  return run_read(input);
    if (name == "Glob")  return run_glob(input);
    if (name == "Grep")  return run_grep(input);
    if (name == "Bash")  return run_bash(input);
    if (name == "Write") return run_write(input);
    return {"error: unknown tool " + name, true};
}

bool requires_permission(const std::string& name) {
    return name == "Bash" || name == "Write";
}

std::string preview(const std::string& name, const json& input) {
    if (name == "Write") return preview_write(input);
    return {};
}

} // namespace tools
