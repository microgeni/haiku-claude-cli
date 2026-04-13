# Changelog

All notable changes to this project are recorded here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.10.0] - 2026-04-14

### Added
- **WebFetch tool** — libcurl-based HTTP/HTTPS GET. Follows up to 5
  redirects, 30 s overall timeout, 10 s connect timeout, transparent
  content decoding. Returns `HTTP <status>  (<content-type>)` header
  followed by the response body, truncated to `max_bytes` (default
  32768). Auto-approved (no permission prompt for reads).
- **WebSearch tool** — Brave Search API wrapper. Registered in
  `tools::definitions()` only when `BRAVE_SEARCH_API_KEY` is set, so
  Claude doesn't see an unusable tool on machines without a key.
  Returns up to 10 `title` / `url` / `description` blocks from
  `/res/v1/web/search`.
- **Task tool** — one-shot sub-agent. Spawns a fresh
  `send_conversation` call with the same auth/model/memory/system but
  `include_tools=false`, streams the sub-agent's response to the
  terminal under a dim `sub-agent:` label, and returns the final
  assistant text as the tool result. Sub-agent token usage is
  aggregated into the session totals. Recursion-safe because the
  sub-agent has no tool access at all (including no Task).
- **TodoWrite / TodoRead tools** — in-process todo list for
  multi-step work. TodoWrite replaces the entire list from a
  `{todos: [{content, status}]}` input; statuses are `pending`,
  `in_progress`, or `completed`. TodoRead returns the current list
  as a checklist ([x]/[-]/[ ]).
- **`/todos` slash command** prints the current in-session list
  under a "current todos:" header, plus added to tab completion.

## [0.9.0] - 2026-04-14

### Added
- **MCP (Model Context Protocol) stdio client.** The CLI spawns
  configured MCP servers as subprocesses, speaks newline-delimited
  JSON-RPC 2.0 over their stdin/stdout, runs the `initialize`
  handshake (protocolVersion `2024-11-05`, client info
  `haiku-claude-cli`), then discovers their tools via `tools/list`.
- Server configuration lives under a new `mcp_servers` key in
  `config.json`, keyed by server name, each entry an object with
  `command` (required), `args` (string array), and `env` (string
  map). Servers that fail to spawn or initialize log to stderr and
  are skipped.
- MCP-provided tools are **namespaced** as `mcp__<server>__<tool>`
  when advertised to Claude, so they can't collide with built-ins.
  `tools::definitions()` merges them into the `tools` array sent
  with every request.
- MCP tool calls route through `tools/call` transparently from
  `tools::run()`. The tool result's text content blocks are
  concatenated into a single `ToolResult` with `isError` propagated.
- Every `mcp__*` tool `require_permission` by default — the user
  is always prompted before a third-party MCP server runs.
- At-exit teardown: an `atexit` handler closes pipes and `waitpid`s
  every spawned child so the CLI doesn't leak subprocesses.

### Deferred
- HTTP/SSE transport for remote MCP servers.
- `resources/` and `prompts/` MCP capabilities (tools-only slice).
- Per-server allowlisting inside the permission prompt.

## [0.8.1] - 2026-04-14

### Changed
- Running `claude` with no message or `-i` flag now drops directly
  into interactive mode when stdin is a terminal. `-i` becomes a
  formality in the common case; piped/redirected invocations still
  hit the usage error path so scripts fail loudly on empty input.
- REPL user prompt simplified from `you> ` to `> ` (bold cyan), to
  match Claude Code and most modern TUIs. The `claude> ` response
  prompt stays (bold magenta) so turns are still visually distinct.

## [0.8.0] - 2026-04-14

### Added
- **Shell-command hooks** registered in `config.json` under a `hooks`
  key, keyed by event name, with each entry a
  `{ matcher?, command }` object. Five event types:
  - `SessionStart` — once when the REPL starts.
  - `UserPromptSubmit` — before a user message is committed to
    history. Block drops the turn and prints `[hook blocked prompt]`.
  - `PreToolUse` — before a tool runs, after the `[tool: ...]`
    notice but before the permission prompt. Block synthesizes a
    denied `tool_result` with `hook blocked <tool>`.
  - `PostToolUse` — after a successful tool run, with the result
    included in the payload.
  - `Stop` — after a successful turn, with the assistant's text in
    the payload.
- Each hook runs as `sh -c <command>`. Event payload is written to
  the hook's stdin as JSON (with `event` and, for tool events,
  `tool_name` injected). Stderr is forwarded verbatim so hooks can
  talk back to the user. The `matcher` field on PreToolUse/
  PostToolUse filters by tool name; empty matcher matches any tool.
- Non-zero exit from any matching hook means `Block` — the caller
  aborts whatever was about to happen.

### Deferred
- Project-level hooks file (`.claude/hooks.json`).
- Stdout-driven context injection from hooks into the prompt.
- Per-hook timeouts.

## [0.7.0] - 2026-04-14

### Added
- **Project memory** — `CLAUDE.md` in the current working directory
  is loaded as a system-prompt preamble on every turn, so per-project
  context doesn't have to be pasted each time. Edits take effect on
  the next round-trip; no restart needed.
- **User memory** — `~/config/settings/claude-cli/CLAUDE.md` is
  loaded as a user-level preamble, appended before project memory.
- **`/memory [user]`** slash command opens the relevant `CLAUDE.md`
  in `$EDITOR` (falls back to `nano`), creating parent directories
  for the user-level file if needed.
- **Custom slash commands** loaded from `.claude/commands/*.md`
  (project) and `~/config/settings/claude-cli/commands/*.md` (user,
  respecting `XDG_CONFIG_HOME`). The filename minus `.md` becomes
  the command name; the file body is the prompt template. `{{args}}`
  is substituted with whatever text followed the command in the REPL.
  Project definitions override user ones on name collision.
- **Tab completion** in the REPL for slash commands — both built-in
  and custom, via libedit's `rl_attempted_completion_function`.
  Triggers only when the current word is at the start of the line
  and begins with `/`, so argument completion still behaves normally.
- `/help` now lists any loaded custom commands after the built-in set.

### Fixed
- OAuth-gated `/v1/messages` requests no longer 429 with
  `rate_limit_error / "Error"` when the system prompt contains
  content beyond the Claude Code preamble (e.g. a loaded `CLAUDE.md`).
  The fix sends `system` as a two-element array — `[{type:"text",
  text:"<preamble>"}, {type:"text", text:"<extra>"}]` — instead of a
  concatenated string. Anthropic's fingerprint check is positional
  on `system[0]`, not a substring scan.

## [0.5.0] - 2026-04-14

### Added
- **Write tool** — creates or overwrites a file, auto-creates parent
  directories, returns a byte-count summary. Requires permission on
  first use.
- **Edit tool** — exact-string replacement in an existing file, with
  an optional `replace_all` flag for multi-match cases. Errors on
  zero matches and on >1 match without `replace_all`. Requires
  permission on first use.
- **Permission preview system** — `tools::preview(name, input)`
  returns an optional multi-line description of what a tool would
  do, rendered dim between the `[tool: ...]` notice and the
  yes/always/no prompt. Write's preview shows new-vs-overwrite with
  byte/line counts and the first 10 lines of content; Edit's preview
  renders a block-style diff (`-` old_string / `+` new_string) with
  the line number of the first match, capped at 12 lines per side.
- **Out-of-cwd warning** — Write/Edit paths outside the current
  working directory are flagged with `[WARNING: outside cwd]` in
  the preview. Not a hard deny; the permission prompt is the safety
  net.

## [0.4.0] - 2026-04-14

### Added
- **Tool use** — Claude can now read, search, and execute on the local
  machine via the Messages API `tools` parameter. The CLI handles the
  full `tool_use` / `tool_result` round-trip loop across streaming and
  feeds results back until Claude returns `stop_reason=end_turn`.
- **Read tool** — returns the contents of a text file, with optional
  `start_line` / `end_line` range (both 1-indexed, inclusive).
- **Glob tool** — POSIX `glob(3)` wrapper. Takes a shell-style pattern
  and returns matching paths sorted newest-first by mtime.
- **Grep tool** — `fork`/`execvp` wrapper around POSIX
  `grep -rnH -e PATTERN --`, with a 32 KiB output cap and clean
  handling of no-match (exit 1 → `(no matches)`) vs real errors.
- **Bash tool** — runs a shell command via `sh -c`, captures combined
  stdout+stderr, returns exit code + output (32 KiB cap). The only
  tool that requires an interactive permission prompt.
- **Permission prompt system** — a session-scoped allowlist of
  pre-approved tool names, with a prompt on first use of any tool
  marked "dangerous". Answer `(y)es` for one-shot allow, `(a)lways`
  to add to the session allowlist, anything else denies. Denied
  tools return a `user denied permission` result so Claude can
  adapt gracefully. Read/Glob/Grep auto-approve; only Bash prompts.
- **Spinner stops on `message_start`** — previously waited for first
  renderer output, which left a stale spinner on tool-use-only
  responses that had no text block.

### Changed
- `StreamState` now carries structured `content_blocks`, per-block
  accumulators (`current_type`, `current_text`, `current_tool_id`,
  `current_tool_name`, `current_tool_input_raw`), and the finalized
  `stop_reason`. `SendResult` exposes `content_blocks` and
  `stop_reason` so callers can drive the tool-use loop.
- `send_conversation` takes an `include_tools` parameter that toggles
  the request's `tools` field. `/compact` calls it with
  `include_tools=false` so summary passes can't reach for tools.
- New `send_with_tools` wrapper is the entry point for both one-shot
  and REPL turns. It loops on `stop_reason == "tool_use"`, appending
  the assistant message and the synthesized `tool_result` user
  message, until Claude is done.
- The REPL snapshots `messages` before each `send_with_tools` call
  and restores the snapshot on failure, so a partial tool-use loop
  doesn't leave the conversation in a half-committed state.

## [0.3.0] - 2026-04-14

### Added
- **Slash commands** in the REPL via a dispatcher that routes any
  input beginning with `/`:
  - `/help` (or `/?`) lists the available commands.
  - `/clear` resets the running conversation and session counters.
  - `/model [NAME]` prints the current model or swaps to a new one
    mid-session without restarting the REPL.
  - `/compact` asks Claude to summarize the running history and
    replaces it with a single `[previous context]` + summary pair,
    preserving important decisions, code, and open questions.
  - `/cost` prints a session cost estimate based on cumulative
    input/output tokens × per-model prices (from config or built-in
    fallbacks). On OAuth sessions it notes the cost is informational
    since billing is against the Pro/Max subscription quota.
  - `/exit`, `/quit` leave the REPL (alongside the existing
    `exit`/`quit`/`:q` keywords and Ctrl+D).
- **Config file** at `~/config/settings/claude-cli/config.json`
  (Haiku) or the XDG equivalent. Optional JSON with keys: `model`,
  `max_tokens`, `system`, `show_usage`, `prices`. CLI flags override
  the config. `--help` lists the file path and the accepted keys,
  and the shown defaults reflect whatever the config resolves to.
- **Per-model price table** for `/cost`. Config's `prices` key
  accepts per-model `{"input": N, "output": N}` entries in
  dollars-per-million-tokens; built-in fallbacks substring-match
  sonnet/opus/haiku when a model isn't in the config.

### Changed
- `interactive_loop` now maintains its own live `model` string so
  `/model` can mutate it mid-session. The welcome banner hints at
  `/help` instead of just the exit keywords.

### Fixed
- Pressing Ctrl+C while Claude is streaming now aborts the in-flight
  request gracefully — libcurl's `CURLOPT_XFERINFOFUNCTION` is wired
  to a SIGINT-set flag, partial rendered output is flushed, a dim
  `[interrupted]` note prints, and the REPL prompt returns instead
  of the process dying. Failed user turns are dropped from history
  (existing behavior), so the conversation stays consistent.

## [0.2.0] - 2026-04-13

### Added
- `-s/--system TEXT` — custom system prompt. When OAuth is used the
  required Claude Code prefix is preserved and the user text is
  appended; with an API key the user text is used verbatim.
- `-u/--usage` — after a one-shot response, print input/output token
  counts to stderr.
- `-r/--resume` — preload the REPL with the last saved session from
  `~/config/settings/claude-cli/history.json` (Haiku) or the XDG path
  elsewhere. Implies `-i`. Conversation history is auto-saved after
  every successful turn.
- `CHANGELOG.md` following Keep a Changelog; release notes on Gitea
  releases are now extracted from it automatically.
- `docs/ROADMAP.md` describing milestones from v0.2 through v1.0
  toward Claude Code feature parity, including a dedicated Terminal
  UI polish milestone.
- Terminal UI polish (new `tui` module):
  - ANSI color detection honoring `NO_COLOR`, `CLICOLOR=0`,
    `TERM=dumb`, plus `--plain` / `--color` overrides.
  - Bold cyan `you> ` and bold magenta `claude> ` REPL prompts;
    dim-styled meta notes.
  - Braille "thinking" spinner with live elapsed-seconds counter
    between request submit and first rendered output.
  - Streaming markdown renderer: **bold**, *italic* / _italic_,
    `inline code`, fenced code blocks, `#`/`##`/`###` headings,
    bullet and numbered lists.
  - Syntax highlighting inside code blocks for C/C++, Python, Shell,
    Rust, and JSON (keywords, strings, numbers, comments, C/C++
    preprocessor lines).
  - Line editing in the REPL via libedit: arrow-key history,
    emacs-style bindings, and persistent history at
    `~/config/settings/claude-cli/repl_history`.
  - Multi-line input: either `"""` (or `'''`) fenced blocks or
    trailing-backslash continuation, with a dim `... ` continuation
    prompt.
  - Per-turn status line printed after each REPL response showing
    turn number, elapsed time, and this-turn / session token totals:
    `[turn 3  1.8s  in 42/167  out 128/512]`.
  - Palette uses standard 16-color ANSI codes so the user's terminal
    theme (dark or light) drives the actual rendered colors.

### Changed
- CI build + test merged into one job (one runner warmup per push).
- `ci_scripts/build.sh` defensively installs `libedit_devel` on Haiku
  if the pkg-config file is missing so fresh Taurus images don't
  break CI.
- `.PackageInfo.in` now requires `lib:libedit`; the Makefile and
  Haiku HPKG build pull it via pkg-config.

## [0.1.1] - 2026-04-13

### Added
- HPKG packaging via a new `make package` target and a `.PackageInfo.in`
  template. Produces `claude_cli-<version>-<build>-x86_64.hpkg` on Haiku.
- Gitea Actions release job that runs on `v*` tag pushes: builds the HPKG
  on Taurus, generates a source tarball, and uploads both to the matching
  Gitea release (via `RELEASE_TOKEN` secret).

### Changed
- CI build and test jobs merged into a single `Build & Test` job so each
  push pays only one macos-arm64 runner warmup.

### Fixed
- Stdin slurping no longer hangs on non-TTY empty pipes (e.g. under
  `ssh host 'claude hi'` without `-t`, or CI jobs inheriting an idle
  runner pipe). Uses `poll()` before reading, and closes stdin
  explicitly at the ssh layer in CI.
- `upload_gitea_release.sh`: avoid a Python f-string backslash syntax
  error when building the release payload.

## [0.1.0] - 2026-04-13

### Added
- Initial usable release.
- One-shot mode: `claude "your message"` sends a single turn and prints
  the reply.
- Interactive REPL (`-i`, `--interactive`) with multi-turn history.
- Stdin piping: `cat file | claude "summarize"` appends piped content to
  the message when stdin is not a TTY.
- Flag overrides: `-m/--model MODEL`, `-t/--max-tokens N`.
- Streaming responses via Server-Sent Events — tokens print as Claude
  generates them.
- OAuth 2.0 with PKCE (`claude login` / `claude logout`) that
  authenticates against claude.ai and uses your Pro/Max subscription
  quota instead of per-token API billing.
- `ANTHROPIC_API_KEY` fallback when no OAuth tokens are stored.
- Haiku-native credential storage at
  `~/config/settings/claude-cli/credentials.json`.
- Build system: Makefile targeting C++17, pkg-config for libcurl,
  nlohmann_json, and OpenSSL; installs to
  `/boot/system/non-packaged/bin/claude` by default on Haiku.
- Nix dev shell (`flake.nix`) for prototyping on macOS before building
  on real Haiku hardware.
- Gitea Actions CI that SSHes from a macOS runner to Taurus and runs the
  build + functional tests on every push to `main`/`dev`.
