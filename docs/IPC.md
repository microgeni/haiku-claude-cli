# Talking to the Claude GUI from another application

The Claude desktop app (`Claude`, signature
`application/x-vnd.Microgeni-claude-gui`) accepts a simple, documented IPC
message so that **any** Haiku application can hand a prompt to Claude — open
a new chat window scoped to a project, pre-fill the input box, and
optionally auto-submit it. This is the same mechanism Genio's
**Tools ▸ Claude** integration uses, but it is not Genio-specific: it is a
stable public entry point for editors, scripts, Tracker add-ons, or any
BApplication.

There are two equivalent ways in:

1. **A `BMessage` sent to the running app** (`'ASKP'`) — best when Claude
   may already be open and you want to reuse the live instance.
2. **Command-line arguments** at launch (`--prompt`, `--working-dir`, …) —
   best from a shell script or a launcher that does not link against the
   Application Kit.

Both routes converge on the same handler and behave identically.


## 1. The `'ASKP'` BMessage protocol

Send a `BMessage` whose `what` is `'ASKP'` to the app's signature. The app
opens a new `ChatWindow` scoped to the requested working directory, seeds
the input field with the assembled prompt, brings the window to the front,
and — if you ask it to — submits the turn immediately.

| Constant        | Value    | Defined in                       |
|-----------------|----------|----------------------------------|
| target signature| `application/x-vnd.Microgeni-claude-gui` | `Makefile` (`GUI_APP_SIG`), `app_main_gui.cpp` (`kAppSig`) |
| message `what`  | `'ASKP'` | `app_main_gui.cpp` (`kMsgAskPrompt`) |

### Fields

All fields are optional, but a message with no `prompt` opens an empty
window. Send the fields relevant to your use case and omit the rest.

| Field         | Type            | Meaning |
|---------------|-----------------|---------|
| `prompt`      | `B_STRING_TYPE` | The question or instruction. Seeds the input box. |
| `context`     | `B_STRING_TYPE` | Extra text **appended below** the prompt (two newlines between). Use it for a code selection, a compiler error, a stack trace, etc. |
| `working_dir` | `B_STRING_TYPE` | Directory Claude's tools run in. Also flags the session as "launched externally" so file edits round-trip to the caller (see below). |
| `file`        | `B_STRING_TYPE` | The focused file, for provenance. Marks the session external. |
| `line`        | `B_INT32_TYPE`  | 1-based caret line associated with `file`. |
| `send`        | `B_BOOL_TYPE`   | `true` = auto-submit the seeded prompt; `false`/absent = wait for the user to press Enter or **Send**. |

> **Prompt assembly.** When both are present the window is seeded with
> `prompt` + `"\n\n"` + `context`. Keep the actual instruction in `prompt`
> and put raw material (code, logs) in `context`.

### Minimal example

```cpp
#include <Application.h>
#include <Messenger.h>
#include <Message.h>

static const char*    kClaudeGuiSig = "application/x-vnd.Microgeni-claude-gui";
static const uint32_t kMsgAskPrompt = 'ASKP';

void AskClaude(const char* prompt, const char* workingDir, bool autoSend)
{
    // A BApplication must exist in your process for BMessenger to work.
    BMessenger target(kClaudeGuiSig);
    if (!target.IsValid()) {
        // Claude is not running. Either launch it (see BRoster below) or
        // fall back to the command-line route.
        return;
    }

    BMessage msg(kMsgAskPrompt);
    msg.AddString("prompt", prompt);
    if (workingDir && workingDir[0])
        msg.AddString("working_dir", workingDir);
    msg.AddBool("send", autoSend);

    target.SendMessage(&msg);   // fire-and-forget; no reply is sent.
}
```

`SendMessage` is asynchronous and returns `B_OK` once the message is
queued; Claude never sends a reply. If you need to be sure Claude is
running first, launch it with `BRoster`:

```cpp
#include <Roster.h>

BRoster roster;
if (!roster.IsRunning(kClaudeGuiSig))
    roster.Launch(kClaudeGuiSig);   // then send 'ASKP', or pass argv (below).
```

You can also pass the whole request through `BRoster::Launch()` as a
launch `BMessage`, which starts Claude *and* delivers the `'ASKP'` payload
in one call — handy when Claude is not yet open.


## 2. The command-line entry point

Launching the `Claude` binary with these flags produces exactly the same
result as an `'ASKP'` message. This is the friendliest route from shell
scripts and Genio's extension wrapper.

| Flag                    | Equivalent field | Notes |
|-------------------------|------------------|-------|
| `--prompt <text>`       | `prompt`         | Seeds the input box. Also accepts `--prompt=<text>`. |
| `--working-dir <path>`  | `working_dir`    | Or `-w <path>`, or `--working-dir=<path>`. |
| `--file <path>`         | `file`           | Provenance; marks the session external. |
| `--line <n>`            | `line`           | 1-based caret line. |
| `--send`                | `send=true`      | Auto-submit the prompt. |
| `--project-dir <path>`  | `working_dir`    | Genio's name for the project root; also sets the external/round-trip flag. |

Example:

```sh
Claude --working-dir /Data/Code/myproj \
       --prompt "Explain what build.sh does" \
       --send
```

Because Haiku routes a second launch of an already-running app back into
the live instance (via `ArgvReceived`), running the command again while
Claude is open simply opens another scoped window — you do not get a second
process.


## 3. What "launched externally" changes

Passing `working_dir`, `file`, or `--project-dir` marks the session as
having been launched by another tool. This is the **provenance signal** and
it enables the file round-trip: after Claude's `Write` or `Edit` tool
succeeds, `editor::NotifyFileChanged()` posts a `B_REFS_RECEIVED` message
(with `be:line`) to the editor so the changed file opens at the edited
line.

Today that round-trip targets Genio (`application/x-vnd.Genio`); see
[`contrib/genio/README.md`](../contrib/genio/README.md) for the editor
side. A directly-launched GUI or the CLI sends none of these fields, so the
integration stays completely inert.


## 4. Testing the path

A standalone sender and a driver script exercise the protocol without
needing Genio:

```sh
make ipc-test          # builds build/ipc_test_sender
sh tests/ipc_test.sh   # launches Claude if needed, fires several messages
```

`tests/ipc_test_sender.cpp` is a ~130-line reference implementation of a
BMessage sender you can copy from. Run it directly for ad-hoc checks:

```sh
build/ipc_test_sender --prompt "What is 2 + 2?" \
                      --working-dir "$(pwd)" \
                      --context "Sent over IPC from the shell."
```

The Claude window should come to the front with the prompt pre-filled,
ready to send.


## 5. Compatibility notes

- The message `what` (`'ASKP'`) and the app signature are stable; treat
  them as the public contract.
- All `'ASKP'` fields are optional and forward-compatible — unknown fields
  are ignored, so newer Claude builds may add fields without breaking older
  senders.
- The canonical definitions live in `src/app_main_gui.cpp`
  (`kAppSig`, `kMsgAskPrompt`, and the `MessageReceived`/`ArgvReceived`
  handlers). If you vendor the constants into your own code, keep them in
  sync with that file and the `GUI_APP_SIG` value in the `Makefile`.
