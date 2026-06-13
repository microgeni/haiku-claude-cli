# Roadmap

## Vision

`haiku-claude-cli` aims to be a first-class Claude client on Haiku OS
with a feature set approaching Anthropic's Claude Code CLI, while
staying native to Haiku conventions — installable via `pkgman`,
respecting Haiku config paths under `~/config/settings/`, and
integrating with Haiku system services where it actually helps.

The tool should feel like it belongs on Haiku, not a generic Linux
binary dropped onto the system. Every feature is judged by whether it
makes the CLI more useful to a Haiku developer and whether it plays
nicely with the rest of the OS.

## Current state (v0.1.1 + unreleased)

- Streaming SSE responses.
- OAuth 2.0 + PKCE (Pro/Max subscription quota) and `ANTHROPIC_API_KEY`
  fallback.
- One-shot, stdin-piped, and interactive REPL modes.
- Custom system prompts (`-s/--system`), model and max-token overrides
  (`-m/-t`).
- Token usage reporting (`-u/--usage`).
- Conversation persistence with `-r/--resume`.
- HPKG packaging and Gitea CI for build, test, and release upload.
- Keep-a-Changelog auto-generated release notes.

This is a solid "text in, text out" CLI. What's missing is everything
around **agentic behavior** (tool use, file reading/editing, shell
execution) and **session ergonomics** (slash commands, cancellation,
config files, cost tracking).

## Reference: what Claude Code does

Claude Code is an agentic CLI that lets Claude read and write files,
execute shell commands, search codebases, maintain todo lists, spawn
sub-agents, load project memory, run hooks, and integrate with MCP
servers. It ships with a rich terminal UI, slash commands, keyboard
shortcuts, and IDE extensions.

The roadmap below prioritizes the features that matter most for a
Haiku developer using Claude to work on their own code — not every
Claude Code feature.

## Milestones

### v0.2 — Quality of life ✓

Make the existing text-in/text-out loop noticeably nicer to use
without adding any new capabilities.

- [x] Config file at `~/config/settings/claude-cli/config.json` with
      defaults for model, max-tokens, system prompt, show-usage,
      and per-model prices. CLI flags override the file.
- [x] `/model MODEL` slash command in the REPL to swap models mid-session.
- [x] `/cost` slash command showing cumulative tokens for the current
      session plus a rough price estimate from the config's price
      table (with built-in fallbacks that substring-match common
      model families).
- [x] `/clear` slash command to reset the running conversation.
- [x] `/compact` slash command that asks Claude to summarize the
      running conversation and replaces history with the summary.
- [x] Ctrl+C during a streaming response cancels the in-flight request
      cleanly instead of killing the process.

### v0.3 — Terminal UI polish ✓

The current REPL is `getline` + raw stream output. Claude Code's TUI
gets a lot of its feel from small things — live markdown rendering,
a thinking spinner, proper line editing. Match the parts that work on
Haiku's Terminal app (ANSI + UTF-8, 256 colors, no sixel/kitty).

- [x] **Markdown rendering** in assistant output: bold, italic, inline
      code, code blocks with language label, bullet lists, headings.
      Falls back to raw text when stdout isn't a TTY so piped output
      stays scriptable.
- [x] **Syntax-highlighted code blocks** — minimal tokenizer per
      language (C/C++, Python, shell, JSON to start), ANSI 256-color
      theme, auto-detected from the code block's language label.
- [x] **Line editing via libedit/readline** — arrow-key navigation,
      in-memory history, emacs-style bindings, persisted REPL history
      at `~/config/settings/claude-cli/repl_history`.
- [x] **Multi-line input** — trailing backslash (or `"""` fence) opens
      a continuation prompt for pasting multi-paragraph messages.
- [x] **Transient status line** shown during the thinking window
      (between request submit and first streamed token): model,
      running message count, max-tokens cap, elapsed time, and an
      `esc:cancel` hint. Truncates to the current `terminal_width()`
      so it stays on one line on narrow terminals. Shipped instead
      of a fixed-bottom frame — see box-drawing note below.
- [x] **Thinking spinner** between request submit and first token,
      erased automatically when the stream starts.
- [x] **Distinct turn styling** — color and bold for `you>` / `claude>`
      prompts, dim for meta notes like `[resumed N messages]`. Honors
      `NO_COLOR` and non-TTY stdout.
- [x] **Terminal resize handling** — SIGWINCH handler marks the
      cached `terminal_width()` dirty so the next spinner tick
      re-reads `TIOCGWINSZ` and truncates cleanly.
- [x] **ESC to cancel in-flight work** — stdin is put into cbreak
      mode for the duration of each HTTP stream; a background
      thread watches for a bare `0x1B` byte and sets `g_interrupted`,
      reusing the same curl-abort path as Ctrl+C. CSI escape
      sequences (arrow keys etc.) are ignored so accidental
      keystrokes during streaming don't kill the turn. Termios is
      restored on scope exit so libedit gets stdin back in cooked
      mode for the next prompt.
- [x] **Palette portability** — the TUI sticks to standard 16-color
      ANSI codes (no 256-color / truecolor) so the user's terminal
      theme (dark or light) controls the actual rendered colors.
      True dark/light auto-detection via terminal queries or
      `COLORFGBG` is deferred until we hit a concrete need.
- [x] **Unicode box-drawing frame** around the REPL — explicitly
      deferred in v0.3. Fixed-bottom frames need DECSTBM scroll regions and
      careful SIGWINCH tracking, which is a bigger refactor than
      v0.3 warrants. The transient status line above covers the
      actually-useful information window (the thinking phase) at a
      fraction of the complexity. Shipped in v1.1.2.

The TUI layer should be isolated behind a small abstraction so a
`--plain` flag or a non-TTY stdout disables it entirely and falls back
to the current raw streaming behavior. Piping into scripts must keep
working without surprises.

### v0.4 — Read-only tool use ✓

Let Claude see (and, via Bash, act on) the local project. This is the
biggest architectural jump in the roadmap: the server-side tool-use
loop, where Claude requests a tool, the CLI runs it, and the result
is fed back as a `tool_result` turn.

- [x] `Bash` tool — sh -c command wrapper, combined stdout+stderr
      capture, 32 KiB output cap. Prompts for permission on the first
      call per session (no substring write-heuristic — the prompt is
      the safety net).
- [x] `Read` tool — file contents with optional `start_line`/`end_line`
      range.
- [x] `Glob` tool — POSIX glob pattern match, sorted newest-first by
      mtime.
- [x] `Grep` tool — fork/exec POSIX `grep -rnH -e PATTERN --`, clean
      no-match vs error handling.
- [x] Per-tool permission prompts with (y)es-once / (a)lways-this-
      session / (n)o choices, stored in a session-scoped allowlist.
      Read/Glob/Grep auto-approve; only Bash prompts.
- [x] Wire the Messages API `tools` parameter and handle the
      tool_use / tool_result turn-taking across streaming
      (`send_with_tools` loop + `StreamState.content_blocks`).

### v0.5 — Write tools ✓

Claude can now modify files — with safety rails.

- [x] `Write` tool — create or overwrite a file, auto-creates parent
      directories. Prompts for permission with a preview showing new
      vs overwrite, byte/line counts, and the first 10 lines of the
      content.
- [x] `Edit` tool — exact-string replacement in an existing file, with
      a `replace_all` flag. Prompts for permission with a block-style
      diff (`-` old_string / `+` new_string) and the line number of
      the first match.
- [x] Preview every write/edit before the permission prompt — shown
      dim between the `[tool: ...]` notice and the yes/always/no
      choice. Edit's preview is a block diff; a full unified LCS diff
      across arbitrary rewrites is deferred.
- [x] Writes outside the current working directory are flagged with a
      `[WARNING: outside cwd]` marker in the preview so the user can
      make an informed decision. A hard auto-deny was rejected as too
      restrictive for personal use on Haiku; the warning + permission
      prompt is the safety net.

### v0.6 — Project memory ✓

Absorb per-project context without pasting it every turn.

- [x] Load `CLAUDE.md` from the current working directory as a
      project preamble (appended after the user-level memory and
      before the `--system` flag content).
- [x] Load `~/config/settings/claude-cli/CLAUDE.md` as a user-level
      preamble.
- [x] `/memory [user]` slash command to open either the project or
      user `CLAUDE.md` in `$EDITOR` (falls back to `nano`). Memory is
      re-read on the next turn, so edits take effect immediately with
      no REPL restart.

### v0.7 — Slash commands (custom + built-in polish) ✓

- [x] Namespace for built-in commands (`/help`, `/model`, `/cost`,
      `/clear`, `/compact`, `/memory`, `/exit`, `/quit`).
- [x] User-defined commands loaded from `.claude/commands/*.md` in
      the project and `~/config/settings/claude-cli/commands/`
      globally. Project definitions override user ones on collision.
- [x] Argument support in custom commands via `{{args}}` substitution.
      Text following the command name becomes `{{args}}` in the body,
      which is sent as the user message.
- [x] Tab completion in the REPL for slash command names, built-in
      and custom, wired via libedit's `rl_attempted_completion_function`.

### v0.8 — Hooks ✓

Let the user react to lifecycle events with plain shell scripts.

- [x] Hook types: `SessionStart`, `UserPromptSubmit`, `PreToolUse`,
      `PostToolUse`, `Stop`.
- [x] Hooks declared in the existing `config.json` under a `"hooks"`
      key, keyed by event name. Each entry is
      `{ "matcher": "...", "command": "..." }`; matcher is used as a
      tool-name filter for PreToolUse/PostToolUse (missing matcher =
      match any). Project-level overrides deferred.
- [x] Each hook runs as `sh -c <command>` with the event payload
      written to its stdin as JSON (enriched with `event` and, for
      tool events, `tool_name`). Stderr is forwarded verbatim so
      hooks can talk back to the user. A non-zero exit from any
      matching hook Blocks — UserPromptSubmit drops the turn,
      PreToolUse synthesizes a denied tool_result. Stdout is not
      captured in this slice (no context injection yet).

### v0.9 — MCP (Model Context Protocol) ✓

Interoperate with MCP servers so every MCP tool is available to this
CLI.

- [x] stdio transport — spawn each configured server as a subprocess,
      speak newline-delimited JSON-RPC 2.0 over stdin/stdout, run
      the `initialize` handshake, and query `tools/list`.
- [x] Server config lives under a `mcp_servers` key in the existing
      `config.json` — each entry has `command`, optional `args`, and
      optional `env`. A separate `mcp.json` file is deferred; the
      existing config.json path is enough for now.
- [x] Advertise MCP-provided tools to Claude via the Messages API
      `tools` array. Tool names are namespaced as
      `mcp__<server>__<tool>` so they can't collide with built-ins.
      `tools::run` dispatches any `mcp__…` name to the owning server
      via `tools/call`, concatenates text content blocks in the
      result, and propagates `isError`. All MCP tools
      `require_permission`.
- [ ] HTTP/SSE transport for remote MCP servers (deferred).
- [ ] `resources/`, `prompts/` — tools-only in this slice.

### v0.10 — Advanced built-ins ✓

Fill in the remaining Claude Code tools that make sense on a personal
dev machine.

- [x] `WebFetch` tool — libcurl GET with redirect following, 30 s
      timeout, 32 KiB truncation. Returns HTTP status + content-type
      header followed by the body. Raw HTML is passed through for
      Claude to parse directly (no server-side HTML→Markdown).
- [x] `WebSearch` tool — Brave Search API wrapper. Only registered
      in `tools::definitions()` when `BRAVE_SEARCH_API_KEY` is set,
      so Claude doesn't see an unusable tool. Returns up to 10
      title/url/description blocks.
- [x] `Task` tool — one-shot sub-agent via `send_conversation` with
      `include_tools=false`. Fresh messages array, same auth/model/
      memory as parent, streams to the terminal, final text becomes
      the tool_result. Recursion-safe because the sub-agent has no
      tools (including no Task).
- [x] Todo list tools (`TodoWrite` / `TodoRead`) backed by an
      in-process vector. Statuses: pending / in_progress / completed.
      `/todos` slash command prints the current list as a checklist.

### v1.1 — Remote Control via Telegram bot ✓

Instead of reverse-engineering Claude Code's undocumented
`/remote-control` bridge protocol, use Telegram's fully-
documented Bot API as the transport. Same outcome (drive the
local CLI from your phone while tools execute on your
machine), same outbound-only HTTPS security story, but
implementable from public docs in an afternoon.

- [x] New `claude telegram` subcommand (alongside
      `login`/`logout`) that runs a headless bridge loop.
      No REPL overlay — meant to run in a tmux window or
      background shell on the dev machine.
- [x] `src/telegram.{h,cpp}` — tiny Telegram Bot API client
      over libcurl: `getUpdates` with 30 s long-poll,
      `sendMessage` with Markdown V2 formatting, offset
      persistence so polls don't redeliver.
- [x] Config key in `config.json`:
      ```json
      "telegram": {
        "bot_token":             "123456:ABC...",
        "allowed_user_ids":      [12345678],
        "allow_destructive_tools": false
      }
      ```
      Unauthorized user IDs are ignored silently so a stray
      random chat can't fingerprint the bot. At least one
      allowed user is required.
- [x] Per-user in-memory `messages[]` so each authorized
      Telegram user has their own rolling conversation. A
      `/new` message from the user resets their history.
- [x] Bridge loop: incoming message → run `send_with_tools`
      with that user's history → stream the response to the
      local terminal as usual → send the final assistant
      text back via `sendMessage`.
- [x] Read-only tool pre-approval: `Read`, `Glob`, `Grep`,
      `WebFetch`, `WebSearch`, `Task`, `TodoWrite`,
      `TodoRead` auto-run. `Bash`, `Write`, `Edit`, and any
      MCP tool are blocked unless
      `allow_destructive_tools: true` is set — there's no
      way to prompt y/a/n through Telegram cleanly.
- [x] `/help` from Telegram replies with the per-user
      command list; `/new` resets history; any other
      message goes to Claude.
- [x] Hooks, memory (`CLAUDE.md`), and MCP still apply in
      Telegram mode — the bridge just replaces the REPL
      input source, the rest of the stack is unchanged.

Shipped beyond the original slice:
- [x] **Inline-keyboard buttons for numbered choices** —
      when Claude replies with a numbered list, each option
      becomes a tap-to-answer button under the message.
- [x] **Local libedit prompt alongside the Telegram poller**
      so the bridge operator can type from the laptop too,
      serialized against the remote poller via a shared
      process mutex. Local input mirrors to the primary
      Telegram chat and shares rolling history with that
      user — hop between laptop and phone without losing
      context.
- [x] **Typing indicator** — `sendChatAction(..., "typing")`
      once per second for the duration of each send, so the
      remote user sees the "typing..." animation.
- [x] **Streaming edits** — the placeholder message is
      edited in-place with `editMessageText` as tokens
      arrive, so the Telegram chat sees the reply build up
      token-by-token instead of a wall of text.
- [x] **Slash commands from the local prompt** — `/usage`,
      `/help`, `/clear`, `/model`, `/compact`, `/todos`,
      `/memory`, and custom `.claude/commands/*.md`
      commands all work inside `claude telegram` the same
      way they do in `claude -i`.

Deferred for a later slice:
- Inline-keyboard permission prompts for destructive tools
  (would let `Bash`/`Write` run interactively without the
  config flag).
- Voice notes / image attachments from Telegram.
- Multi-chat / group-chat support.

### v1.1.1 — Reliability fixes ✓

- [x] One-shot runs no longer silently deny destructive tools.
      `prompt_permission` now detects non-TTY stdin and emits a
      loud error pointing at `-y`/`--yes` or
      `allow_destructive_tools` config key.
- [x] `-y`/`--yes` CLI flag auto-approves destructive tools for
      one-shot and piped-stdin runs.
- [x] Top-level `allow_destructive_tools` config key, persistent
      equivalent of `-y`.
- [x] `max_tokens` truncation is no longer silent — loud stderr
      warning with output/max numbers and re-run guidance.
- [x] Orphan `tool_use` blocks on truncation stripped from REPL
      history to avoid API 400 on continuation.
- [x] Default `max_tokens` raised from 1024 to 8192.
- [x] `/mute` and `/unmute` bridge commands gating every outbound
      Telegram Bot API call.

### v1.1.2 — TUI + /remote-control ✓

Major TUI overhaul and in-REPL remote control toggle.

- [x] **Fixed-bottom status frame** via DECSTBM scroll region.
      4-row frame: rule above input, fixed input row, rule below,
      status content (model, turn, tokens, max). SIGWINCH rebuild,
      atexit crash-safe teardown.
- [x] **Rich spinner** matching Claude Code style — rotating star
      glyphs (✶ ✷ ✸ ✹ ✺ ✻ ✼ ✽), randomized gerund verb with
      ~1 Hz dim/bright pulse, live `↑ N tokens` count from
      `message_start`, `Xm Ys` elapsed format, `esc:cancel` hint.
- [x] **Markdown table alignment** — pipe tables are buffered,
      column widths computed, emitted with box-drawing borders
      (┌ ┬ ┐ ├ ┼ ┤ └ ┴ ┘), bolded headers, and `:---`/`---:`/
      `:---:` alignment markers honored.
- [x] **ESC cancels in-flight work** — stdin goes cbreak during
      curl, a background thread watches for bare `0x1B`, sets
      `g_interrupted` via the same path as Ctrl+C.
- [x] **Cursor hidden during streaming** via DECTCEM so it
      doesn't bounce through the rendered output.
- [x] **Tool permission prompts on the fixed input row** — the
      question text goes into the scroll history, the y/a/n
      answer reads at the `>` prompt, choice is replayed as a
      dim `-> yes/always/no` line.
- [x] **`/remote-control` toggle** — spawns a lean background
      Telegram poller from inside `claude -i`. Toggle on/off,
      fast stop via curl progress callback (~1 s instead of
      ~20 s). Green `Remote Control active` label in the status
      row. Local turns mirror to the primary Telegram chat.
      Each Telegram user has independent rolling history.
- [x] **Session resilience** — TCP keepalive
      (60 s idle / 30 s interval), per-turn OAuth token refresh,
      persistent curl handle with connection reuse, exponential
      backoff retry on 429/5xx/curl-transient (3 attempts,
      1 s / 2 s / 4 s).
- [x] **Tab completion** for hyphenated slash commands —
      `rl_completer_word_break_characters` set to whitespace only.
- [x] **SIGWINCH handling** — `tui::terminal_width()` /
      `terminal_rows()` cache refreshed on resize, scroll region
      rebuilt between prompts.

### v1.0 — Stable release ✓

Polish, docs, and Haiku-native integration.

- [x] Man page installed to
      `/boot/system/documentation/man/man1/claude.1`; shipped inside
      the HPKG.
- [x] `claude -V / --version` prints the build's semver string. The
      same constant feeds the Messages API `User-Agent`.
- [x] Structured logs at `~/config/settings/claude-cli/logs/` with
      date-stamped filenames. Opt-in via a `logging.enabled` key in
      `config.json`. Records session starts, turn tokens, tool
      invocations, and HTTP error details.
- [x] Graceful non-2xx error handling: Anthropic's error envelope is
      parsed for `error.message`, well-known codes (401/403/429/5xx)
      get plain-language explanations, and the opaque Claude Code
      "Error" 429 gate is distinguished from a real rate limit.
      Per-turn OAuth token refresh added in v1.1.2 so long sessions
      don't fail mid-conversation.
- [x] End-to-end README walkthrough rewritten covering install, auth,
      REPL, slash commands, tools, memory, hooks, MCP, config keys,
      and environment variables.
- [ ] HaikuDepot package submission — requires a HaikuPorts recipe
      and external review. Deferred; the HPKG is downloadable from
      the Gitea release page for now.

### v1.2 — BFS attribute tools (filesystem-as-database) ✓

Haiku's BFS carries typed extended attributes on every file and
indexes them for instant queries. This milestone exposes that
capability as first-class tools so Claude can persist its
understanding of a codebase *on the files themselves* and query
it back in O(1) instead of re-reading everything.

**Token economics** — measured on Taurus (real Haiku x86_64
hardware) against haiku-claude-cli's own 17-file, 7 013-line
codebase via `tests/bfs_tools_test.sh`:

| Scenario | Measured | Savings |
|----------|----------|---------|
| Read ALL source files (status quo) | **66 654 tokens** | — |
| Load `claude:summary` attributes only | **154 tokens** | **99.8 %** |
| Summaries + 3 targeted file reads | ~3 400 tokens | ~94.9 % |
| Summaries + 1 large file (main.cpp) | ~31 000 tokens | ~53.5 % |

On a 1 000-file project the savings scale linearly: a full
read would cost ~500 K tokens; summaries alone stay under 3 K.
The difference is the gap between "fits in one turn" and
"needs aggressive /compact to avoid context overflow".

**Performance** — measured on Taurus:

| Operation | Latency |
|-----------|---------|
| Write 17 attrs (`addattr`) | 285 ms |
| Read 17 attrs (`catattr`) | 293 ms |
| Glob (`ls *.cpp *.h`) | 20 ms |
| BFS query (`query '...'`) | 17 ms |
| Index creation (`mkindex`) | 16 ms |

Query vs. glob gap is small at 17 files (17 ms vs. 20 ms) but
the BFS query stays O(1) while glob scales linearly with file
count — at 1 000+ files the gap becomes 10–50×.

**Headline number: 66 500 tokens saved per session.** That's
the difference between consuming most of the context window
just to understand the project layout vs. staying under 200
tokens and having the full window available for actual work.

**Attribute persistence**: BFS attributes are local-only — they
do NOT travel with `git push/clone`. That's intentional: they're
a local cache of Claude's understanding, not shared project
state. `git clean`, `git checkout`, `git reset --hard` all
leave them intact. If the volume is wiped or the repo is cloned
fresh, Claude just regenerates the summaries on the next session.

#### Tools

- [x] **`Query`** — execute a BFS query and return matching
      file paths.
      ```json
      {
        "name": "Query",
        "input_schema": {
          "type": "object",
          "properties": {
            "expression": {
              "type": "string",
              "description": "BFS query expression, e.g. 'BEOS:TYPE == \"text/x-source-code\" && last_modified > %1hour%'"
            },
            "volume": {
              "type": "string",
              "description": "Volume to search. Defaults to the volume containing cwd."
            }
          },
          "required": ["expression"]
        }
      }
      ```
      Internally: `fork/exec` the `query` command (ships with
      Haiku), capture stdout, return one path per line. Truncate
      at 32 KiB like other tools. Auto-approved (read-only).

- [x] **`ReadAttr`** — read one or more attributes from a file.
      ```json
      {
        "name": "ReadAttr",
        "input_schema": {
          "type": "object",
          "properties": {
            "path": { "type": "string" },
            "names": {
              "type": "array",
              "items": { "type": "string" },
              "description": "Attribute names to read. Empty = list all attributes and their types."
            }
          },
          "required": ["path"]
        }
      }
      ```
      Internally: `catattr` for reading, `listattr` for listing.
      Auto-approved (read-only).

- [x] **`WriteAttr`** — write a typed attribute to a file.
      ```json
      {
        "name": "WriteAttr",
        "input_schema": {
          "type": "object",
          "properties": {
            "path": { "type": "string" },
            "name": { "type": "string", "description": "Attribute name, e.g. 'claude:summary'" },
            "type": { "type": "string", "enum": ["string", "int32", "int64", "float", "double", "bool"], "description": "Attribute type. Defaults to string." },
            "value": { "type": "string", "description": "Value to write (converted to the declared type)." }
          },
          "required": ["path", "name", "value"]
        }
      }
      ```
      Internally: `addattr -t <type> <name> <value> <path>`.
      **Requires permission** (writes to the filesystem, same
      tier as `Write`/`Edit`). The preview shows the path,
      attribute name, type, and value.

- [x] **`IndexAttr`** — create a BFS index for fast querying.
      ```json
      {
        "name": "IndexAttr",
        "input_schema": {
          "type": "object",
          "properties": {
            "name": { "type": "string", "description": "Attribute name to index, e.g. 'claude:component'" },
            "type": { "type": "string", "enum": ["string", "int32", "int64", "float", "double"], "description": "Index type." }
          },
          "required": ["name", "type"]
        }
      }
      ```
      Internally: `mkindex -t <type> <name>`. **Requires
      permission** (creates a volume-level index). Only offered
      on Haiku builds (`#ifdef __HAIKU__`).

#### Workflow: auto-summary on first session

When Claude reads a source file for the first time in a
project, it can write a one-line `claude:summary` attribute:

```
addattr -t string claude:summary \
    "OAuth PKCE flow + token refresh for Pro/Max subscriptions" \
    src/oauth.cpp
```

Next session, instead of reading 17 files (64 K tokens), Claude
queries:

```
catattr -d claude:summary src/*.cpp src/*.h
```

Gets 17 one-liners (~425 tokens), understands the project
shape, and reads only the files it actually needs for the
current task. **95–99 % token reduction per session**.

#### Guard rails

- `claude:*` namespace is reserved for CLI-written attributes.
  Claude should not overwrite system attributes (`BEOS:TYPE`,
  `MAIL:*`, `Audio:*`) without explicit user confirmation.
- `WriteAttr` is permission-gated like `Write`/`Edit`.
- `IndexAttr` warns that it affects the entire volume, not
  just the project directory.
- On non-Haiku builds (macOS via nix), all four tools are
  omitted from `tools::definitions()` so Claude doesn't see
  them.

### v1.3.4 — BFS default-on (system-prompt guidance + summary preload) ✓

v1.2 shipped the BFS attribute tools but Claude wasn't using
them: general-purpose Read/Glob defaults dominate unless
something nudges. Now the CLI ships BFS guidance baked into
the system prompt and preloads a summary map at session start
so Claude always knows the feature exists and what's already
been cached.

- [x] **BFS system-prompt block** — `compose_system` appends
      a Haiku-only paragraph explaining `ReadAttr` /
      `WriteAttr` / `Query`, when to prefer them, and the
      `claude:*` namespace convention. No CLAUDE.md edit
      required on the user's side — the CLI injects it every
      turn.
- [x] **Summary preload** — first call to `compose_system`
      walks cwd (excluding .git/build/node_modules and dot
      directories), catattrs `claude:summary` off each file,
      and caches the non-empty results in a process-scoped
      snapshot. Each subsequent turn reuses the snapshot —
      one filesystem walk per session, not per turn.
- [x] **Graceful empty-state** — with no seeded summaries,
      the block still appears and prompts Claude to write
      them as it reads source files, so later sessions
      inherit the cache.
- [x] **Non-Haiku no-op** — `#ifdef __HAIKU__` guards keep
      the feature out of macOS dev builds (where the BFS
      tools aren't even registered).

Deferred:
- [x] **Auto-create `claude:summary` index** — on startup,
      `main()` fork+exec's `mkindex -t string claude:summary`
      (Haiku-only, `#ifdef __HAIKU__`). Fully silent: the
      child redirects stdio to `/dev/null` and we ignore the
      exit code, so "already exists" errors are harmless and
      a missing/read-only volume fails quietly. Query now
      runs O(1) on fresh installs without any manual step.
- Background refresh of the summary snapshot mid-session
  when WriteAttr is called. Currently stale until next
  session start.

### v1.3 — Tracker drag-and-drop ✓

Haiku's Terminal inserts dropped file paths straight into the
active input line. Rather than ship a separate Tracker add-on
(the 2016-era way to integrate), the REPL itself auto-detects
when a line is a path drop and stashes it as an attachment for
the next outgoing turn. Zero BeAPI, zero extra binary.

- [x] **`-a / --attach PATH`** — repeatable CLI flag. Resolves
      to an absolute path via `realpath`, fails loudly if the
      path doesn't exist. Works in one-shot, stdin-piped, and
      interactive modes. Shell users can also type the flag
      manually; the Tracker drop path below is the zero-friction
      alternative in the REPL.
- [x] **In-REPL drop detection** — after libedit returns a line,
      the REPL tokenizes it with shell quoting (single/double
      quotes preserved, `\\` escaping). If every token is an
      absolute path that `stat()` resolves, the line is treated
      as an attachment event: paths accumulate in a
      `pending_paths` vector, a dim `[attached: …]` line
      acknowledges, and the API turn is skipped. The
      leading-slash requirement means a bare filename like
      `main.cpp` still goes to Claude as a literal prompt.
- [x] **Attachment preamble** — on the next real user turn,
      accumulated paths are prepended to the outgoing API
      content as a `Files attached to this session:\n- …\n\n`
      block. The user's typed text is unchanged in the replay,
      logs, and hook payloads — only the API sees the preamble.
      Pending list drains exactly once per turn, then refills
      from subsequent drops.
- [x] **Works both directions** — `claude -a src/foo.cpp
      "summarize"` bakes the preamble into a one-shot. `claude
      -i` → drag `foo.cpp` from Tracker → type your question
      produces the same content on the wire.

**UX example (interactive mode)**

```
claude -i
> <drag foo.cpp from Tracker onto Terminal>
> '/boot/home/src/foo.cpp'
[attached: /boot/home/src/foo.cpp]
> what does this file do?
claude> [Reads /boot/home/src/foo.cpp, then explains it.]
```

**Deferred**:
- A proper Tracker right-click add-on (`.so` with
  `process_refs`) that launches Terminal with `claude -i -a
  <refs>` pre-wired. The drop-on-running-terminal flow covers
  the common case; the add-on would only help when no Terminal
  is open, which is rare on a Haiku dev machine.
- Directory drops — currently `stat()` accepts them so they're
  announced as attachments, but Claude has no Directory-specific
  tool yet (Glob/Read/Grep work on patterns within). Revisit if
  it becomes friction.
- `application/x-vnd.Microgeni-claude-cli` signature + MIME
  type registration. Not needed for the drop-to-terminal UX;
  only useful if we later ship a GUI-launchable entry point.

### v1.3.3 — Haiku app icon (HAL in the Terminal) ✓

On Haiku, every binary can carry a `BEOS:ICON` attribute that
Tracker, HaikuDepot, and pkgman pick up automatically. Shipped
the canonical artwork + build plumbing in this slice; the
Icon-O-Matic round-trip that produces the binary HVIF file is
a local step on a Haiku install, documented below.

**Canonical design — "HAL in the Terminal"** (`assets/claude-icon.svg`):

The official Haiku Terminal icon (tombstone CRT monitor + tilted
keyboard + coiled cable, MIT-licensed via
[darealshinji/haiku-icons](https://github.com/darealshinji/haiku-icons))
with the screen content replaced by a black panel showing the
glowing red HAL 9000 eye, plus a small green Haiku leaf tucked
into the top-right corner of the screen as the brand tie. The
chassis and keyboard come from Haiku's own artwork so the icon
sits natively alongside Tracker / StyledEdit / Terminal in a
Dock row; HAL carries the "this is an AI" story at every size.

Why this won among thirteen drafts:
- **Haiku-native by construction** — the silhouette, lighting,
  keyboard keys, and cable loop are drawn by Haiku's own
  designers.
- **HAL's red eye is the strongest "AI" signifier at 16×16** —
  even when everything else blurs, a red dot on a monitor
  reads as "AI inside."
- **Character** — a tiny HAL watching from the Dock has
  personality the cleaner-but-generic variants don't.

**Trademark caveat** — HAL 9000 is Kubrick/Clarke's. Close
enough to a generic "AI icon" in tech culture to be safe for
personal use on the Taurus dev machine; swap for a
trademark-clean design (stylized `C`, terminal cursor, scroll
+ brush, etc.) before any HaikuDepot submission or wider
public distribution.

**Build plumbing**:

- [x] **`assets/claude-icon.svg`** — canonical source,
      attributed to Haiku contributors + darealshinji in the
      header comment.
- [x] **`assets/ref/App_Terminal.svg`** — unmodified Haiku
      Terminal reference kept next to the canonical for
      attribution clarity and future iteration.
- [x] **Makefile icon-stamp steps** — both the `install`
      target and the HPKG staging block run
      `addattr -t 'VICN' -f $(ICON_HVIF) BEOS:ICON <binary>`
      when `assets/claude-icon.hvif` exists and `addattr` is
      present. Silently skipped on macOS dev (no `addattr`)
      and on first-time Haiku builds before the HVIF has been
      produced. `ICON_HVIF` is overridable so a future design
      swap can point at an alternate file without rewriting
      the Makefile.

**SVG → HVIF conversion (automated via `icon2icon`)**:

Haiku ships a package called `hvif_tools` (from
github.com/threedeyes/hvif-tools) that provides the
`icon2icon` CLI — SVG/HVIF/IOM/PNG converter with full
gradient support. No Icon-O-Matic GUI step required.

```sh
# one-time on Taurus:
pkgman install -y hvif_tools

# regenerate after any SVG edit:
icon2icon claude-icon.svg claude-icon.hvif -f hvif
```

The committed `assets/claude-icon.hvif` is the output of
that conversion. 3.2 KB, 38 styles, 44 paths, 43 shapes —
a bit heavier than a hand-optimized Haiku icon but well
within HPKG ergonomics.

Optional sanity-check render:

```sh
icon2icon claude-icon.hvif claude-icon-preview.png \
    --width 256 --height 256
```

`assets/claude-icon-preview.png` is the committed render at
256×256 so reviewers can see the binary output without
installing Haiku tooling.

If you ever want to hand-tune (rebuild concentric circles
as a single HVIF radial gradient, set Min LOD on
small-detail layers, etc.), Icon-O-Matic on a Haiku install
still works as a fallback — import the SVG, tweak, export.

**Deferred**:
- A trademark-clean design for HaikuDepot submission.
- Binary-embedded resource route via Haiku's `rc` compiler
  (single-binary distribution, no post-install attribute
  step). Doable but more moving parts; the `addattr` route
  is plenty for now.

### v1.3.2 — Desktop notifications on slow turns ✓

When Claude takes long enough that the user probably walked
away from the laptop, fire a Haiku desktop notification so they
know to come back. Threshold-gated and runtime-toggleable so
the alert isn't spammy for fast replies.

- [x] **`notify` config sub-object** — `{ "enabled": true,
      "min_duration_seconds": 60 }` under the top-level
      `config.json`. Defaults favor "on": anyone who doesn't
      want notifications can flip `enabled` to false.
- [x] **Post-turn trigger** — at the end of each streamed
      assistant turn in `interactive_loop`, if `elapsed >=
      threshold` and the toggle is on, shell out to Haiku's
      `notify` CLI with title `Claude response ready (Ns)` and
      body = first sentence of the reply, whitespace-collapsed
      and capped at 120 chars.
- [x] **Fork+execvp, no shell** — avoids any quoting risk;
      `notify-server` is BMessage-based so the CLI itself
      returns in milliseconds (we `waitpid` it cleanly).
- [x] **Haiku-only via `#ifdef __HAIKU__`** — macOS builds
      under nix compile the helper as a no-op so dev iteration
      stays silent.
- [x] **`/notify` slash command** — no-arg prints current
      state, `on`/`off` toggles, numeric sets threshold in
      seconds. Session-scoped; doesn't rewrite config.json.
      Makes testing trivial: type `/notify 2` to verify the
      alert fires, `/notify 60` to restore production gating.

**Deferred — "cool animation" stretch**:
- `notify --type progress --messageID claude-turn-N` supports a
  progress-bar notification that can be re-sent with the same
  `messageID` to update in place. A follow-up could show a live
  "Claude is thinking… (↑ 812 · 24s)" progress notification
  during streaming, replaced by the "response ready" alert on
  complete. Needs a spinner-adjacent thread that fires `notify`
  every ~1 s — cheap, but wasted effort if Claude typically
  answers under the threshold.
- Custom icon via `--icon /path/to/icon.hvif` once the HVIF app
  icon lands from the v1.0 Haiku-native extras list.
- Different notification types (information / important /
  error) based on whether the turn produced tool errors or
  other anomalies.

### v1.3.1 — `/open` URL launcher ✓

Claude answers often cite docs, issues, or references by URL.
Rather than copy-paste into a browser, let the REPL track URLs
as they stream in and launch them with one slash command.

- [x] **URL harvest on every turn** — after each assistant
      reply streams in, the REPL scans `result.assistant_text`
      for `http://` / `https://` tokens, strips trailing
      sentence punctuation (`.,;:!?`), and dedups in insertion
      order. Tolerates markdown `[label](url)` and
      angle-bracketed `<url>` forms. Not a full RFC 3986 parser
      — the goal is "grab something openable," not validation.
- [x] **`/open`** (no args) lists the session's URLs, numbered.
- [x] **`/open N`** launches the Nth URL via Haiku's `open`
      command (same binary macOS ships, so the dev workflow
      under nix just works). Fire-and-forget: the child is
      backgrounded with `>/dev/null 2>&1 &` so the REPL status
      frame isn't stomped.
- [x] **`/open <url>`** launches an arbitrary URL; lets the user
      paste a link without having to drop to another window.
- [x] **Shell-safe** — URL passed through a single-quote
      escaper (`'` → `'\''`) before being handed to `system()`,
      so query strings with quotes can't break the command.
- [x] Registered in `/help`, `all_slash` for tab completion,
      and both the interactive REPL and the Telegram bridge's
      local prompt.

### v1.4 — True async input (type while Claude thinks) ✓

The `LocalWorker` thread introduced in v1.6.3 moves
`SendWithTools` off the main thread, but the main thread still
blocks on `fDisplayCv` waiting for the turn to finish before
returning to `ReadMessage()`. This milestone lifts that
constraint so the user can type the next prompt — or cancel the
current one — while the response is still streaming.

#### Cancel-and-retype (Ctrl+X) ✓

A dedicated keypress (Ctrl+X, distinct from ESC/Ctrl+C which
discard the turn) cancels the in-flight request and restores
the user's last submitted text to the libedit input buffer for
editing. Use case: you press Enter, immediately notice a typo
or missing context, and want to amend without retyping from
scratch.

- [x] **`repl::RestoreInput(line)`** — pushes bytes in reverse
      via `rl_stuff_char` so libedit's LIFO queue drains
      left-to-right into the edit buffer at the start of the
      next `readline()` call.
- [x] **Ctrl+X binding in cbreak monitor** — the ESC-watch
      thread watches for `0x18`; sets `g_cancel_retype = 1` and
      `g_interrupted = 1`. Worker records `job.userText` into
      `TurnResult.cancelledInput` and clears `g_cancel_retype`.
- [x] **`TurnResult.cancelledInput`** — set by the worker on
      Ctrl+X. `InteractiveLoop` calls `repl::RestoreInput` when
      non-empty before looping back to `ReadMessage`.
- [x] **Status-bar hint** — `ctrl+x: amend` dim label shown in
      the status bar while a turn is running; cleared on
      completion.
- [x] **History: don't record cancelled turns** — `repl::RemoveLastRecord()`
      removes the last libedit history entry and flushes to disk when
      Ctrl+X is detected; `repl::RestoreInput` then seeds the edit buffer.
      The amended re-submission becomes the canonical history entry.

#### True non-blocking prompt (type-ahead) ✓

Return to `ReadMessage()` immediately after enqueuing, so the
user can compose the next message while the current one streams.
The incoming keystrokes are buffered by libedit; the turn is
submitted only after the worker signals completion (or the user
explicitly queues it).

- [x] **`fWorkerOwnsDisplay` as a tri-state** — added an explicit
      `DisplayState { Idle, Streaming, Done }` on `LocalWorker`
      alongside the existing `fWorkerOwnsDisplay` terminal-ownership
      bool. The worker sets `Streaming` on dispatch and `Done` on
      completion; `drain_turn()` returns it to `Idle`. The main loop
      reads `Streaming` vs `Done` to decide whether to stage input or
      drain.
- [x] **Double-Enter to queue** — a line submitted while
      `fDisplayState == Streaming` is stashed in
      `LocalWorker::fQueuedInput` instead of blocking on
      `fDisplayCv.wait`. The loop returns to the prompt; when the
      top-of-loop drain fires after the turn completes, the queued
      line is moved into the next iteration's `queuedLine` and
      dispatched through the normal path-drop / slash / hooks /
      `dispatch_turn` flow without re-entering `ReadMessage()`.
      Queue depth is 1 — a second queued line replaces the first with
      a `[queued (replaced previous)]` notice.
- [x] **Visual separation** — a dim `[queued: …]` annotation is
      printed to the scroll history and a `[queued] next prompt` hint
      is shown on the status row while the input is staged, so the
      user knows it is staged, not yet submitted.

**Deferred within this milestone**:
- Streaming the worker's output interleaved with user keystrokes
  (requires a full split-screen TUI; deferred to a later milestone).
- Multi-turn queue depth > 1 (one staged turn is the common case;
  a queue of N opens questions about rollback semantics).

---

### v1.4.1 — BFS summary snapshot background refresh ✓

The `claude:summary` snapshot loaded at session start grows
stale as Claude writes new `WriteAttr` calls during the session.
A background refresh keeps the in-process cache consistent so
later turns in the same session benefit from summaries written
earlier without restarting.

- [x] **`config::RefreshSummarySnapshot(changed_paths)`** —
      O(changed) per-path update via `catattr`; removes old line
      by prefix match, inserts new value if valid UTF-8.
- [x] **`TurnResult.writtenSummaryPaths`** — `SendWithTools`
      accumulates paths from successful `WriteAttr claude:summary`
      calls in a thread-local vector; `api::DrainWrittenSummaryPaths()`
      returns and clears it. Worker stores it in `TurnResult`;
      `InteractiveLoop` calls `RefreshSummarySnapshot` when non-empty.
- [x] **Full re-scan on `/compact`** — `commands.cpp` calls
      `config::ReloadBfsSummaries()` after `SaveHistory`.
- [x] **Session-start cap** — `kSnapshotLineCap = 500`: `RefreshSummarySnapshot`
      counts lines in `g_bfs_snapshot` and returns early when at or above
      the cap. `BfsSystemBlock` appends a note telling Claude that
      mid-session WriteAttr updates are not reflected and to use ReadAttr
      for the current value of a specific file.

**Deferred**:
- inotify / `BPathMonitor`-based real-time watch (overkill for
  the common single-session workflow; the per-turn refresh covers
  99 % of cases).
- Purging stale entries for deleted files (rare; harmless —
  a miss on a deleted file just returns an empty result).

---

### v1.9 — Native GUI (`claude-gui`) ✓

A first-class BeAPI desktop client built from the same core logic
modules as the CLI (`api`, `tools`, `config`, `hooks`, `mcp`,
`oauth`, `models`, `notify`), with the terminal front-end swapped
for a native Haiku GUI (`chat_window`, `gui_sink`, `app_main_gui`).
Built via `make gui` → `build/claude-gui`; Haiku-only (links
`libbe`, `libtracker`, `libnetwork`).

The window streams responses into a styled `BTextView` with the
shared markdown renderer and syntax highlighter, drives tools
through a `GuiSink` that marshals every worker-thread event to the
window thread via `BMessenger`, and persists conversations as BFS
session files (browsable in Tracker).

Architecture:
- [x] **Worker thread + `GuiSink`** — `api::SendWithTools` runs on a
      background `std::thread`; the sink posts `MSG_CHUNK` /
      `MSG_TOOL_*` / `MSG_ASK_*` / `MSG_STATUS` / `MSG_ERR` BMessages
      to the window (non-blocking). The worker is **joined** (never
      detached) in `MSG_WORKER_DONE` / `QuitRequested` / dtor before
      `delete fSink`, so it can never touch a freed sink.
- [x] **Tool-context history** — the worker mutates a member
      `fWorkerMessages` in place (not a discarded copy); on a clean
      turn the main thread adopts it via `std::move`, so
      `tool_use` / `tool_result` blocks survive into the next turn.
- [x] **Permission dialog** — `BAlert` with Deny / Allow Once /
      Always allow `<tool>`. "Always" inserts into
      `api::AlwaysAllowed()` (the shared session allowlist) so the
      tool is never re-prompted. Blanket auto-approve lives in
      Tools ▸ Ludicrous Mode.
- [x] **`AskChoice` modal** — numbered tool choices render as a
      native `BAlert` (≤ 3) or a self-contained `ChoiceModal` window
      (> 3) with a sem handshake; default button + Esc-cancel a11y.
      (No active caller in the tool loop yet — forward-looking.)
- [x] **Image attachments** — dropped JPEG / PNG / GIF / WebP
      (≤ 5 MB) are base64-encoded and sent as `image` content blocks
      in an array-form user message. Non-image files keep the
      inline-text-fence behavior.

Quality-of-life (the GUI counterpart to v0.2/v0.3):
- [x] **Export Transcript** (File ▸ Export…, Cmd-E) — `BFilePanel`
      → Markdown serializer handling text / array / image /
      tool_use / tool_result content.
- [x] **Find in conversation** (Cmd-F) — case-insensitive search bar
      with live-search, prev/next wraparound, `n / total` counter.
- [x] **Font zoom** (Cmd +/-/0) — rescales output runs
      multiplicatively, preserving the renderer's heading/body
      proportions; re-applied after each turn, replay, and clear.
- [x] **Session sidebar** (View ▸ Sessions, Cmd-B) — left-docked
      `BListView` of saved BFS sessions (newest first) with
      New / Open / Rename / Delete. Multi-select (Shift/Cmd-click)
      supports bulk delete; right-click a row for a Rename / Open /
      Delete context menu. Rename updates only the `claude:title`
      attribute. `session::Delete` / `session::Rename` added to the
      store.
- [x] **Welcome splash**, model picker, token bar, slide-in
      Settings panel, slash-command autocomplete popup, desktop
      notifications, and the shared HVIF app icon.

Also shipped:
- [x] **Per-session settings** — each saved session remembers the
      model, system prompt, working directory, and max-tokens it was
      created with (stored in the session-file envelope) and restores
      them on load, so continuing an old conversation keeps its
      context.
- [x] **Persisted GUI preferences** — window frame, chat zoom level,
      and last-used model are saved to `<ConfigDir>/gui_prefs.msg` on
      quit and restored on launch.

Deferred:
- CLI feature parity: hooks/MCP status surface, `/compact` inside the
  GUI. (Running cost estimate now shown in the token bar.)
- Wiring an actual caller for `AskChoice` in the tool loop.
- Streaming-markdown intra-line rendering (currently line-by-line;
  partial lines complete on the next chunk).

---

## Haiku-native extras

Features that don't exist in Claude Code but would make this CLI feel
native on Haiku. Sprinkle in along the roadmap as they become natural.

- Respect Haiku's system accent color in REPL prompt styling.

## Non-goals

Things explicitly excluded from this roadmap:

- **IDE integrations** (VS Code, JetBrains). Haiku users mostly live
  in Pe, Koder, or the terminal; an IDE extension would be a separate
  project.
- **Image input in the terminal**. The Messages API supports it, but
  there's no good terminal workflow for attaching images on Haiku.
  (The native GUI *does* support image attachments as of v1.9 — the
  exclusion is specifically the CLI/Terminal front-end.)
- **Windows and Linux ports**. macOS builds via nix exist only as a
  development convenience for prototyping before building on Taurus.
- **Plugin marketplace**. The user config directory is the plugin
  system; anything else is scope creep.
- **Parity with every Claude Code slash command**. We'll ship the
  commands that are actually useful on Haiku, not every one.
- **Re-implementing Anthropic's agentic loop wholesale**. We mirror
  the parts that matter for day-to-day coding; experimental Claude
  Code features come later or never.

## Open questions

- **Cost estimation source of truth**: hard-code per-model prices or
  ship a JSON lookup updated each release? Out-of-date baked-in numbers
  are worse than no numbers.
- **Tool-use safety on Haiku**: there's no user namespace or filesystem
  sandbox, so `Bash` and `Write` run with the invoking user's full
  privileges. Permission prompts are the only safety layer — is that
  enough, or do we need per-tool denylists?
- **MCP on Haiku**: does every MCP server we care about actually build
  and run on Haiku, or do we document a Linux-VM fallback for servers
  that don't?
- **Session storage growth**: `history.json` grows unbounded today.
  Cap at N turns, rotate to timestamped files, or rely on `/compact`?

## How to contribute

File an issue on Gitea for anything in this roadmap you'd like
prioritized, or a feature that isn't listed. Pull requests welcome
against `dev` — see `docs/GIT_WORKFLOW.md` for the branching model.

---

_This roadmap is a plan, not a promise. Scope and order will shift as
real usage on Haiku exposes what actually matters._
