#include "tools.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <glob.h>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

// Shared cancellation flag — set by Esc or Ctrl+C in main.cpp.
extern volatile sig_atomic_t g_interrupted;

#include <curl/curl.h>

#include "mcp.h"

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

std::string read_file_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

ToolResult run_edit(const json& input) {
    const std::string path  = input.value("path",       std::string{});
    const std::string old_s = input.value("old_string", std::string{});
    const std::string new_s = input.value("new_string", std::string{});
    const bool replace_all  = input.value("replace_all", false);

    if (path.empty())  return {"error: Edit requires a `path` argument", true};
    if (old_s.empty()) return {"error: Edit requires a non-empty `old_string`", true};

    std::string content = read_file_all(path);
    if (content.empty()) {
        std::ifstream test(path);
        if (!test.is_open()) return {"error: cannot open " + path, true};
    }

    const size_t count = count_occurrences(content, old_s);
    if (count == 0) {
        return {"error: old_string not found in " + path, true};
    }
    if (count > 1 && !replace_all) {
        return {"error: old_string matches " + std::to_string(count)
                + " times in " + path + "; set replace_all=true to replace all", true};
    }

    std::string out;
    out.reserve(content.size());
    size_t start = 0;
    size_t match;
    while ((match = content.find(old_s, start)) != std::string::npos) {
        out.append(content, start, match - start);
        out.append(new_s);
        start = match + old_s.size();
        if (!replace_all) break;
    }
    out.append(content, start, std::string::npos);

    std::ofstream of(path, std::ios::binary);
    if (!of.is_open()) return {"error: cannot open " + path + " for writing", true};
    of.write(out.data(), static_cast<std::streamsize>(out.size()));
    of.close();
    if (!of) return {"error: write to " + path + " failed", true};

    const std::string plural = (count == 1) ? "" : "s";
    return {"edited " + path + " (" + std::to_string(count) + " replacement" + plural + ")", false};
}

std::string preview_edit(const json& input) {
    const std::string path  = input.value("path",       std::string{});
    const std::string old_s = input.value("old_string", std::string{});
    const std::string new_s = input.value("new_string", std::string{});
    const bool replace_all  = input.value("replace_all", false);

    const std::string content = read_file_all(path);
    if (content.empty() && !std::ifstream(path).is_open()) {
        return "  -> Edit " + path + "  [error: cannot open]";
    }

    const size_t count = count_occurrences(content, old_s);
    if (count == 0) {
        return "  -> Edit " + path + "  [error: old_string not found]";
    }
    if (count > 1 && !replace_all) {
        return "  -> Edit " + path + "  [error: " + std::to_string(count)
             + " matches; set replace_all=true]";
    }

    const size_t first = content.find(old_s);
    int line_num = 1;
    for (size_t i = 0; i < first; ++i) {
        if (content[i] == '\n') ++line_num;
    }

    std::ostringstream body;
    body << "  -> Edit " << path << " @ line " << line_num;
    if (count > 1) body << " (" << count << " matches, replace_all)";
    if (!path_inside_cwd(path)) body << "  [WARNING: outside cwd]";

    auto emit_lines = [&](const std::string& text, const char marker) {
        size_t shown = 0;
        std::istringstream iss(text);
        std::string line;
        while (shown < 12 && std::getline(iss, line)) {
            body << "\n  " << marker << " " << line;
            ++shown;
        }
        size_t total_lines = std::count(text.begin(), text.end(), '\n')
                           + (text.empty() || text.back() == '\n' ? 0 : 1);
        if (shown < total_lines) {
            body << "\n  " << marker << " ... (" << (total_lines - shown) << " more lines)";
        }
    };

    emit_lines(old_s, '-');
    emit_lines(new_s, '+');
    return body.str();
}

size_t append_to_string(void* ptr, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct Todo {
    std::string content;
    std::string status; // "pending", "in_progress", "completed"
};

std::vector<Todo> g_todos;

std::string format_todos() {
    if (g_todos.empty()) return "(no todos)";
    std::ostringstream out;
    for (size_t i = 0; i < g_todos.size(); ++i) {
        const char* mark = "[ ]";
        if (g_todos[i].status == "completed")   mark = "[x]";
        else if (g_todos[i].status == "in_progress") mark = "[-]";
        if (i > 0) out << "\n";
        out << mark << " " << g_todos[i].content;
    }
    return out.str();
}

ToolResult run_todowrite(const json& input) {
    if (!input.contains("todos") || !input["todos"].is_array()) {
        return {"error: TodoWrite requires a `todos` array", true};
    }
    g_todos.clear();
    for (const auto& t : input["todos"]) {
        if (!t.is_object()) continue;
        Todo todo;
        todo.content = t.value("content", std::string{});
        todo.status  = t.value("status",  std::string{"pending"});
        if (todo.content.empty()) continue;
        g_todos.push_back(std::move(todo));
    }
    return {"updated todo list (" + std::to_string(g_todos.size()) + " items)\n"
            + format_todos(), false};
}

ToolResult run_todoread(const json& /*input*/) {
    return {format_todos(), false};
}

ToolResult run_websearch(const json& input) {
    const std::string query = input.value("query", std::string{});
    if (query.empty()) {
        return {"error: WebSearch requires a `query` argument", true};
    }
    const char* key = std::getenv("BRAVE_SEARCH_API_KEY");
    if (!key || !*key) {
        return {"error: BRAVE_SEARCH_API_KEY is not set; get a free key at "
                "https://brave.com/search/api/", true};
    }

    CURL* curl = curl_easy_init();
    if (!curl) return {"error: curl_easy_init failed", true};

    char* escaped = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
    std::string url = "https://api.search.brave.com/res/v1/web/search?q=";
    url += (escaped ? escaped : "");
    curl_free(escaped);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("x-subscription-token: " + std::string(key)).c_str());
    headers = curl_slist_append(headers, "accept: application/json");

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli/0.10");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return {std::string("error: ") + curl_easy_strerror(res), true};
    }
    if (http_code != 200) {
        return {"error: Brave Search HTTP " + std::to_string(http_code) + ": " + body, true};
    }

    try {
        const json j = json::parse(body);
        if (!j.contains("web") || !j["web"].contains("results")
            || !j["web"]["results"].is_array()) {
            return {"(no results)", false};
        }
        std::ostringstream out;
        int shown = 0;
        for (const auto& r : j["web"]["results"]) {
            if (shown >= 10) break;
            if (shown > 0) out << "\n";
            out << "- " << r.value("title", std::string{}) << "\n"
                << "  " << r.value("url",   std::string{}) << "\n"
                << "  " << r.value("description", std::string{});
            ++shown;
        }
        if (shown == 0) return {"(no results)", false};
        return {out.str(), false};
    } catch (const json::exception& e) {
        return {std::string("error: failed to parse search response: ") + e.what(), true};
    }
}

ToolResult run_webfetch(const json& input) {
    const std::string url = input.value("url", std::string{});
    if (url.empty()) {
        return {"error: WebFetch requires a `url` argument", true};
    }
    const int max_bytes = input.value("max_bytes", 32 * 1024);

    CURL* curl = curl_easy_init();
    if (!curl) return {"error: curl_easy_init failed", true};

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "haiku-claude-cli/0.10");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    char* content_type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    std::string ct = content_type ? content_type : "";
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return {std::string("error: ") + curl_easy_strerror(res), true};
    }
    if (http_code < 200 || http_code >= 400) {
        return {"error: HTTP " + std::to_string(http_code), true};
    }

    const bool truncated = static_cast<int>(body.size()) > max_bytes;
    if (truncated) {
        body = body.substr(0, static_cast<size_t>(max_bytes));
    }

    std::string out;
    out += "HTTP " + std::to_string(http_code);
    if (!ct.empty()) out += "  (" + ct + ")";
    out += "\n\n";
    out += body;
    if (truncated) out += "\n\n[... truncated at " + std::to_string(max_bytes) + " bytes]";
    return {out, false};
}

ToolResult run_bash(const json& input) {
    const std::string command  = input.value("command", std::string{});
    const int         timeout  = input.value("timeout_seconds", 60);
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
        // Put the child in its own process group so we can kill the
        // whole group (child + any grandchildren) cleanly on cancel.
        setsid();
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
        execvp("sh", const_cast<char* const*>(argv));
        _exit(127);
    }

    close(pipefd[1]);

    // Deadline for the timeout (0 means no timeout enforced).
    const auto start_time = std::chrono::steady_clock::now();
    const bool has_timeout = (timeout > 0);

    std::string output;
    char        buf[4096];
    bool        killed = false;

    // Poll the read end in short bursts so we can react to
    // g_interrupted (Esc / Ctrl+C) or a timeout without blocking
    // indefinitely in read().
    while (true) {
        if (g_interrupted) {
            killed = true;
            break;
        }
        if (has_timeout) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= static_cast<double>(timeout)) {
                killed = true;
                break;
            }
        }

        struct pollfd pfd {};
        pfd.fd     = pipefd[0];
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, 100); // 100 ms tick
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; // timeout tick — loop back to check flags

        if (!(pfd.revents & POLLIN)) break; // HUP / ERR — child closed pipe
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break; // EOF
        output.append(buf, static_cast<size_t>(n));
    }

    close(pipefd[0]);

    if (killed) {
        // Kill the entire process group (negative pid = pgid from setsid).
        ::kill(-pid, SIGTERM);
        // Give it a moment to die gracefully, then SIGKILL.
        struct timespec ts { 0, 200'000'000 }; // 200 ms
        ::nanosleep(&ts, nullptr);
        ::kill(-pid, SIGKILL);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (killed && g_interrupted) {
        return {"[interrupted]", true};
    }
    if (killed) {
        // Timeout path.
        return {"error: command timed out after " + std::to_string(timeout) + "s\n" + output, true};
    }

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
        setsid(); // own process group so kill(-pid, ...) reaches it
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
    bool        killed = false;

    while (true) {
        if (g_interrupted) { killed = true; break; }

        struct pollfd pfd {};
        pfd.fd     = pipefd[0];
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, 100); // 100 ms tick
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; // tick — recheck g_interrupted

        if (!(pfd.revents & POLLIN)) break; // HUP/ERR
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break; // EOF
        output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    if (killed) {
        ::kill(-pid, SIGTERM);
        struct timespec ts { 0, 200'000'000 };
        ::nanosleep(&ts, nullptr);
        ::kill(-pid, SIGKILL);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (killed) return {"[interrupted]", true};

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

namespace {

json builtin_definitions() {
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
            {"name", "Task"},
            {"description",
                "Spawn a focused sub-agent to answer a self-contained question. "
                "The sub-agent runs as a fresh Claude turn with no tools and does "
                "not share the parent's conversation history — useful for "
                "delegating work that would otherwise pollute the main context "
                "(summarization, translation, expansion, etc.). The final "
                "assistant text is returned as this tool's result."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"description", {
                        {"type", "string"},
                        {"description", "Short label for the sub-task (displayed to the user)."},
                    }},
                    {"prompt", {
                        {"type", "string"},
                        {"description", "Full task prompt sent to the sub-agent."},
                    }},
                }},
                {"required", json::array({"prompt"})},
            }},
        },
        {
            {"name", "TodoWrite"},
            {"description",
                "Replace the current in-session todo list. Use this to plan "
                "multi-step work and check items off as they finish. The list is "
                "process-scoped — it does not persist across sessions. The user "
                "can view the current list at any time with the /todos slash "
                "command."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"todos", {
                        {"type", "array"},
                        {"description", "Full replacement list of todo items."},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"content", {{"type", "string"}}},
                                {"status",  {
                                    {"type", "string"},
                                    {"enum", json::array({"pending", "in_progress", "completed"})},
                                }},
                            }},
                            {"required", json::array({"content"})},
                        }},
                    }},
                }},
                {"required", json::array({"todos"})},
            }},
        },
        {
            {"name", "TodoRead"},
            {"description",
                "Return the current in-session todo list as a simple checklist."},
            {"input_schema", {
                {"type", "object"},
                {"properties", json::object()},
            }},
        },
        {
            {"name", "WebFetch"},
            {"description",
                "Fetch a URL via HTTPS and return the response body. Follows up to "
                "5 redirects, times out after 30 seconds, and truncates the body to "
                "`max_bytes` (default 32768). The first line of the result shows "
                "HTTP status and content-type."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"url", {
                        {"type", "string"},
                        {"description", "Absolute URL to fetch."},
                    }},
                    {"max_bytes", {
                        {"type", "integer"},
                        {"description", "Truncate the body to this many bytes."},
                    }},
                }},
                {"required", json::array({"url"})},
            }},
        },
        {
            {"name", "Edit"},
            {"description",
                "Replace an exact string in a file with a new string. old_string "
                "must appear verbatim (case-sensitive, whitespace-sensitive). By "
                "default the match must be unique; set replace_all=true to replace "
                "every occurrence. Requires user permission on first use; the "
                "preview shows the line number plus a block-style diff of the "
                "change."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "File path to edit."},
                    }},
                    {"old_string", {
                        {"type", "string"},
                        {"description", "Exact text to find in the file."},
                    }},
                    {"new_string", {
                        {"type", "string"},
                        {"description", "Replacement text."},
                    }},
                    {"replace_all", {
                        {"type", "boolean"},
                        {"description", "Replace every occurrence instead of requiring a unique match."},
                    }},
                }},
                {"required", json::array({"path", "old_string", "new_string"})},
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

// ── BFS attribute tools (Haiku only) ──────────────────────────
//
// These wrap Haiku's command-line attribute/query utilities so
// Claude can use the filesystem as a database. Guarded by
// __HAIKU__ — on macOS/Linux builds the tools simply don't
// exist and Claude never sees them.

#ifdef __HAIKU__

ToolResult exec_capture(const char* const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return {std::string("error: pipe() failed: ") + std::strerror(errno), true};
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return {std::string("error: fork() failed: ") + std::strerror(errno), true};
    }
    if (pid == 0) {
        setsid(); // own process group so we can kill descendants
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    close(pipefd[1]);
    std::string output;
    char        buf[4096];
    bool        killed = false;

    while (true) {
        if (g_interrupted) { killed = true; break; }

        struct pollfd pfd {};
        pfd.fd     = pipefd[0];
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, 100); // 100 ms tick
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; // tick — recheck g_interrupted

        if (!(pfd.revents & POLLIN)) break; // HUP/ERR
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break; // EOF
        output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    if (killed) {
        ::kill(-pid, SIGTERM);
        struct timespec ts { 0, 200'000'000 };
        ::nanosleep(&ts, nullptr);
        ::kill(-pid, SIGKILL);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (killed) return {"[interrupted]", true};

    constexpr size_t kMaxBytes = 32 * 1024;
    if (output.size() > kMaxBytes) {
        output = output.substr(0, kMaxBytes) + "\n[... output truncated]";
    }
    if (!WIFEXITED(status)) {
        return {"error: command terminated abnormally\n" + output, true};
    }
    const int code = WEXITSTATUS(status);
    if (code != 0 && output.empty()) {
        return {"exit " + std::to_string(code), true};
    }
    if (code != 0) {
        return {"exit " + std::to_string(code) + "\n" + output, true};
    }
    return {output.empty() ? "(no output)" : output, false};
}

ToolResult run_query(const json& input) {
    const std::string expr = input.value("expression", std::string{});
    if (expr.empty()) {
        return {"error: Query requires an `expression` argument", true};
    }
    const std::string vol = input.value("volume", std::string{});
    if (vol.empty()) {
        const char* argv[] = {"query", expr.c_str(), nullptr};
        return exec_capture(argv);
    }
    const char* argv[] = {"query", "-v", vol.c_str(), expr.c_str(), nullptr};
    return exec_capture(argv);
}

ToolResult run_read_attr(const json& input) {
    const std::string path = input.value("path", std::string{});
    if (path.empty()) {
        return {"error: ReadAttr requires a `path` argument", true};
    }
    const auto names = input.value("names", std::vector<std::string>{});
    if (names.empty()) {
        const char* argv[] = {"listattr", path.c_str(), nullptr};
        return exec_capture(argv);
    }
    std::string combined;
    for (const auto& name : names) {
        const char* argv[] = {"catattr", "-d", name.c_str(), path.c_str(), nullptr};
        auto result = exec_capture(argv);
        if (!combined.empty()) combined += "\n";
        combined += name + " : " + result.content;
        if (result.is_error) combined += " [error]";
    }
    return {combined, false};
}

ToolResult run_write_attr(const json& input) {
    const std::string path  = input.value("path", std::string{});
    const std::string name  = input.value("name", std::string{});
    const std::string value = input.value("value", std::string{});
    const std::string type  = input.value("type", std::string{"string"});
    if (path.empty() || name.empty()) {
        return {"error: WriteAttr requires `path` and `name` arguments", true};
    }
    // Hard-restrict to the claude:* namespace so Claude can't
    // silently overwrite BEOS:TYPE, MAIL:*, Audio:*, or any
    // other system-owned attribute. This restriction replaces
    // the old per-call permission prompt: WriteAttr is now
    // auto-approved, safety comes from the namespace gate.
    if (name.rfind("claude:", 0) != 0) {
        return {"error: WriteAttr is restricted to the claude:* namespace "
                "(got '" + name + "'). Use claude:summary, "
                "claude:component, claude:reviewed, etc.", true};
    }
    const char* argv[] = {"addattr", "-t", type.c_str(),
                          name.c_str(), value.c_str(),
                          path.c_str(), nullptr};
    return exec_capture(argv);
}

ToolResult run_index_attr(const json& input) {
    const std::string name = input.value("name", std::string{});
    const std::string type = input.value("type", std::string{});
    if (name.empty() || type.empty()) {
        return {"error: IndexAttr requires `name` and `type` arguments", true};
    }
    const char* argv[] = {"mkindex", "-t", type.c_str(),
                          name.c_str(), nullptr};
    return exec_capture(argv);
}

std::string preview_write_attr(const json& input) {
    const std::string path  = input.value("path", std::string{});
    const std::string name  = input.value("name", std::string{});
    const std::string value = input.value("value", std::string{});
    const std::string type  = input.value("type", std::string{"string"});
    return "  -> WriteAttr " + path + "\n"
         + "     " + name + " (" + type + ") = " + value;
}

std::string preview_index_attr(const json& input) {
    const std::string name = input.value("name", std::string{});
    const std::string type = input.value("type", std::string{});
    return "  -> IndexAttr: mkindex -t " + type + " " + name + "\n"
         + "     WARNING: creates a volume-level index";
}

json haiku_definitions() {
    return json::array({
        {
            {"name", "Query"},
            {"description",
                "Execute a BFS filesystem query and return matching file paths. "
                "Uses Haiku's indexed attribute system for O(1) lookups instead "
                "of directory traversal. Example expressions: "
                "'BEOS:TYPE == \"text/x-source-code\"', "
                "'name == \"*.cpp\" && last_modified > %1hour%', "
                "'claude:component == \"networking\"'. "
                "Returns one path per line, truncated at 32 KiB."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"expression", {
                        {"type", "string"},
                        {"description", "BFS query expression."},
                    }},
                    {"volume", {
                        {"type", "string"},
                        {"description", "Volume to search. Defaults to the volume containing cwd."},
                    }},
                }},
                {"required", json::array({"expression"})},
            }},
        },
        {
            {"name", "ReadAttr"},
            {"description",
                "Read extended attributes from a file on Haiku's BFS filesystem. "
                "Pass specific attribute names to read their values, or pass an "
                "empty names array to list all attributes and their types. "
                "Useful for reading claude:summary, BEOS:TYPE, Audio:Artist, "
                "and any other metadata stored on the file."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "File path to read attributes from."},
                    }},
                    {"names", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Attribute names to read. Empty or omitted = list all."},
                    }},
                }},
                {"required", json::array({"path"})},
            }},
        },
        {
            {"name", "WriteAttr"},
            {"description",
                "Write a typed extended attribute to a file on Haiku's BFS "
                "filesystem. Auto-approved — no permission prompt. Restricted "
                "to the claude:* namespace (claude:summary, claude:component, "
                "claude:reviewed, etc.) so it can't overwrite system "
                "attributes like BEOS:TYPE, MAIL:*, or Audio:*. Attributes "
                "survive git operations and branch switches, so use them to "
                "persist understanding across sessions."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "File path to write the attribute on."},
                    }},
                    {"name", {
                        {"type", "string"},
                        {"description", "Attribute name, e.g. 'claude:summary'."},
                    }},
                    {"type", {
                        {"type", "string"},
                        {"enum", {"string", "int32", "int64", "float", "double", "bool"}},
                        {"description", "Attribute type. Defaults to string."},
                    }},
                    {"value", {
                        {"type", "string"},
                        {"description", "Value to write."},
                    }},
                }},
                {"required", json::array({"path", "name", "value"})},
            }},
        },
        {
            {"name", "IndexAttr"},
            {"description",
                "Create a BFS attribute index on the current volume so that "
                "Query can search by this attribute in O(1). Only needs to be "
                "done once per attribute name per volume. Example: index "
                "'claude:component' as string so 'Query claude:component == "
                "\"networking\"' returns instantly. Requires user permission "
                "(modifies volume-level state)."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"name", {
                        {"type", "string"},
                        {"description", "Attribute name to index, e.g. 'claude:component'."},
                    }},
                    {"type", {
                        {"type", "string"},
                        {"enum", {"string", "int32", "int64", "float", "double"}},
                        {"description", "Index type."},
                    }},
                }},
                {"required", json::array({"name", "type"})},
            }},
        },
    });
}
#endif // __HAIKU__

} // namespace

json definitions() {
    json out = builtin_definitions();
    if (const char* key = std::getenv("BRAVE_SEARCH_API_KEY"); key && *key) {
        out.push_back({
            {"name", "WebSearch"},
            {"description",
                "Search the web via the Brave Search API. Returns up to 10 "
                "results as title/url/description blocks. Requires "
                "BRAVE_SEARCH_API_KEY in the environment."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"query", {
                        {"type", "string"},
                        {"description", "Search query string."},
                    }},
                }},
                {"required", json::array({"query"})},
            }},
        });
    }
#ifdef __HAIKU__
    for (const auto& t : haiku_definitions()) out.push_back(t);
#endif
    for (const auto& t : mcp::tool_definitions()) out.push_back(t);
    return out;
}

ToolResult run(const std::string& name, const json& input) {
    if (name == "Read")     return run_read(input);
    if (name == "Glob")     return run_glob(input);
    if (name == "Grep")     return run_grep(input);
    if (name == "Bash")     return run_bash(input);
    if (name == "Write")    return run_write(input);
    if (name == "Edit")     return run_edit(input);
    if (name == "WebFetch")  return run_webfetch(input);
    if (name == "WebSearch") return run_websearch(input);
    if (name == "TodoWrite") return run_todowrite(input);
    if (name == "TodoRead")  return run_todoread(input);
    if (name == "Task")      return {"error: Task is handled by the agent loop", true};
#ifdef __HAIKU__
    if (name == "Query")     return run_query(input);
    if (name == "ReadAttr")  return run_read_attr(input);
    if (name == "WriteAttr") return run_write_attr(input);
    if (name == "IndexAttr") return run_index_attr(input);
#endif
    if (auto mcp_res = mcp::run(name, input); mcp_res) return *mcp_res;
    return {"error: unknown tool " + name, true};
}

bool requires_permission(const std::string& name) {
    if (name == "Bash" || name == "Write" || name == "Edit") return true;
#ifdef __HAIKU__
    // WriteAttr is auto-approved — the claude:* namespace gate
    // inside run_write_attr is the safety layer now. IndexAttr
    // still prompts because it mutates volume-wide BFS state
    // beyond the claude:* scope.
    if (name == "IndexAttr") return true;
#endif
    if (mcp::is_mcp_tool(name)) return true;
    return false;
}

std::string preview(const std::string& name, const json& input) {
    if (name == "Write") return preview_write(input);
    if (name == "Edit")  return preview_edit(input);
#ifdef __HAIKU__
    if (name == "WriteAttr") return preview_write_attr(input);
    if (name == "IndexAttr") return preview_index_attr(input);
#endif
    return {};
}

} // namespace tools
