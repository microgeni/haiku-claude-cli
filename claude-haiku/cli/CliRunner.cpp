#include "CliRunner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include "cch/AgentLoop.h"
#include "cch/ApiClient.h"
#include "cch/StreamSink.h"
#include "cch/Tools.h"

namespace cli {

// ---------------------------------------------------------------------------
// StdoutSink — the CLI's StreamSink implementation.
// onChunk writes tokens to stdout as they arrive.
// onError writes to stderr.
// onDone flushes stdout.
// ---------------------------------------------------------------------------

static cch::StreamSink StdoutSink()
{
	cch::StreamSink s;
	s.onChunk  = [](const std::string& c) {
		std::fputs(c.c_str(), stdout);
		std::fflush(stdout);
	};
	s.onDone   = [](int /*code*/) {
		std::fputc('\n', stdout);
		std::fflush(stdout);
	};
	s.onError  = [](const std::string& msg) {
		std::fprintf(stderr, "error: %s\n", msg.c_str());
	};
	return s;
}

// ---------------------------------------------------------------------------
// ParseArgs
// ---------------------------------------------------------------------------

Options ParseArgs(int argc, char** argv)
{
	Options opts;

	// Detect piped stdin: if stdin is not a tty and no message was
	// given on the command line, use Pipe mode.
	bool have_prompt = false;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--interactive" || arg == "-i") {
			opts.mode = Mode::Interactive;
			continue;
		}
		if (arg == "--continue" || arg == "-c") {
			opts.continueConv = true;
			continue;
		}
		if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
			opts.filePath = argv[++i];
			opts.mode     = Mode::File;
			continue;
		}
		if (arg.empty() || arg[0] == '-') {
			// Unknown flag — ignore for now; could print usage.
			continue;
		}
		// Positional argument — the prompt.
		if (!opts.prompt.empty()) opts.prompt += ' ';
		opts.prompt += arg;
		have_prompt = true;
	}

	// Auto-detect piped stdin when no explicit mode was chosen.
	if (opts.mode == Mode::OneShot && !have_prompt
		&& !isatty(fileno(stdin))) {
		opts.mode = Mode::Pipe;
	}

	return opts;
}

// ---------------------------------------------------------------------------
// RunOneShot / RunInteractive
// ---------------------------------------------------------------------------

static cch::AgentLoop MakeAgent(const std::string& systemPrompt)
{
	cch::ApiClient::Config cfg;
	// Model defaults to what's in Config; override via env or future flags.
	const char* model = std::getenv("CLAUDE_MODEL");
	if (model && *model) cfg.model = model;

	static cch::ApiClient api(cfg);
	static cch::ToolRegistry reg;
	static bool registered = false;
	if (!registered) {
		cch::RegisterBuiltins(reg);
		registered = true;
	}
	return cch::AgentLoop(api, reg, systemPrompt);
}

int RunOneShot(const Options& opts)
{
	std::string prompt = opts.prompt;

	if (opts.mode == Mode::Pipe) {
		// Slurp stdin and append to the prompt.
		std::string piped;
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0)
			piped.append(buf, n);
		while (!piped.empty()
			   && (piped.back() == '\n' || piped.back() == '\r'))
			piped.pop_back();
		if (!piped.empty()) {
			if (!prompt.empty()) prompt += "\n\n";
			prompt += piped;
		}
	} else if (opts.mode == Mode::File) {
		// Read the file and prepend as context.
		FILE* f = std::fopen(opts.filePath.c_str(), "r");
		if (!f) {
			std::fprintf(stderr, "error: cannot open %s\n",
			             opts.filePath.c_str());
			return 1;
		}
		std::string content;
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			content.append(buf, n);
		std::fclose(f);
		if (!content.empty()) {
			prompt = "File: " + opts.filePath + "\n\n"
			       + content + "\n\n" + prompt;
		}
	}

	if (prompt.empty()) {
		std::fprintf(stderr, "usage: claude-cli [OPTIONS] PROMPT\n");
		return 1;
	}

	auto agent = MakeAgent({});
	agent.Turn(prompt, StdoutSink());
	return 0;
}

int RunInteractive(const Options& /*opts*/)
{
	// Dumb cooked-mode loop: read a line, stream the reply, repeat.
	// No raw-mode termios, no VTIME, no spinner, no ANSI.
	// This is the SSH/tmux fallback — the GUI owns rich interactive use.
	auto agent = MakeAgent({});
	const cch::StreamSink sink = StdoutSink();

	std::string line;
	std::cout << "claude-cli interactive (Ctrl+D to exit)\n";

	while (true) {
		std::cout << "\nyou> " << std::flush;
		if (!std::getline(std::cin, line)) break;
		if (line.empty()) continue;
		if (line == "/exit" || line == "/quit") break;
		if (line == "/clear") { agent.Reset(); continue; }

		std::cout << "\nclaude> " << std::flush;
		agent.Turn(line, sink);
	}
	return 0;
}

} // namespace cli
