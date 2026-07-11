# claude-haiku

A native Haiku Claude client. One portable C++ core, two front-ends (GUI + CLI),
one uniform streaming path that both API responses and command output flow through.

## Layout

```
core/                 libclaudecore.a — pure C++17, ZERO BeAPI
  include/cch/
    StreamSink.h        the keystone: onChunk/onDone/onError contract
    Process.h           fork/exec + argv, holds PID, killable (the foundation)
    CommandTarget.h     Local / SshPosix / SshRouterOs — all sit on Process
    Tools.h             tool dispatch; FileSearch injection point (BFS on Haiku)
    ApiClient.h         HTTP/SSE/JSON to /v1/messages, surfaces tool_use
    AgentLoop.h         orchestrator: the tool-use cycle, owns conversation state
  src/                  .cpp implementations

gui/                  claude-gui — BeAPI shell (links core + libbe)
  ChatWindow.h          BWindow owning BTextView; MSG_* worker<->main boundary
                        BFS conversation storage + native search adapters live here

cli/                  claude-cli — Unix tool (links core only)
                        one-shot / pipe-oriented; the GUI owns rich interactive
                        use, so the CLI sheds the persistent-REPL machinery
                        (raw-mode termios, VTIME, SelectOption, render loop).
                        A minimal --interactive fallback remains for SSH/tmux.
```

## The two rules that keep it correct

1. **The core never includes a BeAPI header.** Anything BeAPI (BTextView, BFS
   BQuery, BMessage) lives in `gui/` and is injected into the core via interfaces
   (StreamSink, FileSearch). The CLI injects POSIX implementations of the same.

2. **Worker threads never touch a BView.** The core runs on a worker; its
   StreamSink packages text into BMessages and SendMessage()s them to the window.
   MessageReceived (main thread, window lock) does the actual Insert. This is the
   responsiveness fix — event-driven, no termios/VTIME polling.

## Division of labor (the two front-ends stop overlapping)

The GUI owns rich interactive use; the CLI becomes the Unix tool. They no
longer compete, so each can shed what the other does better:

- **GUI** = the interactive client. Rich text (BTextView), scrollback,
  concurrent panes, BFS history, drag-and-drop context. Where you have a
  *conversation*.
- **CLI** = the Unix tool. Stateless one-shot, pipeable, scriptable, headless.
  Where you fire a *query* from a script or an SSH session.

      claude-cli "summarize this"            # run once, print, exit
      cat log | claude-cli "what's failing"  # stdin as context
      claude-cli -f config.rsc "explain"     # file in, answer out

Dropping interactive-as-primary removes the entire persistent-REPL apparatus
from the CLI -- raw-mode termios, VTIME tuning, SelectOption, the ANSI render
loop. The CLI's StreamSink collapses to "write chunk to stdout": onChunk =
fputs(stdout). A one-shot run is AgentLoop::Turn() once with a stdout sink,
then exit -- no Reset-and-loop, optionally no retained history at all.

This is also why the TUI-library question (FTXUI vs libvaxis vs hand-rolled)
disappears: a one-shot CLI needs no TUI library, and the GUI uses BeAPI.

### --interactive: the dumb fallback

A minimal back-and-forth mode is kept for when the GUI is not reachable (SSH,
tmux, headless): read a line, stream a reply, repeat -- COOKED mode, no fancy
rendering, no VTIME/SelectOption. It is a fallback, not the CLI's reason to
exist, so it does not pull the interactive complexity back in.

## Why API tokens and command output share one path

Both are "text arriving incrementally on a worker." Both drive a `StreamSink`.
The GUI's sink turns callbacks into BMessages; one MessageReceived renders both.
A streamed model response and a streaming `find` are the same shape.

## Build order (de-risk first)

TLS is the riskiest Haiku dependency. Before any GUI work:

1. Build just `ApiClient` + `StreamSink` and stream a real response from the
   Anthropic endpoint to stdout. Settle curl-vs-mbedTLS here.
2. Add `Process` + `LocalTarget`, wire a bash tool, get the agent loop running
   headless in the CLI.
3. Add the GUI shell over the working core.
4. Add SSH targets (ControlMaster), then the RouterOS target.
5. Add BFS storage + native search in the GUI.

## Build

```
make core      # the library first
make cli       # headless, fastest to iterate
make gui       # needs libbe
make           # all three
```
