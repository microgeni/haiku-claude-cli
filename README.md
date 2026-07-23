![haiku-claude-cli icon](assets/claude-icon-preview.png)

*- Join the AI revolution, resistance is futile!*

# haiku-claude-cli

A native Claude client for Haiku OS. It runs as a single C++17 binary
with no runtime dependencies beyond libcurl, OpenSSL, libedit, and
nlohmann/json — and it treats Haiku as a first-class platform, not an
afterthought.

## What it does

`claude` is an agentic coding assistant that can read and write files,
run shell commands, search the web, spawn sub-agents, and hold a
multi-turn conversation — all from your terminal. You ask it something,
it plans the work, uses its tools, and streams the answer back token by
token.

Three things make the Haiku edition stand out from generic CLI clients:

### BFS attribute cache — fewer tokens, faster context

Haiku's Be File System stores arbitrary typed metadata alongside every
file. `claude-cli` uses this to maintain a persistent one-line
`claude:summary` attribute on every source file it reads. On the next
session it reads the summary with `ReadAttr` (≈20 tokens) instead of
opening the file (thousands of tokens). Summaries are auto-seeded after
the first full read and can be enriched with `WriteAttr` at any time.

The system prompt is automatically pre-populated with all existing
summaries at startup, giving Claude instant project-wide context without
consuming your token budget. A BFS index is created on first run so
`Query("claude:summary == \"*\"")` resolves in O(1) regardless of
project size.

### Haiku desktop integration — drag, drop, notify

- **Drag and drop**: drop any file from Tracker onto the Terminal window.
  The CLI detects the pasted path, attaches the file's content to the
  next message, and shows a confirmation line — no copy-paste of file
  paths required. **Dropped images** (jpeg / png / gif / webp, up to 5 MB)
  are sent as vision content blocks so you can ask Claude about a
  screenshot or diagram, not just text files.
- **Desktop notifications**: when a long-running response finishes,
  `claude-cli` fires a native `BNotification` via Haiku's
  `notification_server`. The alert shows the first sentence of the
  reply, uses the Claude icon and includes a rotating playful title 
  so repeated notifications stay readable.
 
### Telegram remote control — Claude in your pocket

Add a `telegram` block to `config.json` and the bridge starts
automatically whenever you launch `claude` interactively. From your
phone you can send prompts, approve or deny tool-use requests (via
inline keyboard buttons), mute/unmute the bridge, and receive streamed
replies — all while the same session stays open locally on your Haiku
machine. A background `/remote-control` poller can also be toggled
mid-REPL without restarting the session.

### Ludicrous mode — all permission prompts, gone

Type `/ludicrous` in the REPL and the status bar lights up with a yellow
⚡ **LUDICROUS** badge. Every subsequent tool call — `Bash`, `Write`,
`Edit`, anything that would normally stop and ask — is auto-approved
without a prompt, and a dim `⚡ ludicrous: auto-approved <tool>` line
confirms each one in the transcript. Type `/ludicrous` again to disengage
and restore normal permission prompts. It's session-scoped, so it resets
automatically when you quit.

Use it when you trust the task completely and the approval rhythm is
getting in the way. Don't use it when you don't.

### Desktop app — Claude with a Haiku-native GUI

Alongside the terminal `claude`, the package ships **Claude**, a BeAPI
desktop app. It's the same agent and tools behind a native window: a
scrollable chat transcript with markdown and syntax-highlighted code
blocks (rendered in your live Genio theme), a tool bar, drag-and-drop
image and file attachments, a token/cost bar, and a Settings dialog for
model, working directory, and notification thresholds. **File ▸ New
Session** opens additional independent windows.

The same Telegram remote control is available from **Tools ▸ Remote
Control**, and **Tools ▸ Ludicrous Mode** mirrors the CLI's auto-approve
toggle (both surface a badge in the token bar while active).

### Genio IDE integration — edits land back in your editor

Launch the desktop app from [Genio](https://github.com/Genio-The-Haiku-IDE/Genio)'s
**Tools ▸ Claude** menu and the two apps round-trip: Genio hands Claude
the active project and file, and every file Claude then writes or edits
opens (or refreshes) in the live Genio editor with the cursor jumped to
the edited line. It activates only when launched from Genio — a directly
launched GUI and the CLI are unaffected. Setup is one wrapper script; see
[`contrib/genio/`](contrib/genio/).

### Talk to Claude from your own app — the `'ASKP'` IPC

The same door Genio uses is open to any application. Send the desktop app
an `'ASKP'` `BMessage` (or launch it with `--prompt` / `--working-dir` /
`--send`) to open a scoped chat window, pre-fill the input box, and
optionally auto-submit — from an editor, a script, or a Tracker add-on.
The message `what` and app signature are a stable public contract; the
full protocol is documented in [`docs/IPC.md`](docs/IPC.md).

---

## Features

- **Streaming** token-by-token responses over Server-Sent Events.
- **Two auth modes**
  - `claude login` → OAuth 2.0 + PKCE against claude.ai, requests go
    against your Pro/Max subscription quota.
  - `ANTHROPIC_API_KEY` → per-token Console billing.
- **One-shot, piped, and interactive** modes.
  - `claude "your question"` for a quick one-shot.
  - `cat file.txt | claude "summarize"` appends piped input to the
    message.
  - Bare `claude` in a terminal drops into a REPL.
- **Rich terminal UI**
  - Streaming markdown renderer (bold, italic, inline code, fenced
    code blocks, headings, bullet + numbered lists).
  - Syntax highlighting for C/C++, Python, Shell, Rust, and JSON code
    blocks.
  - Libedit line editing with persistent history, tab completion for
    slash commands, multi-line input via `"""` fences or trailing `\`.
  - Thinking spinner with elapsed-seconds counter.
  - Per-turn status line: `[turn 3  1.8s  in 42/167  out 128/512]`.
- **Ten built-in tools** — `Read`, `Glob`, `Grep`, `Bash`, `Write`,
  `Edit`, `WebFetch`, `WebSearch` (with Brave API key), `Task`
  (one-shot sub-agent), `TodoWrite`/`TodoRead`.
- **Permission prompts** on destructive tools (`Bash`, `Write`,
  `Edit`) with `(y)es once / (a)lways session / (n)o` choices, plus
  a block-style diff preview for writes and edits. Headless runs
  (piped stdin, scripts, CI) can auto-approve via `-y/--yes` or
  `"allow_destructive_tools": true` in `config.json`.
- **Project + user memory** via `CLAUDE.md` files loaded automatically
  each turn.
- **Custom slash commands** from `.claude/commands/*.md` with
  `{{args}}` substitution.
- **Shell-command lifecycle hooks** — `SessionStart`, `UserPromptSubmit`,
  `PreToolUse`, `PostToolUse`, `Stop`. Register in `config.json` and
  block actions by exiting non-zero.
- **MCP (Model Context Protocol)** stdio-transport client — spawn
  MCP servers as subprocesses, discover their tools, advertise them
  to Claude as `mcp__<server>__<tool>`.
- **Interrupt-safe** — Ctrl+C during a streaming response aborts the
  in-flight request cleanly instead of killing the process.
- **Opt-in logs** at `~/config/settings/claude-cli/logs/` for session
  starts, turn tokens, tool calls, and errors.
- **HPKG packaging** for Haiku, built via CI on real Haiku hardware.

## Install

### On Haiku

Download the latest HPKG from the Gitea releases page:

```
pkgman install /path/to/claude_cli-0.10.0-*-x86_64.hpkg
```

Or build from source:

```
pkgman install devel:libcurl devel:libssl nlohmann_json pkgconfig libedit_devel
git clone ssh://git@gitea.microgeni.synology.me:2222/daniel/haiku-claude-cli.git
cd haiku-claude-cli
make
make install
```

Default install prefix is `/boot/system/non-packaged`; override with
`make install PREFIX=/some/where`.

### On macOS (development)

A `flake.nix` is provided for prototyping before building on real
Haiku hardware via the Gitea Actions CI:

```
nix develop       # enter shell with make, pkg-config, curl, nlohmann_json, openssl, libedit
make
./build/claude --help
```

The macOS builds are a development convenience — the real target is
Haiku x86_64 with gcc13.

## Quick start

Authenticate once against your Claude.ai Pro/Max account:

```
claude login
```

Your browser opens, you approve, paste the code back. Tokens land at
`~/config/settings/claude-cli/credentials.json` (0600).

Then:

```
claude "Write a one-line description of Haiku OS."
```

Or drop into the REPL:

```
claude
```

## Interactive mode

```
Claude CLI interactive mode (model: claude-sonnet-4-6).
Type /help for commands, 'exit' or Ctrl+D to leave.

> Plan a refactor for my send_conversation function.
claude> ...
[turn 1  1.8s  in 42  out 128]

> /compact
```

- Arrow up/down walks the persisted REPL history.
- `"""` on a line starts a multi-line block (end with another `"""`).
- A trailing `\` continues the message onto the next line.
- Ctrl+C cancels a streaming response; Ctrl+D leaves the REPL.
- Tab completes slash-command names.

### Slash commands

| Command          | What it does                                       |
|------------------|----------------------------------------------------|
| `/help` `/?`     | List available commands, including custom ones.    |
| `/clear`         | Reset the running conversation.                    |
| `/model NAME`    | Swap the active model mid-session.                 |
| `/compact`       | Ask Claude to summarize history and replace it.    |
| `/cost`          | Session token total + rough price estimate.        |
| `/todos`         | Print the current in-session todo list.            |
| `/memory [user]` | Open project (or user) `CLAUDE.md` in `$EDITOR`.   |
| `/exit` `/quit`  | Leave the REPL.                                    |

Custom commands live under `.claude/commands/<name>.md` (project) or
`~/config/settings/claude-cli/commands/<name>.md` (user). The file
body is a prompt template; `{{args}}` is replaced with everything
following the command in the REPL. Project commands override user
commands on name collision.

## Tools

Claude has access to ten built-in tools:

| Tool          | Purpose                                                      |
|---------------|--------------------------------------------------------------|
| `Read`        | Read a file (or a line range).                               |
| `Glob`        | POSIX glob pattern match, sorted newest-first.               |
| `Grep`        | `grep -rn` wrapper for content search.                       |
| `Bash`        | Shell command via `sh -c`, prompts for permission.           |
| `Write`       | Create/overwrite a file, prompts with content preview.       |
| `Edit`        | Exact-string replacement, prompts with block diff.           |
| `WebFetch`    | HTTP GET a URL and return the body (truncated).              |
| `WebSearch`   | Brave Search wrapper (needs `BRAVE_SEARCH_API_KEY`).         |
| `Task`        | One-shot sub-agent with no tools, isolated history.          |
| `TodoWrite`   | Replace the in-session todo list (plan, track work).         |
| `TodoRead`    | Return the current todo list.                                |

Read-only tools (`Read`, `Glob`, `Grep`, `WebFetch`, `WebSearch`,
`TodoWrite`, `TodoRead`, `Task`) auto-approve. Destructive tools
(`Bash`, `Write`, `Edit`) prompt on first use with `(y)es` / `(a)lways
session` / `(n)o` choices; `Write` and `Edit` show a preview of the
actual change before the prompt.

## Memory (`CLAUDE.md`)

`claude` reads two optional markdown files and prepends them to the
system prompt on every turn:

1. `~/config/settings/claude-cli/CLAUDE.md` — user scope.
2. `./CLAUDE.md` — project scope (current working directory).

Edit either one with `/memory` (project, the default) or
`/memory user`. Memory is re-read on the next turn — no restart
required.

## Configuration

Optional JSON at `~/config/settings/claude-cli/config.json`. Every
key is optional; CLI flags override the file.

```json
{
  "model":      "claude-sonnet-4-6",
  "max_tokens": 8192,
  "history_max_messages": 200,
  "thinking_budget": 0,
  "system":     "Extra system instructions appended after the preamble.",
  "show_usage": false,

  "allow_destructive_tools": false,

  "logging": { "enabled": false },

  "prices": {
    "claude-sonnet-4-6": { "input":  3.0, "output": 15.0 },
    "claude-opus-4-6":   { "input": 15.0, "output": 75.0 },
    "claude-haiku-4-5":  { "input":  0.8, "output":  4.0 }
  },

  "hooks": {
    "UserPromptSubmit": [
      { "command": "cat >> $HOME/tmp/claude.log" }
    ],
    "PreToolUse": [
      { "matcher": "Bash", "command": "my-bash-linter.sh" }
    ]
  },

  "mcp_servers": {
    "filesystem": {
      "command": "npx",
      "args":    ["-y", "@modelcontextprotocol/server-filesystem", "/home/user/workspace"]
    }
  }
}
```

### Hooks

Each hook runs as `sh -c <command>`. The event payload is written
to the hook's stdin as JSON (with `event` and, for tool-related
events, `tool_name` injected). A non-zero exit blocks the next
action — `UserPromptSubmit` drops the turn, `PreToolUse` synthesizes
a denied tool result. Stderr is mirrored to the user's own stderr so
hooks can talk back.

### MCP servers

Each server is spawned as a subprocess at startup. The CLI runs the
`initialize` handshake, queries `tools/list`, and exposes each tool
to Claude as `mcp__<server>__<tool>`. Every MCP tool prompts for
permission before its first use.

## Telegram setup

`claude-cli` has two Telegram modes. Both use the same config block;
the difference is in how you launch.

### 1. Create a bot

1. Open a chat with [@BotFather](https://t.me/BotFather) on Telegram.
2. Send `/newbot`, follow the prompts, and copy the **bot token**
   it gives you (looks like `123456789:AAF...`).
3. Start a chat with your new bot, send it any message, then find
   your **user ID** by opening
   `https://api.telegram.org/bot<TOKEN>/getUpdates` in a browser.
   The `from.id` field in the response is your numeric user ID.

### 2. Add the config block

Add a `telegram` key to `~/config/settings/claude-cli/config.json`:

```json
{
  "telegram": {
    "bot_token":             "123456789:AAF...",
    "allowed_user_ids":      [987654321],
    "allow_destructive_tools": false
  }
}
```

| Key                       | Required | Description                                                   |
|---------------------------|----------|---------------------------------------------------------------|
| `bot_token`               | yes      | Token from BotFather.                                         |
| `allowed_user_ids`        | yes      | Whitelist of numeric Telegram user IDs that may send prompts. |
| `allow_destructive_tools` | no       | Set `true` to let remote callers run `Bash`, `Write`, `Edit`. |

The `allowed_user_ids` list is the security boundary — messages from
any other Telegram user are silently ignored.

### 3. Full bridge mode — auto-start

When the `telegram` block is present and valid in `config.json`, running
`claude` interactively automatically starts the full Telegram bridge —
no separate subcommand needed.

What you get on the phone:

- **Streamed edits** — the bot posts a `⏳ thinking…` placeholder and
  edits it roughly every second as tokens arrive, finishing with the
  full response.
- **Inline permission buttons** — when a destructive tool (`Bash`,
  `Write`, `Edit`) needs approval you get three buttons:
  *Yes, allow once* / *Always allow this session* / *No, deny*.
- **Numbered option buttons** — when Claude lists numbered choices,
  each becomes a tappable inline button.
- **Local mirror** — everything typed at the laptop prompt is also
  sent to your Telegram chat, so the conversation is unified.

Bot commands available from Telegram:

| Command              | Effect                                                            |
|----------------------|-------------------------------------------------------------------|
| `/mute`              | Stop sending replies (incoming prompts still run locally).        |
| `/unmute`            | Resume sending replies.                                           |
| `/new`               | Clear this user's rolling conversation history.                   |
| `/clear`             | Same as `/new`.                                                   |
| `/model [name]`      | Show current model, or swap to a different one.                   |
| `/compact`           | Summarize and replace the running conversation history.           |
| `/usage`             | Session token count and subscription window utilisation.          |
| `/cost`              | Same as `/usage`.                                                 |
| `/todos`             | Print the current in-session todo list.                           |
| `/stats`             | Lifetime token usage and tool stats.                              |
| `/ludicrous`         | Toggle ludicrous mode (auto-approve all tool permissions).        |
| `/help` or `/start`  | Show the full command list.                                       |

`/exit`, `/quit`, and `/remote-control` are not available from Telegram.
Custom commands (`.claude/commands/*.md`) work too — send `/commandname args`
just as you would in the local REPL.

The status bar on the Haiku machine shows **Remote Control active**
in green, with **· muted** appended in yellow when muted.

### 4. Remote-control mode — `/remote-control`

Toggle full bidirectional Telegram control from inside any REPL session:

```
> /remote-control
[remote control: telegram poller started]
```

The poller runs in a background thread and offers the same experience
as the full bridge: streaming edits with a thinking placeholder,
inline permission buttons for destructive tools, numbered-option
buttons, and local-turn mirroring to the primary Telegram chat.
Each Telegram user gets their own independent rolling history.
Type `/remote-control` again (or `/remote-control off`) to stop.

If the `telegram` config block is missing or incomplete, `/remote-control`
will tell you exactly what to add instead of silently failing:

```
[remote control: config.telegram.bot_token is not set]
  Add a 'telegram' block to config.json with
  bot_token and allowed_user_ids.
  See the Telegram setup section in README.md.
```

**Full bridge vs `/remote-control`** — both give the full
bidirectional experience. The full bridge (auto-started when telegram
is configured) is a dedicated session with no local REPL; use it when
you want to hand the machine to Claude and walk away. `/remote-control`
layers Telegram on top of your existing REPL session; use it when you
want to stay at the keyboard and also take questions from your phone.

## Environment

| Variable                | Purpose                                          |
|-------------------------|--------------------------------------------------|
| `ANTHROPIC_API_KEY`     | Fall-back auth when no OAuth tokens are stored.  |
| `BRAVE_SEARCH_API_KEY`  | Enables the `WebSearch` tool.                    |
| `EDITOR`                | Launched by the `/memory` command.               |
| `NO_COLOR` `CLICOLOR`   | Honored by ANSI color detection.                 |
| `XDG_CONFIG_HOME`       | Overrides config directory on non-Haiku builds.  |

## Development

Branching: `dev` for active work, `main` for release tags. See
`docs/GIT_WORKFLOW.md`.

CI: macOS runner checks out the repo and SSHes to a Haiku machine
to run the build + functional tests for every push to
`dev`/`main`, and to produce HPKG release assets for every `v*` tag.
The workflow lives in `.gitea/workflows/build-test.yml`; the shell
scripts under `ci_scripts/`.

Roadmap and changelog live at `docs/ROADMAP.md` and `CHANGELOG.md`.

## License

MIT. See `LICENSE`.
