#ifndef CLI_RUNNER_H
#define CLI_RUNNER_H

#include <string>

#include "cch/StreamSink.h"

// The CLI front-end. NOT a rival to the GUI -- the GUI owns rich interactive
// use, so this is the Unix tool: stateless one-shot, pipeable, scriptable,
// headless. No raw-mode termios, no VTIME, no SelectOption, no render loop.
//
// Every mode below runs the same cch::AgentLoop over the same StreamSink. The
// CLI's sink is trivial: onChunk writes to stdout as text arrives.
//
//   StreamSink StdoutSink() {
//       return {
//           .onChunk  = [](const std::string& c){ std::fputs(c.c_str(), stdout); },
//           .onDone   = [](int){ std::fflush(stdout); },
//           .onError  = [](const std::string& e){ std::fprintf(stderr, "%s\n", e.c_str()); },
//       };
//   }

namespace cli {

enum class Mode {
    OneShot,      // claude-cli "prompt"        -> Turn() once, print, exit
    Pipe,         // cat x | claude-cli "..."   -> stdin folded into context
    File,         // claude-cli -f path "..."   -> file contents as context
    Interactive,  // --interactive: dumb fallback for SSH/tmux (see below)
};

struct Options {
    Mode        mode = Mode::OneShot;
    std::string prompt;       // the user text / instruction
    std::string filePath;     // for Mode::File
    bool        continueConv = false;  // --continue: reload history from disk
};

// Parse argv into Options. Detects piped stdin (not a tty) -> Mode::Pipe.
Options ParseArgs(int argc, char** argv);

// One-shot / pipe / file: build context, run AgentLoop::Turn once with a
// stdout sink, flush, exit. No loop, no retained state unless --continue.
int RunOneShot(const Options& opts);

// The dumb interactive fallback. COOKED mode (no termios raw mode): read a
// line with std::getline, stream the reply to stdout, repeat until EOF/quit.
// Exists only so the CLI is usable when the GUI is unreachable. Deliberately
// has none of the old interactive machinery.
int RunInteractive(const Options& opts);

} // namespace cli

#endif
