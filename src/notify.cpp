#include "notify.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifdef __HAIKU__
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace notify {

std::vector<std::string> ExtractUrls(const std::string& text) {
	std::vector<std::string> out;
	const char* schemes[] = { "http://", "https://" };
	size_t pos = 0;
	while (pos < text.size()) {
		size_t best = std::string::npos;
		for (const char* s : schemes) {
			const auto p = text.find(s, pos);
			if (p < best) best = p;
		}
		if (best == std::string::npos) break;
		size_t end = best;
		while (end < text.size()) {
			const char c = text[end];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
				c == '<' || c == '>' || c == '"' || c == '\'' ||
				c == '(' || c == ')' || c == '[' || c == ']' ||
				c == '{' || c == '}' || c == '|' || c == '`') break;
			++end;
		}
		std::string url = text.substr(best, end - best);
		while (!url.empty()) {
			const char c = url.back();
			if (c == '.' || c == ',' || c == ';' || c == ':' ||
				c == '!' || c == '?') url.pop_back();
			else break;
		}
		if (url.size() > 10) out.push_back(std::move(url));
		pos = end;
	}
	return out;
}

std::string FirstSentence(const std::string& text, size_t max_chars) {
	std::string body;
	body.reserve(text.size());
	bool prevSpace = true;
	for (char c : text) {
		if (c == '\n' || c == '\r' || c == '\t') c = ' ';
		if (c == ' ' && prevSpace) continue;
		body += c;
		prevSpace = (c == ' ');
	}
	while (!body.empty() && body.back() == ' ') body.pop_back();

	const auto punct = body.find_first_of(".!?\n");
	if (punct != std::string::npos && punct + 1 <= max_chars) {
		body.resize(punct + 1);
	}
	if (body.size() > max_chars) {
		body.resize(max_chars);
		body += "\xE2\x80\xA6"; // UTF-8 ellipsis
	}
	if (body.empty()) body = "(empty response)";
	return body;
}

// Locate the canonical Claude icon at runtime. Checked in order:
// the source tree (dev), the HPKG-installed data dir, and the
// non-packaged install dir. First match wins; empty return means
// "skip the --icon flag."
#ifdef __HAIKU__
namespace {
std::string find_claude_icon_path() {
	static const char* const candidates[] = {
		"assets/claude-icon.hvif",
		"/boot/system/data/claude-cli/icon.hvif",
		"/boot/system/non-packaged/data/claude-cli/icon.hvif",
		"assets/claude-icon-Preview.png",
		nullptr
	};
	for (int i = 0; candidates[i]; ++i) {
		struct stat st;
		if (::stat(candidates[i], &st) == 0) return candidates[i];
	}
	return {};
}
} // namespace
#endif

std::string PickPlayfulTitle(double elapsed_seconds) {
	static const char* const titles[] = {
		"Pondering complete",
		"Cogitation concluded",
		"Musing resolved",
		"Contemplation cleared",
		"Reverie returned",
		"Reflection ready",
		"Deliberation delivered",
		"Rumination wrapped",
		"Thought crystallized",
		"Percolating done",
		"Brewing finished",
		"Thinking through",
	};
	constexpr int n = sizeof(titles) / sizeof(titles[0]);
	const int idx = static_cast<int>(std::time(nullptr)) % n;
	char out[128];
	std::snprintf(out, sizeof(out), "%s (%.0fs)", titles[idx], elapsed_seconds);
	return out;
}

void Send(const std::string& title, const std::string& body) {
#ifdef __HAIKU__
	const std::string icon = find_claude_icon_path();
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		// Detach from the parent's session + controlling terminal.
		// Without this, `notify` inherits the REPL's cbreak termios
		// and scroll-region state and the BMessage dispatch to
		// notification_server silently drops the alert even though
		// the child exits with status 0.
		setsid();
		int devnull = ::open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}

		// Haiku's `notify` doesn't support `--` as an
		// end-of-options sentinel — it errors with "Unrecognized
		// option --" and the notification never fires. Put the
		// body straight after the flags. --icon is added
		// conditionally so we don't hand notify a missing path.
		std::vector<const char*> argv;
		argv.reserve(16);
		argv.push_back("notify");
		argv.push_back("--type");    argv.push_back("information");
		argv.push_back("--group");   argv.push_back("Claude CLI");
		argv.push_back("--title");   argv.push_back(title.c_str());
		argv.push_back("--timeout"); argv.push_back("8");
		if (!icon.empty()) {
			argv.push_back("--icon");
			argv.push_back(icon.c_str());
		}
		argv.push_back(body.c_str());
		argv.push_back(nullptr);
		execvp("notify", const_cast<char* const*>(argv.data()));  // flawfinder: ignore
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
#else
	(void)title;
	(void)body;
#endif
}

} // namespace notify
