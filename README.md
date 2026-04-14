# haiku-claude-cli

A native Claude client for Haiku OS — everything you'd expect from an
agentic CLI, in one C++17 binary with no runtime except libcurl,
OpenSSL, libedit, and nlohmann/json.

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
  "max_tokens": 1024,
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
(Taurus) to run the build + functional tests for every push to
`dev`/`main`, and to produce HPKG release assets for every `v*` tag.
The workflow lives in `.gitea/workflows/build-test.yml`; the shell
scripts under `ci_scripts/`.

Roadmap and changelog live at `docs/ROADMAP.md` and `CHANGELOG.md`.

## License

MIT. See `LICENSE`.
