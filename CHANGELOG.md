# Changelog

All notable changes to this project are recorded here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.9.0] - 2026-07-24

### Added

- **GUI: Genio IDE integration** — when the desktop app is launched from
  Genio's **Tools ▸ Claude** menu, every file Claude writes or edits is
  opened (or refreshed) in the running Genio editor, with the cursor jumped
  to the edited line. Genio's extension launcher passes the active project
  and file on argv (`--project-dir` / `--file` / `--line`); their presence
  is what activates the integration, so a directly-launched GUI and the CLI
  are completely unaffected. Files are routed to Genio
  (`application/x-vnd.Genio`) via a `B_REFS_RECEIVED` message with `be:line`.
  Haiku-only; a no-op on the macOS dev build.
- **GUI: Tools ▸ Remote Control** — start/stop the Telegram remote-control
  bridge from the desktop app's Tools menu, mirroring the CLI's
  `/remote-control`. Allowed Telegram users can drive turns on the local
  machine while the GUI is running. The conversation is shared
  bidirectionally: remote turns see the GUI's transcript (via a
  thread-safe snapshot of the message history) and each completed remote
  turn is synced back into the local history and auto-saved. Invalid or
  missing `telegram` config produces a clear in-chat explanation instead
  of failing silently. While active, the Tools menu item shows a checkmark
  and the token bar displays a green **📡 REMOTE** badge.
- **GUI: Ludicrous-mode indicator** — when Tools ▸ Ludicrous Mode is
  enabled, the token bar now shows an amber **⚡ LUDICROUS** badge so the
  auto-approve state is always visible, matching the CLI's status-bar
  treatment. The badge tracks the menu checkmark.

- **Agent Skills** (Claude Code parity) — drop a `SKILL.md` into
  `<config>/skills/<name>/` (personal) or `./.claude/skills/<name>/`
  (project) and it becomes available as `/skill-name`. SKILL.md uses
  YAML frontmatter (`name`, `description`, `disable-model-invocation`,
  `allowed-tools`) plus a markdown body. The body supports `{{args}}`
  substitution and `` !`shell cmd` `` dynamic-context injection (the
  command's stdout is inlined before Claude sees the skill). Skills
  whose frontmatter does not set `disable-model-invocation: true` are
  advertised to the model so it can invoke them automatically when a
  request matches the description. Project skills override user skills
  of the same name. List them with `/skills`. Loaded by both the CLI
  and the GUI.
- **Subagents** (Claude Code parity) — define specialized agents as
  Markdown files in `<config>/agents/<name>.md` (personal) or
  `./.claude/agents/<name>.md` (project), with YAML frontmatter
  (`name`, `description`, `tools`, `model`, `color`) and a body that
  becomes the agent's system prompt. The `Task` tool now accepts a
  `subagent_type` argument: when it matches a loaded definition, the
  sub-turn runs with that agent's prompt and model override (the
  `haiku`/`sonnet`/`opus` aliases resolve to current model ids). The
  available subagents are listed to the model in the system prompt so
  it can delegate appropriately. List them with `/agents`.
- **GUI skill/subagent integration** — the GUI's slash-command popup now
  offers `/skills`, `/agents`, and every loaded skill name. Typing
  `/skills` or `/agents` prints the loaded definitions inline, and
  `/skill-name` expands the skill (with `{{args}}` and `` !`cmd` ``
  injection) into the next turn.


## [1.8.1] - 2026-07-23

### Fixed

- **Arrow key navigation restored** — pressing ←/→/↑/↓ in the prompt
  no longer drops keystrokes.  `bracketed_getc_impl` stashed non-paste
  CSI sequences (e.g. `\e[A`) into `g_paste_buf` and returned `\x1b`,
  but the next call jumped straight to `raw_getc()` without draining
  the buffer first, so the `[` and final byte were silently discarded
  and libedit never received a complete sequence.  Fixed by adding a
  buffer-drain check at the top of `bracketed_getc_impl`.

## [1.8.0] - 2026-07-22

### Added

- **Startup logo** — a block-letter ASCII art logo is displayed at launch,
  replacing the plain version string banner.  The final design uses the
  letter `C` in a clean, two-column-aware layout that looks correct in
  both standard and wide-character-aware terminals.
- **Expanding multi-line input area** — the compose box now grows
  vertically as you type; `Ctrl+J` inserts a soft newline without
  submitting the turn.

### Fixed

- **Responsive input during streaming** — terminal writes are now
  main-thread-only, eliminating the race condition that caused garbled
  output when the worker and the input loop wrote concurrently.
- **Logo rendering on Haiku Terminal** — resolved multiple rendering
  issues: Braille character width detection (1 vs 2 display columns),
  auto-wrap interference, correct column alignment, and consistent row
  widths.  The logo is now written directly via `write(2)` to bypass
  stream buffering, and uses `\x1b[39m` (foreground-reset only) instead
  of `\x1b[0m` to avoid clobbering background colour.

## [1.7.3] - 2026-05-03

### Fixed

- **GitHub mirror CI** — force-push with full `refs/heads/` refspec and
  `--no-thin` to avoid pack-transfer failures against a partially-synced
  remote.  Both Taurus and Asus runners now mirror every `dev` push and
  every release tag to `github.com/microgeni/haiku-claude-cli` reliably.
- **Runner portability** — CI build script now auto-installs missing devel
  packages (`curl_devel`, `nlohmann_json`, `openssl3_devel`, `libedit_devel`)
  and makes the `file` command optional so the pipeline works on any Haiku
  runner without manual setup.
- **`GH_DEPLOY_KEY` secret** — key is now stored base64-encoded in Gitea
  and decoded at runtime, preventing newline corruption on multi-line SSH
  private keys.

### Changed

- **CI runners** — both workflows use `runs-on: haiku` so jobs are picked
  up by whichever of Taurus or Asus is available.

## [1.7.2] - 2026-07-18

### Changed

- **GitHub mirror releases** — every tagged release is now published
  to both the Gitea instance and `github.com/microgeni/haiku-claude-cli`.
  Assets (`.hpkg`, source tarball, `SHA256SUMS`) are identical on both
  platforms.  Requires the `GH_RELEASE_TOKEN` secret in Gitea CI.
- **CI improvements** — parallel `make -j$(nproc)` in the build step;
  `SHA256SUMS` file attached to Gitea releases; workflow split into
  `ci.yml` and `release.yml` for clearer separation of concerns.

## [1.7.1] - 2026-07-17

### Fixed

- **Phantom `> ` echo after turn completion** — when a streaming
  turn finished while the libedit edit buffer was empty, the flush
  timer injected a synthetic `\r` via `bracketed_getc` to wake the
  main loop.  The main loop echoed `UserPrompt() + "" + "\n"` into
  `TurnOutputBuf` before checking for an empty line, producing a
  bare `> ` line in the chat-history scroll region after every turn
  that completed without the user typing.  Fixed by trimming and
  checking for an empty line *before* the echo in the `ReadMessage`
  path, so the echo is skipped and the early-`continue` fires
  directly.

- **Resize during active turn corrupts scroll-region output** — two
  compounding bugs when the user resizes the terminal while a turn
  is streaming:

  1. `apply_scroll_region()` used `std::cout`, which is redirected
     through `TurnOutputBuf` while a turn is active.  The DECSTBM
     escape was buffered and delivered up to 16 ms later inside a
     `FlushTurnOutput()` DECSC/CUP/…/DECRC wrapper.  During that
     window the worker's output still targeted the old scroll region,
     so it could land on status-bar rows or below the new scroll
     bottom.  Fixed by switching `apply_scroll_region()` to
     `DirectWrite` (same as `draw_fixed_frame`).

  2. `RedrawStatusBar()` did not update `g_turn_row2` /
     `g_turn_col` after a resize.  When a turn had already produced
     output (`g_turn_started == true`) the tracker held the old
     scroll-bottom row; subsequent `FlushTurnOutput()` calls CUP'd
     to the stale row — potentially inside the status-bar frame on
     the new (smaller) terminal.  Fixed by recalculating
     `chat_bottom` from the refreshed `g_cached_term_rows` in
     `RedrawStatusBar()` and writing it into `g_turn_row2`.

- **tmux permission-menu hang** — asking Claude to run a destructive
  shell command (e.g. `git commit && git push`) caused the prompt to
  disappear and the process to hang forever when running inside tmux.
  Three compounding bugs were responsible:

  1. **`EscInterruptGuard::pause()` race** — the guard thread polled
     the real-tty fd with a 100 ms timeout; `pause()` waited only
     120 ms for acknowledgement.  If `pause()` was called just as
     the poll started, the 120 ms window expired before the thread
     could acknowledge, leaving both the guard thread and
     `SelectOption()` reading from the same fd concurrently.
     `SelectOption()` would hang forever because the guard thread
     stole its keypress.  Fixed by adding a self-pipe
     (`fWakePipe`) to `EscInterruptGuard`; `pause()` now writes to
     the pipe to immediately kick the thread out of `poll()`,
     guaranteeing acknowledgement within one loop iteration (~1 ms).

  2. **DECSTBM scroll region swallowing the permission menu** —
     `SelectOption()` rendered with `\n`-based line output while the
     DECSTBM scroll region was still active.  At the scroll-region
     bottom, newlines scrolled content into the status-bar rows,
     making the menu invisible to the user.  Fixed by adding
     `tui::SuspendScrollRegion()` / `tui::RestoreScrollRegion()`
     that bracket every `SelectOption()` call: `\x1b[r` resets to
     full-screen before the menu, and the scroll region plus status
     bar are fully restored afterward.

  3. **`g_real_tty_fd` from `dup(stdin)` instead of `/dev/tty`** —
     in tmux the controlling terminal is the PTY slave, which _is_
     stdin at init time.  But `dup(stdin)` produces a second handle
     to the same PTY, and any later `dup2` on fd 0 (e.g. by
     `BlockStdin()`) could leave the real-tty fd in an inconsistent
     state on some Haiku libedit versions.  Fixed by opening
     `/dev/tty` directly in `repl::Init()`, with a `dup(stdin)`
     fallback for terminals without a controlling tty.

### Notes

- **`SelectOption` permission menu not reachable via `tmux send-keys`** —
  by design.  `BlockStdin()` redirects `STDIN_FILENO` to an empty pipe
  while the menu is active; `SelectOption` reads directly from
  `repl::RealTtyFd()` (the `/dev/tty` fd opened at init), so bytes
  injected via `tmux send-keys` land in the pipe and are discarded.
  The menu itself renders correctly and is visible in the pane.
  Workaround for fully-automated sessions: `/ludicrous` mode.

## [1.7.0] - 2026-04-27

### Added
- **True async REPL** — the `> ` prompt is live and accepts keystrokes
  while Claude's response streams above it. A `LocalWorker` background
  thread owns the API call; the main thread returns to `libedit`
  immediately after dispatch. `TurnOutputBuf` (a custom `std::streambuf`)
  intercepts all `std::cout` during a turn and a 16 ms flush-timer thread
  drains it with `DECSC`/`DECRC` so the cursor on the input row is
  preserved throughout.
- **Cancel-and-retype (Ctrl+X)** — pressing Ctrl+X during a streaming
  turn cancels the in-flight request and restores the submitted text into
  the `libedit` edit buffer so the user can amend and resubmit. The
  cancelled attempt is suppressed from `libedit` history (`RemoveLastRecord`).
- **BFS summary snapshot background refresh** — after each successful turn
  `config::RefreshSummarySnapshot(changed_paths)` updates only the
  `claude:summary` entries touched by `WriteAttr` tool calls in that turn,
  keeping the system-prompt snapshot current without a full filesystem
  rescan. `/compact` triggers a full `config::ReloadBfsSummaries()`.
  Snapshot updates are skipped when the file count exceeds 500 to stay
  O(changed) on large projects.
- **Self-pipe wake** — `repl::WakeReadMessage()` writes one byte to a
  non-blocking pipe polled alongside stdin in `raw_getc_or_wake()`, so
  `drain_turn()` fires as soon as the worker finishes without requiring a
  real keypress.
- **Exclusive terminal access for tool permission menus** — `BlockStdin`
  redirects `STDIN_FILENO` to an empty pipe so `libedit`'s internal
  `read(0,…)` blocks instead of consuming tty bytes; `SelectOption` reads
  directly from `RealTtyFd()`. `PauseFlushTimer`/`ResumeFlushTimer` stop
  the 16 ms flush thread for the duration of the menu so stdout is not
  contested.
- **CHANGELOG and README included in HPKG** — the `documentation`
  directory of the Haiku package now ships both files.

### Fixed
- **No-echo shell after tool use** — `EscInterruptGuard` saved
  `libedit`'s cbreak/noecho `termios` state and restored it on
  destruction, leaving the tty without `ECHO` after every turn.
  `repl::Init()` now snapshots the original `termios` before `libedit`
  touches it; `Deinit()` restores it unconditionally via `TCSAFLUSH`.
  `EscInterruptGuard` now operates on `repl::RealTtyFd()` (the `dup()`'d
  tty fd) rather than `STDIN_FILENO` so `BlockStdin()`'s pipe redirect
  can never confuse its save/restore.
- **Corrupt terminal after `/quit`** — `repl::Deinit()` now restores
  `STDIN_FILENO` to the real tty if `BlockStdin()` had redirected it,
  drains stale terminal sequences with `DrainStaleInput()`, and closes
  all pipe fds. `tui::TeardownStatusBar()` emits a trailing `\n` so the
  shell prompt starts on a fresh line. `StatusFrameGuard` calls
  `repl::Deinit()` before `tui::TeardownStatusBar()` to enforce the
  correct teardown order.
- **SIGINT leaves tty in raw mode outside the REPL** — the startup
  `SIGINT` handler (installed before `InteractiveLoop`) now calls
  `repl::Deinit()` and `tui::TeardownStatusBar()` before re-raising, so
  Ctrl+C during login/logout or one-shot mode always restores the tty.
  `api::InterruptGuard` continues to override `SIGINT` inside turns for
  graceful abort, and restores the teardown handler (not `SIG_DFL`) on
  destruction.
- **Prompt disappears after tool permission prompts** — removed the
  `DSR`/`CPR` round-trip (`ESC[6n`) from `SelectOption`; the cursor-row
  cap is now derived deterministically from `TerminalRows() - kStatusBarRows
  - menu_rows`. The `CPR` response had arrived in fragmented kernel batches,
  leaking bytes such as `1R` or `;1R` into `libedit`'s edit buffer and
  causing the input row to go blank or appear frozen.
- **Slash commands invisible while a turn is active** — `BeginTurn()`
  redirects `std::cout` through `TurnOutputBuf`; slash-command output and
  `SelectOption` menus rendered during that window were silently swallowed.
  Input received while `turn_active` now waits for `fDisplayCv` (the
  worker completes first), then `drain_turn()` restores stdout before the
  command is dispatched.
- **Double echo of user input during concurrent turn** — the redundant
  re-echo at line 719 of `session.cpp` (after `drain_turn()`) has been
  removed; `EndTurn()` already flushes the `TurnOutputBuf` content
  including the line-690 echo.
- **Cursor misalignment after permission menu (`DECSC`/`DECRC`)** —
  `ResumeFlushTimer()` now parks the physical cursor at the input row
  (N-2) instead of `chat_bottom` (N-4), so the `FlushTurnOutput`
  `DECSC` saves the correct row and `DECRC` restores `libedit`'s prompt
  to the right position.
- **`status bar` fixed-frame functions intercepted by `TurnOutputBuf`** —
  `draw_fixed_frame()`, `PositionCursorForInput()`, `ClearInputRow()`,
  `RepaintInputRow()`, `HideCursor()`, and `ShowCursor()` now write via
  `DirectWrite()` (bypassing the interceptor) so status-bar redraws are
  never captured into `g_turn_pending`.
- **`cppcheck` passedByValue warning** — `dispatch_turn`'s `line`
  parameter is now passed by `const std::string&`.

## [1.6.3] - 2026-07-15

### Fixed
- **Telegram-origin turns now appear in local scroll history** —
  `ProcessUpdate` had two gaps: (1) the assistant reply was never
  appended to `fUserMessages[user_id]`, so per-user remote context
  accumulated user messages only; (2) there was no write-back path
  from `ProcessUpdate` into the local REPL `messages[]`, so completed
  Telegram turns were invisible to scroll history and never saved to
  `history.json`. Fixed by adding `SetSharedHistoryAppender()` /
  `fSharedHistoryAppend` to `RemoteControl`; `ProcessUpdate` now
  appends the assistant message to the per-user silo and calls the
  appender to push both turns into the live local `messages[]`.
  `session.cpp` wires up the appender alongside `SetSharedHistory`;
  the lambda pushes both messages and calls `config::SaveHistory` so
  `--resume` sees the Telegram turns. `tui::RepaintInputRow()` is used
  by an RAII `CursorGuard` in `ProcessUpdate` to restore the `> `
  prompt after each remote turn.
- **Inline keyboard missing on `MirrorToPrimary` numbered responses** —
  when a locally-initiated turn produced a numbered-choice response,
  `MirrorToPrimary` sent the assistant text to Telegram as plain text
  with no inline keyboard, so the option buttons never appeared and the
  user could not tap to reply. Applied the same
  `ExtractNumberedOptions` → keyboard-building logic that
  `ProcessUpdate` already uses for Telegram-origin turns; the keyboard
  is now passed to both `EditMessageText` and `SendMessage`.

## [1.6.2] - 2026-04-23

### Fixed
- **Remote-control duplicate message header** — `TryHandleSlashImmediate`
  was printing `[remote who] text` unconditionally before checking whether
  the message was a plain prompt, causing `ProcessUpdate` to print it a
  second time. The print is now skipped for plain prompts so only slash
  commands fully handled in `TryHandleSlashImmediate` emit the header there.
- **Input prompt visible during Telegram turns** — libedit's `> ` prompt
  lingered on the input row for the entire duration of a remote-control
  turn. `ProcessUpdate` now calls `tui::ClearInputRow()` at the start to
  blank it, and a RAII `CursorGuard` calls the new `tui::RepaintInputRow()`
  on every exit path so the prompt is always restored once the turn
  completes — replacing the previous scattered `\x1b"8"` calls.

## [1.6.1] - 2026-04-23

### Fixed
- **TUI layout: 4-row fixed frame** — the status-bar frame is now
  `separator / input / separator / status` (`kStatusBarRows = 4`).
  `PositionCursorForInput()` moves to row N-2 (outside the scroll
  region) so libedit's Enter key no longer triggers a DECSTBM scroll;
  `PositionCursorForChat()` targets the scroll-region bottom (N-4) so
  spinner and streamed responses always land in chat history.
- **Spinner and `claude>` response appear above the input row** —
  after the user submits, the spinner and response now scroll into
  history above the fixed `>` prompt row rather than overwriting it.
- **Blank line between prompt and spinner removed** — the spurious
  `\n` before the API call that created a visible gap between the
  user's input line and the spinner has been removed.
- **Screen cleared on startup** — `InstallStatusBar()` now emits
  `\x1b[H\x1b[2J` before applying DECSTBM and drawing the fixed
  frame, preventing prior-session content bleeding into the viewport.
  `InstallStatusBar()` is also called before the welcome text so the
  welcome prints inside the clean scroll region.
- **Turn-1 layout and `claude>` label erasure in tmux** — two
  independent first-turn bugs fixed: `EmitChatRule()` unconditionally
  moves to the scroll-region bottom before printing so the rule always
  triggers a DECSTBM scroll; `ClaudePrompt()` is now emitted by
  `MarkdownRenderer::Emit()` via `SetResponsePrefix()` rather than
  before the spinner, preventing the spinner's `\r\x1b[2K` tick from
  wiping the label on the very first character.
- **Cache-hit % clamped to 100** — `/stats` bar reading no longer
  exceeds 100 % when cache-read tokens reported by the API exceed
  `in_tok`. Cost/savings lines now use `%.2f` instead of `%.4f`.

### Refactored
- **`[turn N]` line and in-chat rule removed from scroll history** —
  both were redundant with the fixed status bar and top separator;
  turn stats are still logged to the config log file and the status
  bar is updated every turn.

## [1.6.0] - 2026-07-14

### Added
- **`/stats` bar charts** — three █-block gauges now appear after the
  summary block, showing Cache hit rate, BFS-read savings rate, and Output
  token share as a filled/unfilled bar with a percentage label:
  ```
  Cache hits   [████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░]  42%
  BFS savings  [████████████████████████████████░░░░░░░░]  78%
  Output share [███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   8%
  ```
- **`/stats` cost/savings annotations** — each bar is followed by an
  explanation line that translates the token counts into dollar figures
  (cache savings at $2.70/M, BFS savings at $3.00/M, output cost at
  $15.00/M):
  ```
               66,500 tokens served from cache  →  saved $0.1796
               39,200 tokens avoided via BFS    →  saved $0.1176
               12,400 output tokens             →  cost  $0.1860
  ```
- **`/stats` turns-per-session sparkline** — a Unicode block sparkline
  (▁▂▃▄▅▆▇█) over the last 60 sessions, with lo/hi/last annotations.
  `RecordSession()` now pushes a new entry and `RecordTurn()` increments
  it; the array is capped at 60 and written atomically with `stats.json`.

## [1.5.6] - 2026-07-14

### Fixed
- **Process crash on non-UTF-8 content** — `nlohmann::json::dump()` defaults
  to `error_handler_t::strict`, which throws `json::type_error` (error.316)
  on any non-UTF-8 byte in a string value.  Tool output containing binary
  data, Latin-1 text, or truncated multi-byte sequences — and assistant
  responses forwarded through the Telegram bridge — both triggered this path,
  reaching `std::terminate` via an uncaught exception.  All `dump()` call
  sites that may carry user or tool data now use `error_handler_t::replace`;
  invalid bytes are silently substituted with U+FFFD so output remains valid
  UTF-8 and the process continues normally.  Sites fixed: `config::SaveHistory`,
  `telegram::Client::SendMessage` (both overloads), `telegram::Client::EditMessageText`,
  `api::ShortInputSummary`, `hooks::Run`, and the MCP stdio transport.

## [1.5.5] - 2026-04-22

### Fixed
- **Cost estimate accuracy** — the `/stats` estimated cost was inflated ~10×
  for heavy cache users because `in_tok` from the API already includes
  cache-read and cache-write tokens, but the old formula charged all of them
  at the full $3.00/M uncached rate. The formula now subtracts `c_read_tok`
  and `c_write_tok` from `in_tok` before applying the uncached rate, then
  prices each tier correctly: fresh input at $3.00/M, cache reads at $0.30/M,
  cache writes at $3.75/M, and output at $15.00/M.
- **Telegram slash commands unblocked** — non-Claude slash commands
  (`/mute`, `/unmute`, `/new`, `/help`, `/model`, etc.) sent via Telegram
  are now handled immediately in the `WorkLoop` via a new
  `TryHandleSlashImmediate()` method, without waiting for `AcquireTurn()`.
  Previously these commands were blocked until any in-progress Claude turn
  finished. Plain prompts and passthrough commands still go through
  `AcquireTurn` → `ProcessUpdate` to preserve the single-turn invariant.
  ANSI escape sequences are stripped from captured output before replying
  to Telegram.

## [1.5.4] - 2026-04-22

### Fixed
- **First prompt double-Enter** — the very first prompt in every interactive
  session required two Enter presses to submit; all subsequent prompts worked
  normally. Three cooperating bugs were responsible:
  1. `bracketed_getc` used a fixed 4-byte speculative read after `\e[` to
     detect the bracketed-paste opener `\e[200~`. Longer CSI sequences (e.g.
     cursor-position reports `\e[row;colR`) had their trailing bytes silently
     dropped, corrupting libedit's key-sequence FSM so it consumed the first
     Enter as part of a meta-key chord. Fixed by reading one byte at a time
     and stopping at a CSI final byte (0x40–0x7E per ECMA-48), with all
     accumulated bytes stashed and replayed verbatim on non-paste sequences.
  2. The bracketed-paste enable sequence `\e[?2004h` was sent twice — once
     via `fputs()` (stdio-buffered) and once via `::write()` — potentially
     eliciting two terminal responses. Removed the early `fputs()` send;
     kept only the single `::write()` call, now preceded by `fflush(stdout)`.
  3. Terminal responses to init sequences could arrive in stdin between
     `tui::InstallStatusBar()` and the first `readline()` call. Added
     `repl::DrainStaleInput()` which briefly sets stdin non-blocking, discards
     any queued bytes, then restores blocking mode — called once at the start
     of each interactive session.
- **Integer tool params crash** — `nlohmann::json::value<int>()` triggered a
  SIGSEGV (confirmed via syslog stack trace) when the model sent `start_line`,
  `end_line`, `max_bytes`, or `timeout_seconds` as a JSON string or null
  instead of a number. Added explicit `is_number()` guards before all
  `get<int>()` calls; falls back to the same defaults on type mismatch.

## [1.5.3] - 2026-04-21

### Fixed
- **Bracketed paste** — terminal paste events (`\e[200~`…`\e[201~`) are now
  fully intercepted in a custom `rl_getc_function` (`bracketed_getc`). The
  paste body is read in one shot, `\r\n`/`\n` converted to backslash-
  continuation sequences, and replayed char-by-char to libedit. Eliminates
  the `0~` artifact that appeared at the start of pasted text and correctly
  reassembles multi-line pastes via the existing backslash-continuation path.
  `repl::Deinit()` sends `\e[?2004l` on exit so the terminal is left clean.
- **Stats migration** — `load()` now runs `migrate_tool_entry()` on every
  `tool_calls` entry after parsing, renaming the legacy `fSavedbytes` key
  (written by an early buggy version) to `saved_bytes`. The `/stats` BFS
  savings block no longer shows "Cache empty" on existing installs.
- **Overloaded-error retry** — stream-level `overloaded_error` events now
  trigger the same exponential-backoff retry loop as HTTP 529, with clearer
  "Claude is overloaded, retrying…" messaging.

## [1.5.2] - 2026-04-20

### Added
- **Syntax highlighting in Write/Edit permission previews** — `tui::LangFromPath()`
  maps 20+ file extensions to language tags; `tui::HighlightCode()` applies
  the existing highlight engine to preview content. Keywords, strings,
  comments and numbers are coloured in the diff block.
- **`tui::DiffRemoved()` / `tui::DiffAdded()`** — new public helpers that
  render full-width diff rows with background colour matching Claude Code
  exactly: dark-red bg `rgb(61,1,0)` + muted-red marker + near-white content
  `rgb(248,248,242)` for removals; dark-green bg `rgb(2,40,0)` + green fg for
  additions. `\x1b[K` fills the background to the terminal right edge.
- **Behaviour system prompt** — three interaction-style guidelines injected
  into every session: intent narration before tool calls, post-tool
  natural-language summary, proactive next-step suggestions after code tasks.
- **Separator rule above prompt** — `tui::EmitChatRule()` now called at the
  top of the REPL loop so each prompt is preceded by a full-width `─` rule,
  matching Claude Code's two-rule frame around the input row.

### Changed
- **Permission dialog matches Claude Code style** — complete visual overhaul:
  - `╌` dashed rule (U+254C) at full terminal width instead of `─` solid.
  - Three-line header: tool label / filename / optional yellow warning line.
  - Full file context shown (surrounding unchanged lines) instead of changed
    lines only.
  - Repeated line numbers on removal/addition rows (`4 - / 4 +` style).
  - `❯` cursor glyph on active option; inactive options indented with spaces.
  - Natural-language question: *"Do you want to make this edit to foo.cpp?"*
  - Directory-scoped option 2: *"Yes, allow all Edit in src/ this session (shift+tab)"*.
  - `Esc to cancel · Tab to amend` footer row.
- **`SelectOption` render fix** — pre-reserves rows before drawing to prevent
  scroll displacement when the preview block fills most of the terminal height.
  Initial render uses `\n` (natural scroll); subsequent redraws use
  `\x1b[1B\r` (no scroll). `PositionCursorForChat()` no longer called before
  `SelectOption` — cursor stays where the preview ended.
- **Bash tool notice shows full command** — `ShortInputSummary()` returns the
  raw command string untruncated for Bash; other tools keep the 80-char `...`
  cap.
- **`...` removed from non-Bash truncation bracket** — restored after
  brief removal; `[tool: Write {"path":...}]` reads naturally.

## [1.5.1] - 2026-04-19

### Changed
- **Dead-code removal** — `tui::Gray()` removed from the public header
  (zero external callers; remains an internal helper in `tui.cpp`).
  Duplicate `ensure_parent_dir()` implementations in `repl.cpp` and
  `tools.cpp` deleted; both now call `paths::EnsureParentDir()`.
  Duplicate `display_width` lambda in `main.cpp::FormatStatusRow`
  removed; now calls the new `tui::DisplayWidth()` public function.
- **Naming convention fixes** (Haiku Coding Guidelines):
  - `load_history` → `LoadHistory` (PascalCase for named-scope function).
  - `Config::fAllowDestructivetools` → `fAllowDestructiveTools` (capital T).
    JSON key reads both spellings for backward compatibility with existing
    `config.json` files.
  - `LineIsPathDrop` → `line_is_path_drop` (snake\_case, consistent with
    sibling helpers `shell_single_quote` / `shell_tokenize`).
  - `notify.cpp`: local variable `fPrevspace` → `prevSpace` (f-prefix is
    for member variables only).
  - `mcp.cpp`: `find_tool` / `find_by_tool_name` parameter `namespaced` →
    `qualifiedName`.
  - `already_recorded` → `recordedBySlashCmd` in `InteractiveLoop`.
- **JSON field renames** — persisted keys `"fSavedat"` (history.json) and
  `"fSavedbytes"` (stats.json) renamed to `"saved_at"` and `"saved_bytes"`.
  Both files include backward-compatible fallback reads so existing data
  is not silently discarded on upgrade. Local variables `fSavedtokens` /
  `fSavedbytes` in `stats.cpp` / `main.cpp` renamed to `savedTokens` /
  `savedBytes`.
- **Nested anonymous namespace flattened** — a redundant inner
  `namespace { }` wrapping `g_bfs_loaded`, `g_bfs_snapshot`,
  `IsValidUtf8`, and `SanitizeUtf8` in `main.cpp` was removed; all four
  are now in the single outer anonymous namespace.
- **`path_inside_cwd`** in `tools.cpp` now uses `std::string` comparison
  instead of mixing `getcwd` output with `std::strlen`.
- **LTO false-positive silenced** — `line_is_path_drop` annotated with
  `__attribute__((noinline))` to prevent GCC 13's LTO alias-analysis from
  emitting a spurious `-Wfree-nonheap-object` warning during release builds.
- **`/open` URL list** uses `std::to_string` instead of `snprintf` with a
  `%zu` format, eliminating a `-Wformat-truncation` warning.

### Added
- **`tui::DisplayWidth()`** — promoted from an anonymous-namespace helper
  to a public function in `tui.h`. Counts visible terminal columns in a
  string, skipping ANSI SGR escapes and handling UTF-8 multi-byte
  sequences correctly. Eliminates a duplicate lambda that had been copied
  into `main.cpp::FormatStatusRow`.
- **`paths::EnsureParentDir(filePath)`** — new helper in `paths.h/cpp`
  that creates all parent directories required for a file path to be
  written. Wraps `paths::MkdirP` and replaces two identical local copies
  that existed independently in `repl.cpp` and `tools.cpp`.

### Fixed
- **`IsSshSession` missing comment** — added a doc comment explaining
  that it is used to suppress the bracketed-paste multi-line hint on SSH,
  where Ctrl+J / Alt+Enter may not reach the application.
- **`fetch_models` missing rationale** — added a comment explaining why it
  uses a private short-lived `CURL*` handle rather than the session handle
  from `get_curl()`.
- **`IsValidUtf8` / `SanitizeUtf8` relationship undocumented** — comment
  now cross-references both functions so it is clear that `IsValidUtf8` is
  the line-level pre-flight guard in `PreloadBfsSummaries` and
  `SanitizeUtf8` is the recovery path used at API request time.

## [1.4.9] - 2026-06-17

### Added
- **`make lint`** — cppcheck static analysis (warning, performance,
  portability categories); exits non-zero on any finding so it can gate CI.
- **`make security`** — flawfinder CWE security audit at level 3+; zero
  hits on clean tree.
- **`make security-full`** — flawfinder full scan at level 2+.
- **`make check`** — runs lint + security in sequence; intended as the
  pre-release gate.
- **Bracketed paste mode** — `repl::Init()` sends `\e[?2004h` on TTY
  startup. Pasted multi-line text is now received atomically via the
  `\e[200~…\e[201~` markers, transformed into backslash-continuation
  lines, and fed to `ReadMessage()`'s existing loop. `repl::Deinit()`
  restores the terminal on exit and signal teardown.
- **`select_option` heading** — the menu now accepts an optional heading
  string. On selection the entire block (heading + all option rows) is
  erased and replaced with a single compact summary line
  (`allow bash? → Yes, allow once`) so scroll history stays informative
  without the full menu lingering. Applied to all four call sites:
  permission prompt, `/model`, `/memory`, and `/compact`.

### Changed
- **Haiku Coding Guidelines conformance** — full codebase refactor:
  - Indentation converted from 4-space to tabs across all 23 source files
    (~7,400 lines).
  - ~95 public namespace functions and class methods renamed from
    `snake_case` to `PascalCase` (`send_conversation` →
    `SendConversation`, `terminal_width` → `TerminalWidth`, etc.).
    Internal anonymous-namespace helpers retain `snake_case` per the
    documented exception.
  - Member variables renamed from trailing-underscore to `f`-prefix style
    (`label_` → `fLabel`, `thread_` → `fThread`, etc.) across all five
    classes: `MarkdownRenderer`, `Spinner`, `EscInterruptGuard`,
    `RemoteControl`, `telegram::Client`.

### Fixed
- **TOCTOU file permission race (CWE-362)** — `SaveHistory()` and
  `SaveTokens()` called `chmod()` after closing an `ofstream`, leaving
  a window where credential files were world-readable. Fixed by opening
  with `open(O_WRONLY|O_CREAT|O_TRUNC, 0600)` + `fdopen()` so the
  correct mode is set atomically at creation time.
- **`snprintf` buffer too small** — the `/open` URL list used a 16-byte
  buffer for `"  %zu. "` which can require up to 25 bytes on 64-bit;
  widened to 32 bytes, eliminating the `-Wformat-truncation` warning.
- **`uselessCallsSubstr` performance** — seven sites of
  `x = x.substr(0, n) [+ suffix]` replaced with `x.resize(n); x += suffix`
  to avoid an unnecessary heap allocation and copy.


### Fixed
- **`terminate()` crash on invalid UTF-8 in system prompt** — CLAUDE.md
  files and the BFS snapshot are read as raw bytes; a non-UTF-8 byte
  (e.g. a Latin-1 em-dash `0x97`) anywhere in the composed system prompt
  caused `nlohmann::json::dump()` to throw `type_error.316`, which
  propagated uncaught to `terminate()`. Fixed by running
  `sanitize_utf8()` over the system prompt before building the JSON
  request body (consistent with how tool results are already handled),
  and by wrapping `body.dump()` in a `try/catch` so any missed bad byte
  produces a clean error message instead of a crash.

## [1.4.7] - 2026-04-19

### Fixed
- **`stats.json` silently wiped on crash or corrupt file** — three
  co-operating bugs could reset all lifetime stats to zero:
  (1) `stats.json` was opened with `std::ofstream` which truncates the
  file to zero immediately; a crash or kill mid-write left an empty file
  that loaded as `fresh()`, which was then saved over the real data.
  Fixed by writing to `stats.json.tmp` first and promoting it with
  `rename(2)`, which is atomic on POSIX — the previous file survives any
  mid-write failure.
  (2) A JSON parse error (corrupt file, half-written file) silently
  returned a zeroed `fresh()` blob which was immediately saved over the
  real data. Fixed by falling back to `stats.json.bak` (the previous
  successful save) before resorting to `fresh()`.
  (3) Input/output token counters were stored as 32-bit JSON integers;
  overflow at ~2 billion tokens would produce a negative value, trigger
  the parse-error path above, and wipe all history. Fixed by using
  `long long` (`int64`) throughout.

## [1.4.6] - 2026-04-19

### Fixed
- **ESC cancel race — subsequent turns fired `[interrupted]` immediately** —
  Two cooperating bugs caused every turn after an ESC-cancel to exit with
  `[interrupted]` before ever hitting the API. (1) `EscInterruptGuard` was
  constructed before `g_interrupted` was cleared, so a stale ESC byte
  lingering in the tty buffer could re-set the flag after the clear.
  Fix: call `tcflush(TCIFLUSH)` and reset `g_interrupted = 0` *before*
  constructing the guard. (2) `send_with_tools` called `send_conversation`
  which spawned a second `EscInterruptGuard` thread concurrently reading
  the same stdin fd, racing with the outer guard. Fix: skip the inner guard
  entirely when `g_active_esc_guard` is already set.
- **Arrow keys in permission menu consumed by ESC guard** — the
  `EscInterruptGuard` background thread held stdin in raw mode and raced
  with `tui::select_option()`. Up/Down CSI sequences were swallowed by the
  guard, leaving the menu unresponsive. Fix: added `pause()`/`resume()` to
  `EscInterruptGuard`; `prompt_permission()` now pauses the guard for the
  duration of `select_option()` so it has exclusive stdin ownership.
- **`select_option` arrow keys didn't update the highlight** — `VMIN=0
  VTIME=1` is a polling read; `read()` could return 0 immediately even with
  CSI bytes already in the kernel buffer. Switched to `VMIN=1 VTIME=1` so
  each read blocks until a byte arrives or the 100 ms timeout expires. Bare
  ESC still times out correctly; arrow keys now reliably redraw.
- **`select_option` option text garbled on every arrow keypress** —
  `render()` used newline to step between option lines; starting at the
  bottom of the DECSTBM scroll region each newline scrolled the region up,
  shifting the menu's absolute position so subsequent erase+reprint landed
  on wrong rows. Replaced with cursor-down + CR. Also added a leading CR in
  `render()` to reset the cursor column to 0 before every draw.
- **Continuation prompts appeared above the initial prompt** —
  `position_cursor_for_chat()` was called before each continuation
  `read_line()`, parking the cursor above the fixed input row. Fix: call
  `position_cursor_for_input()` instead so every readline call draws in the
  same fixed input row.
- **Continuation `read_line()` overwrote the fixed status frame** — on
  multiline entry, libedit's newline after the first accepted line moved the
  cursor outside the scroll region. Subsequent continuation calls drew
  there, clobbering the rule and status rows. Fix: call
  `tui::position_cursor_for_chat()` before every inner `read_line()` in
  `read_message()`.
- **Cursor hidden at `claude>` prompt after multi-tool turns** — the last
  hide-cursor escape from a tool spinner could leave the cursor hidden for
  the entire input wait. Fix: embed show-cursor directly in
  `claude_prompt()` so visibility is unconditionally restored at every
  prompt callsite.

## [1.4.5] - 2026-04-19

### Added
- **`/ludicrous` mode** — session-scoped toggle that auto-approves all
  destructive tool permissions with no prompts. Type `/ludicrous` to
  engage (prints `⚡ LUDICROUS MODE ENGAGED`), type it again to
  disengage. Status bar shows a yellow `⚡ LUDICROUS` label while
  active. Designed for hands-free runs driven from `CLAUDE.md` or
  `ROADMAP.md` context. Available in both the REPL and the Telegram
  bridge local prompt; tab-completes like all slash commands.

### Changed
- **Arrow-key permission prompts** — destructive tool permission
  requests now render as a vertical numbered list with arrow-key
  navigation instead of a `(y)es/(a)lways/(n)o` getline prompt.
  Up/Down arrows move the highlight; number keys `1`/`2`/`3` jump
  directly and confirm; Enter confirms the current selection; Esc
  denies. Falls back to a plain numbered prompt on non-TTY stdout.
- **Telegram permission buttons** — when a destructive tool fires
  during a Telegram turn the bridge sends an inline-keyboard message
  with three buttons (`1. Yes, allow once` / `2. Always allow this
  session` / `3. No, deny`) and blocks until the user taps one,
  instead of silently denying. The local terminal shows
  `[awaiting Telegram response]` and echoes the answer when it
  arrives.
- **`/model` interactive picker** — bare `/model` now fetches the
  model list from the API and presents it as an arrow-key menu with
  the currently active model pre-selected at the top. `/model <name>`
  still sets directly without a menu.
- **`/memory` interactive picker** — bare `/memory` now shows a
  two-option menu (`Project (./CLAUDE.md)` / `User (~/.../CLAUDE.md)`)
  instead of always defaulting to the project file. `/memory user`
  still goes straight to the user file.
- **`/compact` confirmation prompt** — `/compact` now shows a
  `Yes, summarize / No, keep history` menu before firing the
  summarisation call, preventing accidental history wipes.
- **Input row clears on Enter** — the text typed at the fixed-bottom
  input row is now erased immediately when Enter is pressed, so it
  does not linger for the duration of the turn. The `you> message`
  replay into scroll history is unchanged.

## [1.4.4] - 2026-04-18

### Fixed
- **Grep/exec_capture hang on large trees** — `run_grep` and
  `exec_capture` (used by the `Grep`, `Query`, `ReadAttr`, and
  `WriteAttr` tools) previously used a raw blocking `read()` loop
  with no cancellation path. A slow `grep -r` over a large directory
  tree would hang indefinitely even after Esc or Ctrl+C. Both now use
  a `poll(100 ms)` tick loop that checks `g_interrupted` on every
  tick, with a `setsid()` + `SIGTERM`/`SIGKILL` kill-group on
  interrupt — matching the pattern already used by `run_bash`.
- **`g_interrupted` linkage UB** — the flag was defined inside an
  anonymous namespace in `main.cpp`, giving it translation-unit-local
  linkage. The `extern` declaration in `tools.cpp` was therefore
  undefined behaviour (and a potential linker error on stricter
  toolchains). Moved to file scope before the anonymous namespace so
  it has true external linkage.
- **Interruptible retry sleep** — the exponential-backoff retry delay
  in `send_conversation` previously called `sleep_for` for the full
  delay duration, ignoring Esc/Ctrl+C until it expired. Replaced with
  a 100 ms tick loop so cancellation is noticed promptly.

## [1.4.3] - 2026-04-18

### Fixed
- **Spinner glyph pixel-bleed clipping** — the star glyphs
  (U+2736–U+273D, `✶✷✸✹✺✻✼✽`) rendered with pixel overhang in
  Haiku Terminal's default font. The glyph's pixels bled into the
  adjacent cell, which was then painted over by the space character
  printed after the glyph, visually clipping the right side of the
  star on every frame. Replaced with braille rotation glyphs
  (`⣾⣽⣻⢿⡿⠿⢯⣷`, U+28xx block) which are designed for terminal use
  and sit cleanly within their cell boundary.
- **Spinner truncation CSI parser** — the escape-sequence skip loop
  previously only exited `in_esc` on `'m'`; broadened to the full
  standard CSI final-byte range `0x40–0x7E` so any future sequence
  ending on a non-`m` byte is handled correctly.
- **Spinner truncation off-by-one** — `budget = width - 1` caused
  the frame to be cut one column too early (a frame exactly `width`
  columns wide had its last character replaced by `…`). The walker
  now allows content up to `width` columns and only truncates when
  content would exceed `width`, backing up to `width - 1` visible
  columns before appending `…`.
- **Spinner ellipsis colour bleed** — when truncation fired and
  `frame.resize(cut)` sliced mid-escape, the appended `…` was
  rendered in whatever rainbow colour was active at the cut point.
  A `\x1b[0m` reset is now inserted before `…`.

## [1.4.2] - 2026-04-17

### Fixed
- **Spinner truncation cuts glyph short** — the old tail-trim
  loop popped raw bytes one at a time and decremented the
  display-column counter for every byte, including zero-width
  ANSI escape bytes. This caused over-trimming so the animated
  spinner frame was visibly cut one or two columns early on
  narrow terminals. Replaced with a single forward pass that
  skips escape sequences entirely, handles multi-byte UTF-8
  sequences as one column each, records the exact byte offset
  where the budget is exceeded, then slices and appends `…` in
  one step — no more clipping.

## [1.4.1] - 2026-04-17

### Fixed
- **Crash on binary tool output** — `nlohmann::json` throws
  `type_error.316` when serializing strings that contain
  non-UTF-8 bytes (e.g. `cat`-ing a kernel driver blob or
  reading a `/dev` node via the Bash tool). Added
  `sanitize_utf8()` which replaces every invalid byte /
  truncated sequence with the Unicode replacement character
  U+FFFD before the tool result is inserted into the JSON
  message array. Previously this caused an unhandled C++
  exception and an `Abort` with no recovery path.

## [1.4.0] - 2026-04-17

### Added
- **Inline multi-line input** — compose multi-line prompts
  directly in the readline buffer without the `"""` fence
  syntax. Three ways to drop a newline:
  - `\` + Enter — portable, works over any SSH/tmux/mosh
    combination (routed through the existing
    backslash-continuation path).
  - `Ctrl+J` — local terminals.
  - `Alt+Enter` — local terminals (bound via
    `emacs_meta_keymap`).
  The startup banner detects SSH sessions
  (`SSH_CLIENT`/`SSH_TTY`/`SSH_CONNECTION`) and advertises
  only the portable form when remote, since some SSH
  clients and multiplexers swallow `Ctrl+J`/`Alt+Enter`
  before they reach libedit. `/help` lists all three.
- **Telegram streaming with live thinking indicator** — the
  Telegram bridge now shows immediate feedback and edits
  the placeholder message in-place as tokens arrive, so it
  visually looks like Claude is typing live in the chat:
  - Before the first token: animated `⏳ thinking…` with
    cycling dots (0–3) every second.
  - While streaming: accumulated text with a `▌` block
    cursor appended, updated every second via
    `editMessageText`.
  - On completion: final edit strips the cursor and renders
    inline-keyboard buttons for numbered choices.
  Applies to both local-origin turns mirrored to Telegram
  and Telegram-origin messages.
- **Auto-bootstrap of `claude:summary` BFS index** — on
  Haiku, the CLI forks `mkindex -t string claude:summary`
  at startup so the attribute index always exists on the
  current volume. Idempotent, silent, and a no-op on
  non-BFS volumes or if `mkindex` is absent. Query lookups
  over `claude:summary` now run O(1) on fresh installs
  with zero manual setup.

### Fixed
- **Telegram: final response silently dropped on edit
  failure** — `edit_message_text` used to return `true`
  unconditionally, so a rate-limited `editMessageText` call
  swallowed the response and the user saw `...` forever.
  It now returns `false` on real failures (treating
  "message is not modified" as success), and both
  `process_turn` and `process_telegram` fall back to a
  fresh `sendMessage` if the placeholder edit is rejected.
- **Telegram: streaming invisible for short responses** —
  the updater thread slept 1 s *before* its first poll, so
  replies that completed in under a second never updated
  the placeholder. First check now runs immediately, then
  polls every 500 ms.
- **Telegram: 401s after ~8 h in long bridge sessions** —
  `run_telegram_bridge` resolved the OAuth token once at
  startup and never refreshed it. Token is now re-resolved
  before each `send_with_tools` call, matching the
  interactive REPL pattern. Expired-and-unrefreshable
  sessions now surface a clear error in the chat instead
  of silently dropping the response.
- **Ctrl+J / Alt+Enter now actually insert a newline** —
  `rl_insert_text("\n")` submits the line under libedit's
  readline compat layer instead of inserting a literal
  newline. Replaced with a `soft_newline` handler that
  appends `\\` to the buffer and calls `rl_newline()`, so
  `read_message()`'s existing backslash-continuation path
  handles the multi-line assembly. Also: `rl_bind_key('\n',
  ...)` is unreliable under libedit because `0x0A` is
  hardwired as `accept-line` before the compat shim can
  intercept; switched to `rl_set_key("\x0a", ...,
  rl_get_keymap())`.
- **Spinner cursor restore hardened with RAII** — if
  `Spinner::run()` threw between `hide_cursor()` and
  `show_cursor()` (e.g. `std::bad_alloc` from string
  concatenation, or `system_error` from `cv_.wait_for`),
  the worker thread terminated via `std::terminate` without
  restoring the cursor, leaving the user's terminal with
  a hidden cursor until reset. `std::atexit` does not fire
  on `std::terminate`, so the existing teardown-safety net
  did not cover this path. `hide_cursor()` is now wrapped
  in a `ShowGuard` whose destructor emits line-clear +
  `show_cursor()` on every exit path, including unwinding
  exceptions.
- **Cursor flicker around curl calls** — removed the
  redundant `tui::hide_cursor()` / `tui::show_cursor()`
  pair bracketing `curl_easy_perform` in
  `send_conversation`. The Spinner now owns cursor
  visibility for its full lifetime, so the extra calls
  could cause a brief flicker if curl returned before the
  spinner stopped.

## [1.3.0] - 2026-04-17

### Added
- **BFS default-on** — the four BFS attribute tools (Query,
  ReadAttr, WriteAttr, IndexAttr) are now surfaced
  prominently in the system prompt with usage guidance, and
  the CLI auto-seeds a `claude:summary` attribute the first
  time Claude reads a source file. Later sessions read the
  summary via ReadAttr (~10–30 tokens) instead of the full
  file (~thousands), persisting understanding across
  sessions for free. A BFS-summary snapshot of the project
  is also preloaded into the system prompt so Claude knows
  what's already indexed before the first question.
- **Prompt caching** — system prompt, tool definitions, and
  the last user turn are now marked with
  `cache_control: ephemeral`. Subsequent turns in the same
  session hit the cache and process input ~5–10× faster at
  ~10% of the normal input-token cost.
- **Auto /compact** — when the conversation approaches ~80%
  of the model's context window, a /compact runs
  automatically to summarize the history and reclaim
  headroom. The auto-fire also upgrades existing
  `claude:summary` attributes in the same pass.
- **Lifetime stats** — `/stats` command shows sessions,
  turns, tokens, estimated cost, and a BFS-advantage block
  that computes real bytes saved from per-call `stat()` of
  files read via ReadAttr vs. what a full Read would have
  cost. Persisted in `<config_dir>/stats.json`.
- **Desktop notifications** (Haiku only) — `/notify on|off`
  fires a Haiku notification bubble after long turns
  complete, with the project's HAL icon and playful
  past-tense titles ("Pondering complete (24s)",
  "Cogitation concluded (18s)", …) that pair with the
  spinner's gerund verbs. Child process detaches from the
  parent session so the notification_server dispatch works
  through a running REPL.
- **/open URL launcher** — after a turn, any http(s) URLs
  in the reply are collected and `/open` launches the
  `open` command on them (Haiku's native launcher, also
  works on macOS).
- **/model with no args** lists all available models from
  the Anthropic `/v1/models` endpoint so you can pick a
  model without leaving the REPL.
- **Build modes** — `make release` now produces an
  optimized binary (`-O3 -flto -Wl,-s`) in `build-release/`,
  27% smaller than the dev build. Dev (`make`) is unchanged.
- **Persistent understanding workflow** — `WriteAttr` is
  auto-approved for the `claude:*` namespace so Claude can
  record `claude:summary` / `claude:component` /
  `claude:reviewed` without a permission prompt, but system
  attributes (BEOS:*, MAIL:*, Audio:*, …) remain
  permission-gated.

### Changed
- **Spinner** now renders during tool execution too (not
  just API streaming), so the UI never goes silent between
  "calling tool" and "tool result received".
- **TUI palette** consolidated — rule lines, meta text,
  status bar, and the dim channel all use 256-color
  gray-244 instead of a mix of CSI 2;90m and 256-color,
  for consistent brightness across terminals.
- **Spinner glyph + verb** are now rainbow-colorized per
  frame for a subtle personality nod.
- **main.cpp refactor** — pulled `paths`, `stats`, and
  `notify` into their own modules. Clean parallel build
  drops from 17.0s to 13.4s (-21%); incremental builds of
  the extracted modules rebuild in under a second. No
  behavior change.

### Fixed
- **Notification dispatch** — `notify` child now detaches
  from the parent session and redirects stdio to /dev/null
  before execvp so notification_server actually delivers
  the BMessage. Also drops the `--` sentinel, which Haiku's
  `notify` doesn't accept.
- **/model and /clear** now redraw the status bar so the
  model name / turn counters stay visible after either
  command.
- **BFS preload** now skips non-UTF-8 attribute lines so a
  stray binary summary can't break the system-prompt
  snapshot.
- **Config drift** — `PKG_VERSION` in Makefile now tracks
  `kVersion` in source (both read `1.3.0` here).

### Build
- `make release` target (see Added) for optimized release
  builds.
- `build-release/` added to `.gitignore`.
- `CLAUDE.md` documents both modes.

## [1.2.0] - 2026-04-16

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
