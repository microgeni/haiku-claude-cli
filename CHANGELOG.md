# Changelog

All notable changes to this project are recorded here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **BFS attribute tools** (Haiku only, `#ifdef __HAIKU__`):
  - `Query` — execute BFS filesystem queries via the `query`
    command. Indexed attribute lookups in O(1) instead of
    directory traversal. Auto-approved (read-only).
  - `ReadAttr` — read or list extended attributes on a file
    via `catattr` / `listattr`. Auto-approved.
  - `WriteAttr` — write typed attributes via `addattr`.
    Permission-gated like `Write` / `Edit`. Preview shows the
    path, attribute name, type, and value. Enables the
    `claude:summary` / `claude:component` workflow for
    persisting understanding across sessions.
  - `IndexAttr` — create BFS attribute indices via `mkindex`.
    Permission-gated with a volume-level warning in the
    preview.
  All four share a new `exec_capture()` helper that
  fork/exec/waitpid's a command and captures output (same
  pattern as `run_bash` but reusable). Definitions are
  registered via `haiku_definitions()` and appended to
  `tools::definitions()` only on `__HAIKU__` builds; macOS
  builds skip them entirely.
- **BFS tools benchmark script** at
  `tests/bfs_tools_test.sh` — measures token savings
  (full-read vs. summary-read) and query latency (glob vs.
  BFS query) on real Haiku hardware. Writes test
  `claude:summary` attributes, reads them back, compares
  tokens, times BFS queries, and cleans up.

### Fixed
- **Session resilience** — TCP keepalive (60 s / 30 s),
  15 s connect timeout, per-turn OAuth token refresh,
  persistent curl handle with connection reuse, exponential
  backoff retry on 429/5xx/curl-transient (3 attempts,
  1 s / 2 s / 4 s).
- **`/remote-control` Telegram output now visible on
  terminal** via DECSC/DECRC cursor save/restore into the
  scroll region. Local turns mirror to the primary Telegram
  chat via `mirror_to_primary()`.

## [1.1.2] - 2026-04-15

### Added
- **`/remote-control` slash command** toggles a background
  Telegram poller on and off inside the regular interactive
  REPL (`claude -i`). `/remote-control` or
  `/remote-control on` starts a lightweight poller thread
  that accepts messages from `config.telegram.allowed_user_ids`
  and runs each one through `send_with_tools` on the local
  machine, posting the final response via `sendMessage`.
  `/remote-control off` stops and joins the poller. The
  fixed-bottom status frame shows a green
  `Remote Control active` label while running, with a
  `· muted` suffix in yellow when `/mute` is toggled on.
  Each Telegram user gets an independent rolling history;
  local REPL turns and remote turns are serialized against
  each other via a shared `remote_mutex` but don't share
  conversation state. New `RemoteControl` class in `main.cpp`
  owns the poller thread, allowed-users set, per-user
  histories, and the start/stop lifecycle. Validates
  `config.telegram` at `/remote-control` invocation time so
  the error message (if any) appears on the REPL rather than
  at startup. Deliberately leaner than `claude telegram`:
  no streaming edits, no typing indicator, no inline-keyboard
  buttons, no local input mirroring — use the dedicated
  subcommand for the full experience.
- **Fixed-bottom status frame** in interactive REPL and Telegram
  bridge mode. A `tui::install_status_bar()` helper sets a
  DECSTBM scroll region (`\e[1;N-2r`) that carves off the
  bottom two rows of the terminal: a dimmed horizontal rule and
  a status row showing the short model name, turn count,
  cumulative input / output tokens (compact `k` / `M` suffix),
  and max-tokens cap. Chat history and the libedit prompt live
  in the remaining rows and scroll normally above the frame.
  - **Telegram bridge footer** appends a right-aligned
    `Remote Control active` label in green (`tui::green()`) so
    the operator always sees that the bridge is live. When
    `/mute` is toggled, a `· muted` suffix in yellow is
    appended and the footer redraws on every toggle (from
    either the local prompt or a Telegram chat).
  - **SIGWINCH handling** tracks resize events via a new
    `tui::consume_resize_pending()` flag; the REPL loop
    consumes the flag between prompts and calls
    `tui::redraw_status_bar()` to re-read `TIOCGWINSZ`, reset
    the scroll region, and repaint the frame.
  - **Crash-safe teardown** via `std::atexit` + a `SIGTERM`
    handler that calls `teardown_status_bar()` before
    re-raising. Normal REPL exit uses an RAII
    `StatusFrameGuard` scoped to the loop. `SIGINT` is
    deliberately left to `InterruptGuard` so Ctrl+C still
    cancels in-flight requests without tearing down the
    frame.
  - New `tui::terminal_rows()` helper parallel to
    `terminal_width()`, same caching and SIGWINCH refresh.
  - No-op on non-TTY / `--plain` / color-disabled paths and
    when the terminal has fewer than 4 rows (so one-shot
    invocations and piped output keep behaving identically).
- **Aligned markdown tables.** The renderer now detects pipe
  tables, buffers rows until the table ends, computes per-
  column display widths, and emits aligned box-drawing output
  with bolded headers, dimmed borders, and honored
  `:--- / ---: / :---:` alignment markers. `render_inline` was
  refactored into a string-returning helper so cell contents
  retain their inline formatting (bold, italic, inline code)
  while the width math sees the already-formatted string.
- **Transient status line during the thinking window.** The
  existing spinner's label now carries live context — short
  model name, rolling message count, `max_tokens` cap — and
  appends the elapsed time plus an `esc:cancel` hint. The line
  is truncated to the current `terminal_width()` so it stays on
  one row even on narrow terminals or after a mid-session
  resize. Rendered only between request submit and first
  streamed token, then erased — no DECSTBM scroll region, no
  libedit dance, clean teardown when streaming starts.
- **SIGWINCH resize handling.** A new `tui::terminal_width()`
  helper reads `TIOCGWINSZ` lazily and caches the result;
  `tui::install_sigwinch_handler()` (called once from `main()`)
  marks the cache dirty on every resize event so the next
  spinner tick re-reads the width and re-truncates. No-op when
  stdout isn't a TTY.
- **ESC key cancels in-flight work.** A new `EscInterruptGuard`
  RAII helper is scoped around `curl_easy_perform` in
  `send_conversation`: it saves the current termios, puts stdin
  into cbreak mode (`ICANON`/`ECHO` off, `VMIN=0`, `VTIME=0`),
  spawns a background thread that polls stdin for a bare `0x1B`
  byte, and sets `g_interrupted` on detection — reusing the
  same curl-abort path as Ctrl+C. CSI escape sequences (arrow
  keys etc.) are ignored by requiring a single-byte read so
  accidental keystrokes during streaming don't kill the turn.
  On scope exit the termios is restored so libedit's next
  prompt reads in cooked mode. No-op when stdin isn't a TTY.

## [1.1.1] - 2026-04-14

### Fixed
- **Max-tokens truncation is no longer silent.** When an API
  round ended with `stop_reason: "max_tokens"`, `send_with_tools`
  exited its loop (since the stop reason wasn't `"tool_use"`) and
  one-shot mode returned exit 0 with no warning. Any half-serialized
  `tool_use` block in that round was never executed — the user saw
  their Bash/Read exploration run fine, then silence, and no files
  written. The loop now logs a loud
  `error: response truncated at the max_tokens cap (output=N / max=M)`
  line to stderr with the re-run guidance, and explicitly notes that
  the in-flight tool call was dropped. `refusal`, `pause_turn`, and
  any other unexpected `stop_reason` also emit a clear stderr line
  instead of silent exit; `end_turn` and `stop_sequence` stay quiet
  as the happy path.
- **Orphan `tool_use` blocks on truncation no longer poison REPL
  history.** When `stop_reason == "max_tokens"`, partial `tool_use`
  blocks (empty or malformed `input`, no matching `tool_result`)
  are stripped from the assistant turn before it's pushed onto
  `messages[]`. Previously, the next REPL continuation would send
  a `tool_use_id` the API couldn't match to a `tool_result` and
  fail with HTTP 400. Plain text blocks from the same round are
  preserved.
- **One-shot runs no longer silently deny destructive tools.**
  `prompt_permission` used to call `std::getline(std::cin, …)`
  unconditionally, which returned EOF immediately whenever stdin
  wasn't a usable terminal (piped input already consumed, script
  invocation, subprocess with closed stdin). The CLI would emit
  the `allow Write?` prompt, read nothing, fall through to
  `Permission::Deny`, and return `"user denied permission to run
  Write"` as the tool result — leaving the assistant to narrate a
  plausible-sounding "I'll create X" that never actually touched
  the disk. The permission path now detects `!isatty(stdin)`, prints
  a clear `[tool: Write -> denied: no TTY to prompt]` line on stderr
  with guidance, and surfaces the same message in the
  `tool_result` so the model can tell the user why the call failed
  instead of fabricating success.

### Changed
- **Default `max_tokens` raised from 1024 to 8192.** 1024 was fine
  for a text-only CLI but routinely busted during `tool_use` rounds
  where the model emits a large Write/Edit JSON argument (e.g. a
  full source file). 8192 gives tool-using sessions real headroom
  without meaningfully affecting latency or cost. Users who want
  the old behavior can set `"max_tokens": 1024` in `config.json`
  or pass `-t 1024`.

### Added
- **`/mute` and `/unmute` bridge commands** — suppress every
  outbound Telegram Bot API call (`sendMessage`, `editMessageText`,
  `sendChatAction`, placeholder sends, streaming edits, final
  responses, numbered-list buttons, typing indicator) until
  `/unmute`. Incoming messages are still processed normally:
  tools still run, the operator's local terminal still shows
  the full conversation, nothing just leaves the machine. Valid
  from both the local libedit prompt and any authorized Telegram
  chat, backed by a shared `g_telegram_muted` atomic flag.
  `/mute`'s ack is sent *before* the flag flips and `/unmute`'s
  ack is sent *after* it flips, so every state transition is
  visible on the Telegram side instead of being suppressed by
  its own command. All per-call gating lives in four `tg_*`
  wrapper lambdas (`tg_send`, `tg_send_id`, `tg_edit`,
  `tg_typing`) inside `run_telegram_bridge` so the check is in
  one place for every path through the bridge.
- **`-y`, `--yes` flag** — auto-approves destructive tools
  (`Bash`, `Write`, `Edit`) for the current invocation without
  the y/a/n prompt. Intended for one-shot and piped-stdin runs
  where no interactive dialog is possible. Seeds the session
  allowlist so every destructive call in the same run is cleared
  at once.
- **`allow_destructive_tools` config key** (top-level in
  `config.json`) — persistent equivalent of `-y` for setups that
  always run headless. Also applied by the Telegram bridge when
  its own `telegram.allow_destructive_tools` isn't set.
- **Granular denial reasons** — `prompt_permission` now returns a
  specific string for each denial path (`no TTY`, `stdin closed`,
  `user declined`, `non-interactive mode`), threaded into the
  `tool_result` content so the model sees *why* the call was
  blocked instead of a generic message.

## [1.1.0] - 2026-04-14

Remote control via Telegram. Drive the local CLI from your phone
while tools execute on your machine — same outcome as Claude
Code's undocumented `/remote-control`, built on Telegram's
public Bot API instead.

### Added
- **`claude telegram` subcommand** — new headless bridge loop
  alongside `login`/`logout`. Long-polls Telegram for incoming
  messages from allowed users, routes each through
  `send_with_tools` on the local machine, and mirrors the
  assistant's final text back via `sendMessage`. Hooks,
  `CLAUDE.md` memory, and MCP continue to apply — the bridge
  just replaces the REPL input source; the rest of the stack
  is unchanged.
- **`src/telegram.{h,cpp}` Bot API client** over libcurl —
  `getUpdates` with 30 s long-poll and offset persistence,
  `sendMessage` with ~3800-char chunking to stay under
  Telegram's 4096-char cap, `sendChatAction`, `editMessageText`
  (ignores cosmetic "not modified" failures),
  `answerCallbackQuery`, and `send_message_with_id` that
  returns the first chunk's `message_id` so later edits can
  target it.
- **`telegram` config key** in `config.json` with required
  `bot_token` + `allowed_user_ids` (integer array) and optional
  `allow_destructive_tools` boolean. Unauthorized user IDs are
  dropped silently (logged to the opt-in log file) so random
  chats can't fingerprint the bot.
- **Per-user rolling history** — each authorized Telegram user
  has their own in-memory `messages[]`. `/new` and `/clear`
  reset that user's history; `/help` and `/start` reply with
  the per-user command list.
- **Non-interactive permission mode** for the bridge —
  destructive tools (`Bash`, `Write`, `Edit`, any MCP tool)
  are blanket-allowed or blanket-denied based on
  `allow_destructive_tools`. Read-only tools (`Read`, `Glob`,
  `Grep`, `WebFetch`, `WebSearch`, `Task`, `TodoWrite`,
  `TodoRead`) are always auto-approved. Defaults to blocking
  destructive tools.
- **Inline-keyboard buttons for numbered choices** — when
  Claude's reply contains a numbered list (two or more
  `1. …`, `2. …` entries at line start), each option is
  rendered as a tap-to-answer button under the message, up
  to ~28 chars per label. Only the last chunk of a split
  message carries the keyboard so it sits under the final
  visible piece. Tapping sends the number back as the user's
  next message so multi-step prompts feel conversational.
- **Local libedit prompt alongside the Telegram poller** —
  `claude telegram` now runs a background poller thread while
  the main thread drives a standard `>` prompt, so the bridge
  operator can type from the laptop too. A shared
  `process_mutex` serializes the two so there's never a
  concurrent `send_with_tools` call. Local input runs with
  `g_non_interactive_tools=false` so destructive tools still
  prompt y/a/n on the local terminal; Telegram input keeps
  the config-flag path.
- **Local prompt mirrors to the primary Telegram chat** —
  a deterministic `primary_user_id` (smallest allowed ID) is
  picked at startup. Local input echoes as `> <text>` into
  that chat, a `…` placeholder is sent, the streaming reply
  updates in place, and history is shared with
  `user_messages[primary]` so a conversation can hop between
  the laptop and the phone without losing context.
- **Typing indicator** — an updater thread live for the
  duration of each Telegram-sourced `send_with_tools` call
  pushes `sendChatAction(..., "typing")` once per second so
  the remote user sees the animation instead of dead silence.
- **Streaming edits** — `process_sse_event`'s `text_delta`
  branch now appends incoming chunks to a `StreamProgress`
  buffer guarded by a mutex and a monotonic version counter.
  The bridge first sends a tiny `…` placeholder and captures
  its `message_id`, then the same updater thread periodically
  `editMessageText`s it with the growing buffer. The final
  edit after `send_with_tools` returns commits the complete
  text plus the inline keyboard in one call. The remote chat
  sees the reply build up token-by-token instead of waiting
  for a wall of text.
- **Slash commands from the local prompt** — `/usage`,
  `/help`, `/clear`, `/model`, `/compact`, `/todos`,
  `/memory`, and any custom `.claude/commands/*.md` commands
  now work from inside `claude telegram` the same way they do
  in `claude -i`. `/clear` clears the primary chat's running
  history (since the local prompt shares it), `/model NAME`
  swaps models mid-bridge for subsequent turns across both
  surfaces, and `/usage` reflects real cumulative session
  totals because `active_model` / `turn_count` /
  `session_input` / `session_output` are now real locals in
  `run_telegram_bridge` updated after every successful reply
  from either source.

### Known limitations
- libedit's local prompt can be clobbered when a Telegram
  update prints to stdout between redraws. Proper
  `rl_save_prompt` / `rl_forced_update_display` integration
  is deferred.
- Inline-keyboard *permission* prompts for destructive tools
  (letting `Bash`/`Write` run interactively without the
  config flag) are still deferred, as are voice notes /
  image attachments and multi-chat / group-chat support.

## [1.0.1] - 2026-04-14

### Added
- **`/usage` slash command** rendering the three subscription
  windows as a Claude Code-style dialog — filled-block progress
  bars with percentage and local-time reset for each window.
  Data comes from Anthropic's `anthropic-ratelimit-unified-*`
  response headers (5h session, 7d all-models, 7d Sonnet-only),
  captured via `CURLOPT_HEADERFUNCTION` and refreshed on every
  successful or error request. Session summary line shows
  model / turns / input / output / estimated cost above the
  bars, plus a `binding window` note from the
  `representative-claim` header.

### Removed
- **`/cost` slash command** — fully subsumed by `/usage`, which
  now shows the same session summary as its first line plus the
  additional rate-limit windows.

### Deferred
- `/remote-control` — filed as v1.1 in ROADMAP.md. Protocol is
  not publicly documented; implementation is blocked on
  reverse-engineering the official Claude Code bundle or
  mitmproxy-capturing the handshake.

## [1.0.0] - 2026-04-14

First stable release. All roadmap milestones from v0.2 through v1.0
are now complete; the CLI has feature parity with Claude Code's core
functionality on the parts that make sense for a native Haiku build.

### Added
- **Man page** at `docs/claude.1` — full troff-format reference
  covering commands, options, slash commands, config.json keys,
  hooks/mcp_servers layout, environment variables, files, and
  examples. Installed by `make install` to
  `$(PREFIX)/documentation/man/man1/claude.1` and shipped inside
  the HPKG under the same path.
- **`-V` / `--version` flag** prints the binary's semver. The same
  `kVersion` constant also drives the Messages API User-Agent so
  Anthropic sees the correct version string per release.
- **Opt-in structured logs** at
  `~/config/settings/claude-cli/logs/claude-YYYY-MM-DD.log`, enabled
  via `{ "logging": { "enabled": true } }` in config.json. Records
  session start, per-turn token totals, tool invocations, and HTTP
  error details with ISO-style timestamps.
- **Rewritten README** — full walkthrough of install (HPKG,
  from-source, nix dev shell), authentication, REPL usage, slash
  commands, every built-in tool, memory files, hooks, MCP, config
  keys, and environment variables.

### Changed
- **Human-friendly non-2xx error messages.** Anthropic's error
  envelope is parsed for `error.message`; well-known HTTP codes get
  plain-language explanations (401 suggests logout/login, 403
  forbidden, 429 distinguishes the opaque Claude Code gate from a
  real rate limit, 5xx labels server errors). The blanket
  "response body: ..." dump is gone. Errors are also mirrored into
  the log file when logging is enabled.

### Deferred
- Automatic OAuth token refresh mid-stream on 401 (still a manual
  logout/login).
- HaikuDepot submission (requires a HaikuPorts recipe and external
  review; the HPKG is downloadable from the Gitea release page).

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
