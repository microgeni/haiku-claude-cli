#include "hooks.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace hooks {

namespace {

struct Hook {
	std::string matcher; // empty = match any tool
	std::string command;
};

std::map<Event, std::vector<Hook>> g_hooks;

const char* event_name(Event e) {
	switch (e) {
		case Event::SessionStart:     return "SessionStart";
		case Event::UserPromptSubmit: return "UserPromptSubmit";
		case Event::PreToolUse:       return "PreToolUse";
		case Event::PostToolUse:      return "PostToolUse";
		case Event::Stop:             return "Stop";
	}
	return "";
}

std::optional<Event> parse_event_name(const std::string& name) {
	if (name == "SessionStart")     return Event::SessionStart;
	if (name == "UserPromptSubmit") return Event::UserPromptSubmit;
	if (name == "PreToolUse")       return Event::PreToolUse;
	if (name == "PostToolUse")      return Event::PostToolUse;
	if (name == "Stop")             return Event::Stop;
	return std::nullopt;
}

int run_one(const std::string& command, const std::string& stdin_payload) {
	int in_pipe[2];
	int err_pipe[2];
	if (pipe(in_pipe) != 0) return -1;
	if (pipe(err_pipe) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}

	const pid_t pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		return -1;
	}

	if (pid == 0) {
		close(in_pipe[1]);
		if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(126);
		close(in_pipe[0]);

		close(err_pipe[0]);
		if (dup2(err_pipe[1], STDERR_FILENO) < 0) _exit(126);
		close(err_pipe[1]);

		const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
		execvp("sh", const_cast<char* const*>(argv));  // flawfinder: ignore
		_exit(127);
	}

	close(in_pipe[0]);
	close(err_pipe[1]);

	if (!stdin_payload.empty()) {
		size_t off = 0;
		while (off < stdin_payload.size()) {
			const ssize_t n = write(in_pipe[1],
									stdin_payload.data() + off,
									stdin_payload.size() - off);
			if (n <= 0) break;
			off += static_cast<size_t>(n);
		}
	}
	close(in_pipe[1]);

	std::string err_output;
	char        buf[4096];
	ssize_t     n;
	while ((n = read(err_pipe[0], buf, sizeof(buf))) > 0) {
		err_output.append(buf, static_cast<size_t>(n));
	}
	close(err_pipe[0]);

	int status = 0;
	waitpid(pid, &status, 0);

	if (!err_output.empty()) {
		std::cerr << err_output;
		if (err_output.back() != '\n') std::cerr << '\n';
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

void Load(const json& config_hooks) {
	g_hooks.clear();
	if (!config_hooks.is_object()) return;

	for (auto it = config_hooks.begin(); it != config_hooks.end(); ++it) {
		const auto event = parse_event_name(it.key());
		if (!event) continue;
		const auto& list = it.value();
		if (!list.is_array()) continue;

		for (const auto& entry : list) {
			if (!entry.is_object()) continue;
			Hook h;
			h.matcher = entry.value("matcher", std::string{});
			h.command = entry.value("command", std::string{});
			if (h.command.empty()) continue;
			g_hooks[*event].push_back(std::move(h));
		}
	}
}

Outcome Fire(Event event, const json& payload, const std::string& tool_name) {
	const auto it = g_hooks.find(event);
	if (it == g_hooks.end()) return Outcome::Proceed;

	bool blocked = false;
	for (const Hook& h : it->second) {
		if (!h.matcher.empty() && h.matcher != tool_name) continue;

		json enriched = payload.is_null() ? json::object() : payload;
		enriched["event"] = event_name(event);
		if (!tool_name.empty()) enriched["tool_name"] = tool_name;

		const int rc = run_one(h.command,
		    enriched.dump(-1, ' ', false, json::error_handler_t::replace));
		if (rc != 0) blocked = true;
	}
	return blocked ? Outcome::Block : Outcome::Proceed;
}

} // namespace hooks
