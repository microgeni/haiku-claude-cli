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
- [ ] **Unicode box-drawing frame** around the REPL — explicitly
      deferred. Fixed-bottom frames need DECSTBM scroll regions and
      careful SIGWINCH tracking, which is a bigger refactor than
      v0.3 warrants. The transient status line above covers the
      actually-useful information window (the thinking phase) at a
      fraction of the complexity.

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

## Haiku-native extras

Features that don't exist in Claude Code but would make this CLI feel
native on Haiku. Sprinkle in along the roadmap as they become natural.

- Desktop notification via Haiku's `notify` when a long-running task
  completes.
- Filesystem attribute queries as a first-class tool
  (`query 'BEOS:TYPE=text/x-source-code'` style).
- Tracker integration: accept a dropped file or folder as a session
  scope by registering a `application/x-vnd.claude-cli` signature.
- Open URLs from Claude's responses via Haiku's `open` command.
- Respect Haiku's system accent color in REPL prompt styling.
- HVIF application icon for the `claude` binary via `BEOS:ICON`
  attribute. Design in Icon-O-Matic, export as `.hvif`, apply
  via `addattr -t icon -f icon.hvif BEOS:ICON` in the Makefile
  install target. Shows up in Tracker, HaikuDepot, and pkgman.

## Non-goals

Things explicitly excluded from this roadmap:

- **IDE integrations** (VS Code, JetBrains). Haiku users mostly live
  in Pe, Koder, or the terminal; an IDE extension would be a separate
  project.
- **Image input**. The Messages API supports it, but there's no good
  terminal workflow for attaching images on Haiku without a GUI surface.
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
