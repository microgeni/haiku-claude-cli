#include "cch/Tools.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace cch {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::Register(const std::string& name, ToolHandler handler)
{
	fHandlers[name] = std::move(handler);
}

bool ToolRegistry::Dispatch(const ToolCall& call, const StreamSink& liveSink,
                             ToolResult& out) const
{
	auto it = fHandlers.find(call.name);
	if (it == fHandlers.end()) return false;
	out = it->second(call, liveSink);
	return true;
}

std::string ToolRegistry::SchemaJson() const
{
	// The registry owns only the runtime dispatch table. The schema
	// (advertised to the model) is built separately by the consumer
	// (CLI or GUI) which knows which tools it wants to expose. Return
	// empty here; callers that need a schema build it from the
	// registered names and a static definition table.
	return "[]";
}

// ---------------------------------------------------------------------------
// Built-in tool implementations
// ---------------------------------------------------------------------------

namespace {

// ── Read ────────────────────────────────────────────────────────────────────

ToolResult run_read(const json& input)
{
	const std::string path = input.value("path", std::string{});
	if (path.empty())
		return {"error: Read requires a `path` argument", true};

	std::ifstream f(path);
	if (!f.is_open())
		return {"error: cannot open " + path, true};

	const int start = (input.contains("start_line") && input["start_line"].is_number())
	                ? input["start_line"].get<int>() : 0;
	const int end   = (input.contains("end_line")   && input["end_line"].is_number())
	                ? input["end_line"].get<int>()   : 0;

	std::ostringstream out;
	std::string        line;
	int                ln    = 0;
	bool               first = true;
	while (std::getline(f, line)) {
		++ln;
		if (start > 0 && ln < start) continue;
		if (end   > 0 && ln > end)   break;
		if (!first) out << '\n';
		out << line;
		first = false;
	}
	if (first)
		return {"(empty or out-of-range read at " + path + ")", false};
	return {out.str(), false};
}

// ── Write ───────────────────────────────────────────────────────────────────

static bool ensure_parent_dir(const std::string& file_path)
{
	const auto slash = file_path.rfind('/');
	if (slash == std::string::npos || slash == 0) return true;
	const std::string dir = file_path.substr(0, slash);
	// Walk each component and mkdir as needed.
	for (size_t i = 1; i <= dir.size(); ++i) {
		if (i == dir.size() || dir[i] == '/') {
			const std::string part = dir.substr(0, i);
			if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST)
				return false;
		}
	}
	return true;
}

ToolResult run_write(const json& input)
{
	const std::string path = input.value("path", std::string{});
	if (path.empty())
		return {"error: Write requires a `path` argument", true};
	if (!input.contains("content"))
		return {"error: Write requires a `content` argument", true};
	const std::string content = input.value("content", std::string{});

	if (!ensure_parent_dir(path))
		return {"error: cannot create parent directory for " + path, true};

	std::ofstream f(path, std::ios::binary);
	if (!f.is_open())
		return {"error: cannot open " + path + " for writing", true};
	f.write(content.data(), static_cast<std::streamsize>(content.size()));
	f.close();
	if (!f)
		return {"error: write to " + path + " failed", true};

	return {"wrote " + std::to_string(content.size()) + " bytes to " + path, false};
}

// ── Edit ────────────────────────────────────────────────────────────────────

static std::string read_file_all(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) return {};
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static size_t count_occurrences(const std::string& haystack,
                                const std::string& needle)
{
	if (needle.empty()) return 0;
	size_t count = 0, pos = 0;
	while ((pos = haystack.find(needle, pos)) != std::string::npos) {
		++count;
		pos += needle.size();
	}
	return count;
}

ToolResult run_edit(const json& input)
{
	const std::string path      = input.value("path",       std::string{});
	const std::string old_s     = input.value("old_string", std::string{});
	const std::string new_s     = input.value("new_string", std::string{});
	const bool        replace_all = input.value("replace_all", false);

	if (path.empty())  return {"error: Edit requires a `path` argument", true};
	if (old_s.empty()) return {"error: Edit requires a non-empty `old_string`", true};

	std::string content = read_file_all(path);
	if (content.empty()) {
		if (!std::ifstream(path).is_open())
			return {"error: cannot open " + path, true};
	}

	const size_t count = count_occurrences(content, old_s);
	if (count == 0)
		return {"error: old_string not found in " + path, true};
	if (count > 1 && !replace_all)
		return {"error: old_string matches " + std::to_string(count)
		        + " times in " + path + "; set replace_all=true to replace all", true};

	std::string out;
	out.reserve(content.size());
	size_t start = 0, match;
	while ((match = content.find(old_s, start)) != std::string::npos) {
		out.append(content, start, match - start);
		out.append(new_s);
		start = match + old_s.size();
		if (!replace_all) break;
	}
	out.append(content, start, std::string::npos);

	std::ofstream of(path, std::ios::binary);
	if (!of.is_open())
		return {"error: cannot open " + path + " for writing", true};
	of.write(out.data(), static_cast<std::streamsize>(out.size()));
	of.close();
	if (!of)
		return {"error: write to " + path + " failed", true};

	const std::string plural = (count == 1) ? "" : "s";
	return {"edited " + path + " (" + std::to_string(count)
	        + " replacement" + plural + ")", false};
}

// ── Glob ────────────────────────────────────────────────────────────────────

ToolResult run_glob(const json& input)
{
	const std::string pattern = input.value("pattern", std::string{});
	if (pattern.empty())
		return {"error: Glob requires a `pattern` argument", true};

	glob_t results {};
	const int rc = ::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &results);
	if (rc == GLOB_NOMATCH) {
		::globfree(&results);
		return {"(no matches for " + pattern + ")", false};
	}
	if (rc != 0) {
		::globfree(&results);
		return {"error: glob failed", true};
	}

	struct Entry { std::string path; time_t mtime = 0; };
	std::vector<Entry> entries;
	entries.reserve(results.gl_pathc);
	for (size_t i = 0; i < results.gl_pathc; ++i) {
		Entry e;
		e.path = results.gl_pathv[i];
		struct stat st {};
		if (::stat(e.path.c_str(), &st) == 0) e.mtime = st.st_mtime;
		entries.push_back(std::move(e));
	}
	::globfree(&results);

	std::sort(entries.begin(), entries.end(),
	          [](const Entry& a, const Entry& b){ return a.mtime > b.mtime; });

	std::ostringstream out;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (i > 0) out << '\n';
		out << entries[i].path;
	}
	return {out.str(), false};
}

// ── Grep ────────────────────────────────────────────────────────────────────

ToolResult run_grep(const json& input)
{
	const std::string pattern         = input.value("pattern",          std::string{});
	const std::string path            = input.value("path",             std::string{"."});
	const bool        case_insensitive = input.value("case_insensitive", false);

	if (pattern.empty())
		return {"error: Grep requires a `pattern` argument", true};

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
	if (::pipe(pipefd) != 0)
		return {"error: pipe() failed", true};
	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(pipefd[0]); ::close(pipefd[1]);
		return {"error: fork() failed", true};
	}
	if (pid == 0) {
		::setsid();
		const int devnull = ::open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			::dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO) ::close(devnull);
		}
		::close(pipefd[0]);
		::dup2(pipefd[1], STDOUT_FILENO);
		::dup2(pipefd[1], STDERR_FILENO);
		::close(pipefd[1]);
		::execvp("grep", const_cast<char* const*>(argv.data()));
		_exit(127);
	}
	::close(pipefd[1]);

	std::string output;
	char        buf[4096];
	while (true) {
		struct pollfd pfd {};
		pfd.fd = pipefd[0]; pfd.events = POLLIN;
		if (::poll(&pfd, 1, 5000) <= 0) break;
		if (!(pfd.revents & POLLIN)) break;
		const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break;
		output.append(buf, static_cast<size_t>(n));
	}
	::close(pipefd[0]);

	int status = 0;
	::waitpid(pid, &status, 0);

	if (!WIFEXITED(status))
		return {"error: grep terminated abnormally", true};
	const int code = WEXITSTATUS(status);
	if (code == 1)
		return {"(no matches for " + pattern + " in " + path + ")", false};
	if (code != 0)
		return {"error: grep exited with code " + std::to_string(code), true};

	constexpr size_t kMaxBytes = 32 * 1024;
	if (output.size() > kMaxBytes) {
		output.resize(kMaxBytes);
		output += "\n[... output truncated]";
	}
	return {output, false};
}

// ── Bash ────────────────────────────────────────────────────────────────────

ToolResult run_bash(const json& input, const StreamSink& liveSink)
{
	const std::string command = input.value("command", std::string{});
	const int timeout = (input.contains("timeout_seconds")
	                     && input["timeout_seconds"].is_number())
	                  ? input["timeout_seconds"].get<int>() : 60;
	if (command.empty())
		return {"error: Bash requires a `command` argument", true};

	int pipefd[2];
	if (::pipe(pipefd) != 0)
		return {"error: pipe() failed", true};
	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(pipefd[0]); ::close(pipefd[1]);
		return {"error: fork() failed", true};
	}
	if (pid == 0) {
		::setsid();
		const int devnull = ::open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			::dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO) ::close(devnull);
		}
		::close(pipefd[0]);
		::dup2(pipefd[1], STDOUT_FILENO);
		::dup2(pipefd[1], STDERR_FILENO);
		::close(pipefd[1]);
		const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
		::execvp("sh", const_cast<char* const*>(argv));
		_exit(127);
	}
	::close(pipefd[1]);

	const auto start_time = std::chrono::steady_clock::now();
	std::string output;
	char        buf[4096];
	bool        killed = false;

	while (true) {
		if (timeout > 0) {
			const double elapsed = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - start_time).count();
			if (elapsed >= static_cast<double>(timeout)) {
				killed = true;
				break;
			}
		}
		struct pollfd pfd {};
		pfd.fd = pipefd[0]; pfd.events = POLLIN;
		const int r = ::poll(&pfd, 1, 100);
		if (r < 0) { if (errno == EINTR) continue; break; }
		if (r == 0) continue;
		if (!(pfd.revents & POLLIN)) break;
		const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break;
		const std::string chunk(buf, static_cast<size_t>(n));
		output += chunk;
		// Stream live output to the UI so the user sees progress.
		if (liveSink.onChunk) liveSink.onChunk(chunk);
	}
	::close(pipefd[0]);

	if (killed) {
		::kill(-pid, SIGTERM);
		struct timespec ts { 0, 200'000'000 };
		::nanosleep(&ts, nullptr);
		::kill(-pid, SIGKILL);
	}
	int status = 0;
	::waitpid(pid, &status, 0);

	if (killed)
		return {"error: command timed out after " + std::to_string(timeout)
		        + "s\n" + output, true};

	constexpr size_t kMaxBytes = 32 * 1024;
	if (output.size() > kMaxBytes) {
		output.resize(kMaxBytes);
		output += "\n[... output truncated]";
	}
	if (!WIFEXITED(status))
		return {"error: shell terminated abnormally", true};
	const int code = WEXITSTATUS(status);
	if (code != 0)
		return {"exit " + std::to_string(code) + "\n" + output, true};
	return {output.empty() ? "(no output)" : output, false};
}

// ── WebFetch ─────────────────────────────────────────────────────────────────

static size_t append_to_string(void* ptr, size_t size, size_t nmemb, void* userp)
{
	auto* out = static_cast<std::string*>(userp);
	out->append(static_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}

ToolResult run_webfetch(const json& input)
{
	const std::string url = input.value("url", std::string{});
	if (url.empty())
		return {"error: WebFetch requires a `url` argument", true};
	const int max_bytes = (input.contains("max_bytes")
	                       && input["max_bytes"].is_number())
	                    ? input["max_bytes"].get<int>() : 32 * 1024;

	CURL* curl = curl_easy_init();
	if (!curl) return {"error: curl_easy_init failed", true};

	std::string body;
	curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  append_to_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,      "haiku-claude-cli/core");
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

	const CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	char* ct = nullptr;
	curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE,  &ct);
	std::string content_type = ct ? ct : "";
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
		return {std::string("error: ") + curl_easy_strerror(res), true};
	if (http_code < 200 || http_code >= 400)
		return {"error: HTTP " + std::to_string(http_code), true};

	const bool truncated = static_cast<int>(body.size()) > max_bytes;
	if (truncated) body.resize(static_cast<size_t>(max_bytes));

	std::string out = "HTTP " + std::to_string(http_code);
	if (!content_type.empty()) out += "  (" + content_type + ")";
	out += "\n\n" + body;
	if (truncated)
		out += "\n\n[... truncated at " + std::to_string(max_bytes) + " bytes]";
	return {out, false};
}

// ── WebSearch ────────────────────────────────────────────────────────────────

ToolResult run_websearch(const json& input)
{
	const std::string query = input.value("query", std::string{});
	if (query.empty())
		return {"error: WebSearch requires a `query` argument", true};
	const char* key = std::getenv("BRAVE_SEARCH_API_KEY");
	if (!key || !*key)
		return {"error: BRAVE_SEARCH_API_KEY is not set", true};

	CURL* curl = curl_easy_init();
	if (!curl) return {"error: curl_easy_init failed", true};

	char* escaped = curl_easy_escape(
		curl, query.c_str(), static_cast<int>(query.size()));
	std::string url = "https://api.search.brave.com/res/v1/web/search?q=";
	url += escaped ? escaped : "";
	curl_free(escaped);

	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers,
		(std::string("x-subscription-token: ") + key).c_str());
	headers = curl_slist_append(headers, "accept: application/json");

	std::string body;
	curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,     "haiku-claude-cli/core");
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

	const CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
		return {std::string("error: ") + curl_easy_strerror(res), true};
	if (http_code != 200)
		return {"error: Brave Search HTTP " + std::to_string(http_code), true};

	try {
		const json j = json::parse(body);
		if (!j.contains("web") || !j["web"].contains("results")
			|| !j["web"]["results"].is_array())
			return {"(no results)", false};
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
		return {std::string("error: ") + e.what(), true};
	}
}

// ── TodoWrite / TodoRead ─────────────────────────────────────────────────────

struct TodoItem {
	std::string content;
	std::string status;
};
static std::vector<TodoItem> g_todos;

static std::string format_todos()
{
	if (g_todos.empty()) return "(no todos)";
	std::ostringstream out;
	for (size_t i = 0; i < g_todos.size(); ++i) {
		const char* mark = "[ ]";
		if (g_todos[i].status == "completed")    mark = "[x]";
		else if (g_todos[i].status == "in_progress") mark = "[-]";
		if (i > 0) out << "\n";
		out << mark << " " << g_todos[i].content;
	}
	return out.str();
}

ToolResult run_todowrite(const json& input)
{
	if (!input.contains("todos") || !input["todos"].is_array())
		return {"error: TodoWrite requires a `todos` array", true};
	g_todos.clear();
	for (const auto& t : input["todos"]) {
		if (!t.is_object()) continue;
		TodoItem item;
		item.content = t.value("content", std::string{});
		item.status  = t.value("status",  std::string{"pending"});
		if (item.content.empty()) continue;
		g_todos.push_back(std::move(item));
	}
	return {"updated todo list (" + std::to_string(g_todos.size()) + " items)\n"
	        + format_todos(), false};
}

ToolResult run_todoread(const json& /*input*/)
{
	return {format_todos(), false};
}

// ── Haiku BFS tools ──────────────────────────────────────────────────────────

#ifdef __HAIKU__

static ToolResult exec_capture(const char* const argv[])
{
	int pipefd[2];
	if (::pipe(pipefd) != 0)
		return {"error: pipe() failed", true};
	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(pipefd[0]); ::close(pipefd[1]);
		return {"error: fork() failed", true};
	}
	if (pid == 0) {
		::setsid();
		const int devnull = ::open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			::dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO) ::close(devnull);
		}
		::close(pipefd[0]);
		::dup2(pipefd[1], STDOUT_FILENO);
		::dup2(pipefd[1], STDERR_FILENO);
		::close(pipefd[1]);
		::execvp(argv[0], const_cast<char* const*>(argv));
		_exit(127);
	}
	::close(pipefd[1]);

	std::string output;
	char buf[4096];
	while (true) {
		struct pollfd pfd {};
		pfd.fd = pipefd[0]; pfd.events = POLLIN;
		if (::poll(&pfd, 1, 5000) <= 0) break;
		if (!(pfd.revents & POLLIN)) break;
		const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break;
		output.append(buf, static_cast<size_t>(n));
	}
	::close(pipefd[0]);

	int status = 0;
	::waitpid(pid, &status, 0);

	constexpr size_t kMaxBytes = 32 * 1024;
	if (output.size() > kMaxBytes) {
		output.resize(kMaxBytes);
		output += "\n[... output truncated]";
	}
	if (!WIFEXITED(status))
		return {"error: command terminated abnormally\n" + output, true};
	const int code = WEXITSTATUS(status);
	if (code != 0 && output.empty())
		return {"exit " + std::to_string(code), true};
	if (code != 0)
		return {"exit " + std::to_string(code) + "\n" + output, true};
	return {output.empty() ? "(no output)" : output, false};
}

ToolResult run_query(const json& input)
{
	const std::string expr = input.value("expression", std::string{});
	if (expr.empty())
		return {"error: Query requires an `expression` argument", true};
	const std::string vol = input.value("volume", std::string{});
	if (vol.empty()) {
		const char* argv[] = {"query", expr.c_str(), nullptr};
		return exec_capture(argv);
	}
	const char* argv[] = {"query", "-v", vol.c_str(), expr.c_str(), nullptr};
	return exec_capture(argv);
}

ToolResult run_read_attr(const json& input)
{
	const std::string path = input.value("path", std::string{});
	if (path.empty())
		return {"error: ReadAttr requires a `path` argument", true};
	const auto names = input.value("Names", std::vector<std::string>{});
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
		if (result.isError) combined += " [error]";
	}
	return {combined, false};
}

ToolResult run_write_attr(const json& input)
{
	const std::string path  = input.value("path",  std::string{});
	const std::string name  = input.value("name",  std::string{});
	const std::string value = input.value("value", std::string{});
	const std::string type  = input.value("type",  std::string{"string"});
	if (path.empty() || name.empty())
		return {"error: WriteAttr requires `path` and `name` arguments", true};
	// Restrict to the claude:* namespace so system attributes can't
	// be overwritten accidentally.
	if (name.rfind("claude:", 0) != 0)
		return {"error: WriteAttr is restricted to the claude:* namespace", true};
	const char* argv[] = {"addattr", "-t", type.c_str(),
	                      name.c_str(), value.c_str(), path.c_str(), nullptr};
	return exec_capture(argv);
}

ToolResult run_index_attr(const json& input)
{
	const std::string name = input.value("name", std::string{});
	const std::string type = input.value("type", std::string{});
	if (name.empty() || type.empty())
		return {"error: IndexAttr requires `name` and `type` arguments", true};
	const char* argv[] = {"mkindex", "-t", type.c_str(), name.c_str(), nullptr};
	return exec_capture(argv);
}

#endif // __HAIKU__

} // anonymous namespace

// ---------------------------------------------------------------------------
// RegisterBuiltins — wire all built-in tool names into a registry
// ---------------------------------------------------------------------------

// Helper: parse the stored JSON input string back into a json object.
static json parse_input(const ToolCall& call)
{
	try {
		if (!call.input.empty()) return json::parse(call.input);
	} catch (const json::exception&) {}
	return json::object();
}

void RegisterBuiltins(ToolRegistry& reg)
{
	reg.Register("Read",  [](const ToolCall& c, const StreamSink&) {
		return run_read(parse_input(c));
	});
	reg.Register("Write", [](const ToolCall& c, const StreamSink&) {
		return run_write(parse_input(c));
	});
	reg.Register("Edit",  [](const ToolCall& c, const StreamSink&) {
		return run_edit(parse_input(c));
	});
	reg.Register("Glob",  [](const ToolCall& c, const StreamSink&) {
		return run_glob(parse_input(c));
	});
	reg.Register("Grep",  [](const ToolCall& c, const StreamSink&) {
		return run_grep(parse_input(c));
	});
	reg.Register("Bash",  [](const ToolCall& c, const StreamSink& sink) {
		return run_bash(parse_input(c), sink);
	});
	reg.Register("WebFetch",  [](const ToolCall& c, const StreamSink&) {
		return run_webfetch(parse_input(c));
	});
	reg.Register("WebSearch", [](const ToolCall& c, const StreamSink&) {
		return run_websearch(parse_input(c));
	});
	reg.Register("TodoWrite", [](const ToolCall& c, const StreamSink&) {
		return run_todowrite(parse_input(c));
	});
	reg.Register("TodoRead",  [](const ToolCall& c, const StreamSink&) {
		return run_todoread(parse_input(c));
	});
#ifdef __HAIKU__
	reg.Register("Query",     [](const ToolCall& c, const StreamSink&) {
		return run_query(parse_input(c));
	});
	reg.Register("ReadAttr",  [](const ToolCall& c, const StreamSink&) {
		return run_read_attr(parse_input(c));
	});
	reg.Register("WriteAttr", [](const ToolCall& c, const StreamSink&) {
		return run_write_attr(parse_input(c));
	});
	reg.Register("IndexAttr", [](const ToolCall& c, const StreamSink&) {
		return run_index_attr(parse_input(c));
	});
#endif
}

} // namespace cch
