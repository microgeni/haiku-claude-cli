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

	// Telegram remote-control hook.
	if (api::g_telegram_permission_hook) {
		const std::string extra   = tools::Preview(tool_name, input);
		const std::string preview = extra.empty()
			? (tool_name + " " + ShortInputSummary(input))
			: extra;
		if (!extra.empty()) std::cout << tui::Dim(extra) << "\n";
		else std::cout << tui::Meta("  -> " + tool_name + " " + ShortInputSummary(input)) << "\n";

		const bool has_tty = isatty(fileno(stdin)) && isatty(fileno(stdout));
		if (!has_tty || api::g_non_interactive_tools) {
			std::cout << tui::Bold("allow " + tool_name + "? ")
			          << tui::Dim("[awaiting Telegram response]") << "\n" << std::flush;
			const Permission p = api::g_telegram_permission_hook(tool_name, preview, nullptr);
			std::cout << tui::Dim(std::string("  -> ") + (p == Permission::Allow ? "yes" : "no")) << "\n";
			return p;
		}

		// Race: local SelectOption vs Telegram inline keyboard.
		std::atomic<bool> local_answered { false };
		std::atomic<bool> tg_answered    { false };
		Permission         result         { Permission::Deny };
		std::mutex         race_mu;
		std::condition_variable race_cv;

		auto tg_hook = api::g_telegram_permission_hook;
		std::thread tg_thread([&]() {
			const Permission p = tg_hook(tool_name, preview, &local_answered);
			std::unique_lock<std::mutex> lk(race_mu);
			if (!local_answered.load()) {
				result = p;
				tg_answered.store(true);
			}
			race_cv.notify_one();
		});

		const std::vector<std::string> choices = {
			"Yes",
			"Yes, allow all " + tool_name + " this session  (shift+tab)",
			"No",
		};
		std::cout << tui::Dim("[also awaiting Telegram — or answer locally]") << "\n" << std::flush;
		if (api::g_active_esc_guard) api::g_active_esc_guard->pause();
		repl::BlockStdin();
		tui::PauseFlushTimer();
		tui::SuspendScrollRegion();
		const int race_pre_lines =
			(extra.empty()
				? 1
				: static_cast<int>(std::count(extra.begin(), extra.end(), '\n')) + 1)
			+ 1;
		const int picked = tui::SelectOption(choices, "allow " + tool_name + "?",
		                                     &tg_answered, race_pre_lines);
		tui::RestoreScrollRegion();
		tui::ResumeFlushTimer();
		if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
		repl::UnblockStdin();
		repl::RequestClearEditBuffer();
		if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
		if (api::g_active_esc_guard) api::g_active_esc_guard->resume();
		tui::PositionCursorForChat();

		{
			std::unique_lock<std::mutex> lk(race_mu);
			if (!tg_answered.load()) {
				if (picked == -2) {
					g_cancel_retype = 1;
					g_interrupted   = 1;
					if (denial_reason) *denial_reason = "user chose to amend the prompt";
					result = Permission::Deny;
				} else if (picked == 1) {
					api::AlwaysAllowed().insert(tool_name);
					result = Permission::Allow;
				} else if (picked == 0) {
					result = Permission::Allow;
				} else {
					if (denial_reason)
						*denial_reason = "user declined permission for " + tool_name;
					result = Permission::Deny;
				}
				local_answered.store(true);
			}
		}
		race_cv.notify_one();
		tg_thread.join();

		std::cout << tui::Dim(std::string("  -> ") + (result == Permission::Allow ? "yes" : "no")) << "\n";
		return result;
	}

	// Non-interactive (Telegram-only turn, no TTY).
	if (api::g_non_interactive_tools) {
		if (api::g_non_interactive_allow_destructive) return Permission::Allow;
		if (denial_reason)
			*denial_reason = "destructive tool " + tool_name + " is blocked in "
			    "non-interactive mode. Set \"fAllowDestructiveTools\": true in "
			    "config.json to allow it.";
		return Permission::Deny;
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
	const std::vector<std::string> choices = { "Yes", allow_session_label, "No" };

	if (api::g_active_esc_guard) api::g_active_esc_guard->pause();
	repl::BlockStdin();
	tui::PauseFlushTimer();
	tui::SuspendScrollRegion();
	const int picked = tui::SelectOption(choices, question, nullptr, pre_lines);
	tui::RestoreScrollRegion();
	tui::ResumeFlushTimer();
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
	if (picked == 1) {
		api::AlwaysAllowed().insert(tool_name);
		return Permission::Allow;
	}
	if (picked == 0) return Permission::Allow;
	if (denial_reason)
		*denial_reason = "user declined permission for " + tool_name;
	return Permission::Deny;
}
