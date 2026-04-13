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

### v0.3 — Terminal UI polish

The current REPL is `getline` + raw stream output. Claude Code's TUI
gets a lot of its feel from small things — live markdown rendering,
a thinking spinner, proper line editing. Match the parts that work on
Haiku's Terminal app (ANSI + UTF-8, 256 colors, no sixel/kitty).

- [ ] **Markdown rendering** in assistant output: bold, italic, inline
      code, code blocks with language label, bullet lists, headings.
      Falls back to raw text when stdout isn't a TTY so piped output
      stays scriptable.
- [ ] **Syntax-highlighted code blocks** — minimal tokenizer per
      language (C/C++, Python, shell, JSON to start), ANSI 256-color
      theme, auto-detected from the code block's language label.
- [ ] **Line editing via libedit/readline** — arrow-key navigation,
      in-memory history, emacs-style bindings, persisted REPL history
      at `~/config/settings/claude-cli/repl_history`.
- [ ] **Multi-line input** — trailing backslash (or `"""` fence) opens
      a continuation prompt for pasting multi-paragraph messages.
- [ ] **Live status line** at the bottom of the REPL frame showing
      model, turn count, token totals, and elapsed time of the
      in-flight request. Disappears cleanly when the session exits.
- [ ] **Thinking spinner** between request submit and first token,
      erased automatically when the stream starts.
- [ ] **Distinct turn styling** — color and bold for `you>` / `claude>`
      prompts, dim for meta notes like `[resumed N messages]`. Honors
      `NO_COLOR` and non-TTY stdout.
- [ ] **Terminal resize handling** — redraw status line on SIGWINCH.
- [x] **Palette portability** — the TUI sticks to standard 16-color
      ANSI codes (no 256-color / truecolor) so the user's terminal
      theme (dark or light) controls the actual rendered colors.
      True dark/light auto-detection via terminal queries or
      `COLORFGBG` is deferred until we hit a concrete need.
- [ ] **Unicode box-drawing frame** around the REPL — deferred:
      fixed-bottom frames need DECSTBM scroll regions and careful
      SIGWINCH tracking, which is a bigger refactor than v0.3 warrants.

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
      Auto-refresh of expired OAuth tokens mid-stream is still a
      future polish item.
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
