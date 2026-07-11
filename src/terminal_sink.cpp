#include "terminal_sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include "api.h"
#include "repl.h"
#include "tools.h"
#include "tui.h"

// ---------------------------------------------------------------------------
// Forward declarations of globals from api.cpp shared by TerminalSink.
// ---------------------------------------------------------------------------

extern volatile sig_atomic_t g_interrupted;
extern volatile sig_atomic_t g_cancel_retype;

// api.h declares these; they are defined in api.cpp.
// No extra declarations needed here.

// ---------------------------------------------------------------------------
// TerminalSink
// ---------------------------------------------------------------------------

TerminalSink::TerminalSink()
{
	fSpinner = new tui::Spinner("thinking");
	fRenderer.SetSpinner(fSpinner);
	fRenderer.SetResponsePrefix(tui::ClaudePrompt());
}

TerminalSink::~TerminalSink()
{
	if (fSpinner) {
		fSpinner->Stop();
		delete fSpinner;
		fSpinner = nullptr;
	}
}

void TerminalSink::SetLiveInputTokens(std::atomic<int>* counter)
{
	if (fSpinner) fSpinner->SetLiveInputTokens(counter);
}

void TerminalSink::OnText(const std::string& chunk)
{
	fRenderer.Write(chunk);
}

void TerminalSink::OnMeta(const std::string& text)
{
	std::cout << tui::Meta(text) << "\n";
}

void TerminalSink::OnDiag(const std::string& text)
{
	std::cerr << tui::Dim(text) << "\n";
}

void TerminalSink::OnError(const std::string& text)
{
	std::cerr << "\n" << tui::ErrorLabel() << " " << text << "\n";
}

void TerminalSink::OnToolStatus(const std::string& phase)
{
	// For the terminal, the thinking spinner stops when a tool starts.
	// The "[tool: X ...]" meta line already announces the tool.
	// A new per-tool spinner is created by SendWithTools directly for now.
	if (phase.empty() && fSpinner) {
		fSpinner->Stop();
	}
	// Non-empty phase during tool execution: the spinner may have already
	// been stopped; nothing to update in the terminal model.
	(void)phase;
}

void TerminalSink::Flush()
{
	if (fSpinner) { fSpinner->Stop(); }
	fRenderer.Flush();
}

// ---------------------------------------------------------------------------
// AskPermission — lifted from PromptPermission() in api.cpp
// ---------------------------------------------------------------------------

// Build a short one-line summary of a tool's input for display.
static std::string ShortInputSummary(const nlohmann::json& input)
{
	if (input.contains("command") && input["command"].is_string())
		return input["command"].get<std::string>();
	const std::string dumped = input.dump(-1, ' ', false,
	    nlohmann::json::error_handler_t::replace);
	if (dumped.size() <= 80) return dumped;
	return dumped.substr(0, 77) + "...";
}

api::Permission TerminalSink::AskPermission(const std::string& tool_name,
                                             const nlohmann::json& input,
                                             std::string* denial_reason)
{
	using Permission = api::Permission;

	if (api::AlwaysAllowed().count(tool_name)) return Permission::Allow;
	if (!tools::RequiresPermission(tool_name))  return Permission::Allow;

	// Ludicrous mode: auto-approve all tools.
	if (api::g_ludicrous_mode.load()) {
		std::cout << tui::Dim("  \xE2\x9A\xA1 ludicrous: auto-approved " + tool_name) << "\n";
		return Permission::Allow;
	}

	// One-shot / piped stdin without a usable TTY.
	if (!isatty(fileno(stdin))) {
		if (api::g_allow_destructive_tools) {
			api::AlwaysAllowed().insert(tool_name);
			return Permission::Allow;
		}
		const std::string msg =
			"cannot prompt for " + tool_name + " permission: stdin is not a "
			"terminal. Re-run with -y/--yes to auto-approve destructive tools, "
			"set \"fAllowDestructiveTools\": true in config.json, or use "
			"-i/--interactive from a real terminal.";
		std::cerr << tui::Meta("[tool: " + tool_name + " -> denied: no TTY to prompt]") << "\n"
		          << tui::Dim("  " + msg) << "\n";
		if (denial_reason) *denial_reason = msg;
		return Permission::Deny;
	}

	// Full interactive SelectOption prompt.
	const std::string extra = tools::Preview(tool_name, input);
	int pre_lines = 0;
	if (!extra.empty()) {
		std::cout << tui::Dim(extra) << "\n";
		pre_lines = static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1;
	} else {
		const std::string action_line = "  \xE2\x86\x92 " + tool_name
		                              + "  " + ShortInputSummary(input);
		std::cout << tui::Meta(action_line) << "\n";
		pre_lines = 1;
	}

	const std::string file_path = input.value("path", std::string{});
	const auto slash = file_path.rfind('/');
	const std::string basename = (slash == std::string::npos)
	    ? file_path : file_path.substr(slash + 1);
	const auto prev_slash = (slash == std::string::npos || slash == 0)
	    ? std::string::npos : file_path.rfind('/', slash - 1);
	const std::string dir_scope = (!file_path.empty() && slash != std::string::npos)
	    ? file_path.substr(prev_slash == std::string::npos ? 0 : prev_slash + 1,
	                       slash - (prev_slash == std::string::npos ? 0 : prev_slash))
	    : std::string{};

	const std::string question = basename.empty()
	    ? "Do you want to proceed with " + tool_name + "?"
	    : "Do you want to make this edit to " + basename + "?";
	const std::string allow_session_label = dir_scope.empty()
	    ? "Yes, allow all " + tool_name + " this session  (shift+tab)"
	    : "Yes, allow all " + tool_name + " in " + dir_scope + " this session  (shift+tab)";
	const std::vector<std::string> choices = {
		"Yes",
		allow_session_label,
		"\xE2\x9A\xA1 Enable ludicrous mode (auto-approve all tools this session)",
		"No",
	};

	if (api::g_active_esc_guard) api::g_active_esc_guard->pause();
	repl::BlockStdin();
	tui::SuspendScrollRegion();
	const int picked = tui::SelectOption(choices, question, nullptr, pre_lines);
	tui::RestoreScrollRegion();
	if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
	repl::UnblockStdin();
	repl::RequestClearEditBuffer();
	if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
	if (api::g_active_esc_guard) api::g_active_esc_guard->resume();
	tui::PositionCursorForChat();

	if (g_interrupted && picked >= static_cast<int>(choices.size()) - 1) {
		if (denial_reason) *denial_reason = "interrupted — permission denied for " + tool_name;
		return Permission::Deny;
	}
	if (picked == -2) {
		g_cancel_retype = 1;
		g_interrupted   = 1;
		if (denial_reason) *denial_reason = "user chose to amend the prompt";
		return Permission::Deny;
	}
	// "Yes, allow all X this session"
	if (picked == 1) {
		api::AlwaysAllowed().insert(tool_name);
		return Permission::Allow;
	}
	// "⚡ Enable ludicrous mode"
	if (picked == 2) {
		api::g_ludicrous_mode.store(true);
		std::cout << tui::Yellow("\xE2\x9A\xA1 LUDICROUS MODE ENGAGED")
		          << tui::Dim(" \xe2\x80\x94 all tool permissions auto-approved") << "\n";
		return Permission::Allow;
	}
	// "Yes" (allow once)
	if (picked == 0) return Permission::Allow;
	// "No" (deny)
	if (denial_reason)
		*denial_reason = "user declined permission for " + tool_name;
	return Permission::Deny;
}
