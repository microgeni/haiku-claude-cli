#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <nlohmann/json.hpp>

#include "api.h"
#include "config.h"
#include "hooks.h"
#include "mcp.h"
#include "oauth.h"
#include "paths.h"
#include "repl.h"
#include "session.h"
#include "tui.h"

namespace {

void PrintUsage(const char* prog, const std::string& default_model, int default_max_tokens) {
	std::cerr << "Usage: " << prog << " [OPTIONS] [MESSAGE...]\n"
			  << "\n"
			  << "Sends a one-shot message to the Claude API and streams the reply.\n"
			  << "If stdin is not a terminal (piped input), its contents are appended\n"
			  << "to the message so `cat file.txt | " << prog << " \"summarize\"` works.\n"
			  << "\n"
			  << "Commands:\n"
			  << "  login                Authenticate via Claude.ai (OAuth + PKCE).\n"
			  << "  logout               Delete stored credentials.\n"
			  << "\n"
			  << "Options:\n"
			  << "  -i, --interactive    Start a multi-turn REPL session.\n"
			  << "  -m, --model MODEL    Model to use (default: " << default_model << ").\n"
			  << "  -t, --max-tokens N   Max tokens in response (default: " << default_max_tokens << ").\n"
			  << "  -s, --system TEXT    Custom system prompt (appended after the\n"
			  << "                       required Claude Code prefix when OAuth is used).\n"
			  << "  -w, --working-dir PATH\n"
			  << "                       Change to PATH before doing anything else.\n"
			  << "                       GUI launchers can also set CLAUDE_WORKING_DIR\n"
			  << "                       in the environment for the same effect.\n"
			  << "  -u, --usage          After the response, print input/output token\n"
			  << "                       usage to stderr.\n"
			  << "  -r, --resume [NAME]  Start the REPL pre-loaded with the last saved\n"
			  << "                       session (implies -i). Without NAME loads the\n"
			  << "                       default session (history.json). With NAME loads\n"
			  << "                       (or creates) a named session stored as\n"
			  << "                       history-<NAME>.json alongside it.\n"
			  << "  -y, --yes            Auto-approve destructive tools (Bash/Write/Edit)\n"
			  << "                       for this run. Needed for one-shot invocations\n"
			  << "                       without a TTY to answer the y/a/n prompt.\n"
			  << "  -a, --attach PATH    Attach a file path to this session. Repeatable.\n"
			  << "                       Announced to Claude on the next user turn so\n"
			  << "                       tools like Read can pull them in. In interactive\n"
			  << "                       mode you can also drag files from Tracker onto\n"
			  << "                       the Terminal window — the REPL auto-detects paths.\n"
			  << "      --plain          Disable ANSI color output.\n"
			  << "      --color          Force ANSI color output, even when piped.\n"
			  << "      --print-only     Print attached files as a table and exit without\n"
			  << "                       sending any message to the API. Requires -a.\n"
			  << "  -V, --version        Print version and exit.\n"
			  << "  -h, --help           Show this help and exit.\n"
			  << "\n"
			  << "Config file: " << paths::ConfigPath() << "\n"
			  << "  Optional JSON with keys: model, max_tokens, system, show_usage,\n"
			  << "  fAllowDestructiveTools, prices. CLI flags override config values.\n"
			  << "\n"
			  << "Memory files (prepended to the system prompt, user before project):\n"
			  << "  " << paths::UserMemoryPath() << "\n"
			  << "  ./CLAUDE.md (per-project, loaded from the current working directory)\n"
			  << "\n"
			  << "Authentication (in priority order):\n"
			  << "  1. OAuth tokens from 'claude login' (uses Pro/Max quota).\n"
			  << "  2. ANTHROPIC_API_KEY environment variable (billed per token).\n";
}

// Print a one-line token-usage summary to stderr.
void PrintUsageLine(const api::SendResult& result) {
	std::cerr << "[usage] input: " << result.input_tokens
			  << " tokens  output: " << result.output_tokens << " tokens\n";
}

} // namespace

int main(int argc, char* argv[]) {
	api::GlobalInit();
	tui::Init();
	tui::InstallSigwinchHandler();

	// Crash-safe teardown.
	//
	// std::atexit fires on normal return and exit().  It is the primary
	// cleanup path for /quit and Ctrl+D: the REPL loop breaks, the
	// StatusFrameGuard destructor runs repl::Deinit()+TeardownStatusBar(),
	// InteractiveLoop returns, main() returns, and atexit fires as a
	// belt-and-suspenders no-op (g_saved_termios_valid is already false).
	//
	// SIGTERM / SIGINT handlers below call the same teardown directly and
	// then re-raise with SIG_DFL so the process exits with the correct
	// signal status.  This covers:
	//   • SIGTERM  — external kill / system shutdown
	//   • SIGINT   — Ctrl+C while NOT inside InteractiveLoop (e.g. login
	//                flow, one-shot mode, or the tiny window between
	//                InteractiveLoop returning and main() returning).
	//                Inside InteractiveLoop, api::InterruptGuard installs
	//                its own SIGINT handler that just sets g_interrupted=1
	//                so the turn aborts gracefully; that handler is removed
	//                when InteractiveLoop returns, reinstating this one.
	std::atexit([]() {
		repl::Deinit();
		tui::TeardownStatusBar();
	});
	{
		struct sigaction sa {};
		sa.sa_handler = [](int sig) {
			repl::Deinit();
			tui::TeardownStatusBar();
			struct sigaction dfl {};
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			sigaction(sig, &dfl, nullptr);
			raise(sig);
		};
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGTERM, &sa, nullptr);
		sigaction(SIGINT,  &sa, nullptr);
	}

	if (argc >= 2) {
		const std::string cmd = argv[1];
		if (cmd == "login")  return DoLogin();
		if (cmd == "logout") return DoLogout();
	}

	const config::Config cfg = config::Load();
	config::InitLogging(cfg.logging_enabled);
	config::SetHistoryMessageCap(cfg.history_max_messages);
	hooks::Load(cfg.hooks);
	mcp::Init(cfg.mcp_servers);

#ifdef __HAIKU__
	// Ensure the claude:summary BFS index exists on this volume so
	// Query("\"claude:summary\" == \"*\"") runs in O(1) rather than
	// walking every file. mkindex is idempotent.
	{
		pid_t pid = fork();
		if (pid == 0) {
			int devnull = ::open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > 2) close(devnull);
			}
			const char* argv_mk[] = {
				"mkindex", "-t", "string", "claude:summary", nullptr
			};
			execvp("mkindex", const_cast<char* const*>(argv_mk));  // flawfinder: ignore
			_exit(127);
		}
		if (pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0); // reap; ignore exit code
		}
	}
#endif

	std::string              model         = cfg.model;
	int                      max_tokens    = cfg.max_tokens;
	bool                     interactive   = false;
	bool                     show_usage    = cfg.show_usage;
	bool                     print_only    = false;
	bool                     resume        = false;
	std::string              resume_name;   // empty = default history.json
	std::string              custom_system = cfg.system;  // flawfinder: ignore
	std::vector<std::string> parts;
	std::vector<std::string> attachments;

	// Seed the destructive-tool flag from config. -y/--yes below
	// can still flip it on for ad-hoc runs.
	if (cfg.fAllowDestructiveTools) api::g_allow_destructive_tools = true;

	// GUI launchers (e.g. Claude.app on Haiku) may not inherit a meaningful
	// working directory.  Allow them to set CLAUDE_WORKING_DIR in the
	// environment so that the process lands in the right project root before
	// any flag parsing occurs.  --working-dir/-w below overrides this.
	if (const char* env_wd = std::getenv("CLAUDE_WORKING_DIR")) {  // flawfinder: ignore (null-checked; used only as a path)
		if (*env_wd && chdir(env_wd) != 0) {
			std::cerr << "warning: cannot chdir to CLAUDE_WORKING_DIR="
			          << env_wd << ": " << std::strerror(errno) << "\n";
		}
	}

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0], model, max_tokens);
			return 0;
		}
		if (arg == "-V" || arg == "--version") {
			std::cout << "haiku-claude-cli " << config::kVersion << "\n";
			return 0;
		}
		if (arg == "-i" || arg == "--interactive") {
			interactive = true;
			continue;
		}
		if (arg == "-u" || arg == "--usage") {
			show_usage = true;
			continue;
		}
		if (arg == "-r" || arg == "--resume") {
			resume      = true;
			interactive = true;
			// Optional next argument is the session name.
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				resume_name = argv[++i];
			}
			continue;
		}
		if (arg == "-y" || arg == "--yes") {
			api::g_allow_destructive_tools = true;
			continue;
		}
		if (arg == "-a" || arg == "--attach") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a path\n";
				return 1;
			}
			attachments.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--plain") {
			tui::SetColorEnabled(false);
			continue;
		}
		if (arg == "--color") {
			tui::SetColorEnabled(true);
			continue;
		}
		if (arg == "--print-only") {
			print_only = true;
			continue;
		}
		if (arg == "-m" || arg == "--model") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			model = argv[++i];
			continue;
		}
		if (arg == "-t" || arg == "--max-tokens") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			max_tokens = std::atoi(argv[++i]);
			if (max_tokens <= 0) {
				std::cerr << "error: --max-tokens must be a positive integer\n";
				return 1;
			}
			continue;
		}
		if (arg == "-s" || arg == "--system") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a value\n";
				return 1;
			}
			custom_system = argv[++i];
			continue;
		}
		if (arg == "-w" || arg == "--working-dir") {
			if (i + 1 >= argc) {
				std::cerr << "error: " << arg << " requires a path\n";
				return 1;
			}
			const char* wd = argv[++i];
			if (chdir(wd) != 0) {
				std::cerr << "error: cannot chdir to " << wd << ": "
				          << std::strerror(errno) << "\n";
				return 1;
			}
			continue;
		}
		parts.push_back(arg);
	}

	std::string message;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i > 0) message += ' ';
		message += parts[i];
	}

	if (!interactive && !isatty(fileno(stdin))) {
		// Only slurp stdin if data is actually ready. Without this
		// check, fread blocks forever when stdin is an open-but-
		// empty pipe (e.g. `ssh host 'claude hi'` without -t).
		struct pollfd pfd;
		pfd.fd      = STDIN_FILENO;
		pfd.events  = POLLIN;
		pfd.revents = 0;
		const bool has_input =
			poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP));
		if (has_input) {
			std::string stdin_data;
			char        buf[4096];
			size_t      n;
			while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
				stdin_data.append(buf, n);
			}
			while (!stdin_data.empty() && (stdin_data.back() == '\n' || stdin_data.back() == '\r')) {
				stdin_data.pop_back();
			}
			if (!stdin_data.empty()) {
				if (message.empty()) {
					message = std::move(stdin_data);
				} else {
					message += "\n\n";
					message += stdin_data;
				}
			}
		}
	}

	if (!interactive && message.empty()) {
		// With no message and no -i flag, default to interactive
		// mode when stdin is a real terminal.
		if (isatty(fileno(stdin))) {
			interactive = true;
		} else {
			PrintUsage(argv[0], model, max_tokens);
			return 1;
		}
	}

	// Resolve --attach arguments to absolute paths so Claude's Read
	// tool doesn't depend on whatever cwd the REPL inherited.
	std::vector<std::string> resolved_attachments;
	if (!attachments.empty()) {
		resolved_attachments.reserve(attachments.size());
		for (const auto& p : attachments) {
			struct stat st;
			if (stat(p.c_str(), &st) != 0) {
				std::cerr << "error: --attach path not found: " << p << "\n";
				return 1;
			}
			char abs[PATH_MAX];
			const char* use = realpath(p.c_str(), abs) ? abs : p.c_str();  // flawfinder: ignore
			resolved_attachments.emplace_back(use);
		}
		std::cout << tui::Meta(session::FormatAttachedLine(resolved_attachments)) << "\n";
	}

	const config::Auth auth = config::ResolveAuth();
	if (auth.kind == config::AuthKind::None) {
		std::cerr << "error: no authentication configured.\n"
				  << "Run '" << argv[0] << " login' to authenticate with your Claude account,\n"
				  << "or set ANTHROPIC_API_KEY.\n";
		return 1;
	}

	if (interactive) {
		return session::InteractiveLoop(auth, cfg, model, max_tokens, custom_system,
		                                cfg.prices, resume, resume_name, message,
		                                std::move(resolved_attachments));
	}

	api::InterruptGuard interrupt_guard;
	// One-shot: bake the attachment preamble into the single user
	// turn.
	const std::string one_shot_content =
		session::ComposeAttachmentPreamble(resolved_attachments) + message;
	nlohmann::json messages = nlohmann::json::array({{{"role", "user"}, {"content", one_shot_content}}});
	const std::string effective_system = config::ComposeSystem(custom_system);
	const auto result = api::SendWithTools(auth, model, max_tokens, messages, effective_system);
	if (show_usage) {
		PrintUsageLine(result);
	}
	return result.exit_code;
}
