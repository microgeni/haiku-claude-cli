# Permission-Menu Smoke Test (Manual)

The interactive tool-permission menu (`tui::SelectOption`) reads from
`/dev/tty` directly, *not* from `STDIN_FILENO`.  This is a deliberate
security guard against silent auto-approval by automation tools — see
the SECURITY/AUTOMATION NOTE at the top of `SelectOption()` in
`src/tui.cpp`.

The practical consequence: **`tmux send-keys` cannot drive the menu**,
so the menu is not covered by `tests/tmux_smoke_test.sh`.  This file
is the manual walkthrough you run from a real terminal on Taurus
before tagging a release, especially when changes touch any of:

- `src/api.cpp` `EscInterruptGuard` or `PromptPermission`
- `src/tui.cpp` `SelectOption`, `PauseFlushTimer`, `SuspendScrollRegion`
- `src/repl.cpp` `BlockStdin` / `UnblockStdin` / `RealTtyFd`

These are the surfaces involved in commit `4c8cafa` (the tmux hang fix).


## Setup

```sh
cd /Data/Code/Projects/haiku-claude-cli
make                 # ensure build/claude is current
```

Open **two** Terminal windows so you can attach to one inside tmux for
the tmux-specific scenarios.


## Scenario list

For each scenario the *expected* behaviour is in **bold**.  Mark each
with ✅ / ❌ on a scratchpad as you go.  All scenarios start fresh:
exit and relaunch `./build/claude` between them.

### A — bare terminal (no tmux), `Bash` accept

1. Run `./build/claude`.
2. At the prompt, type: `Run this bash command: echo hello-A`
3. **Permission menu appears** with three options, "Yes" highlighted.
4. Press `Enter`.
5. **Tool runs, output streams, status bar advances `turn 0` → `turn 1`.**

### B — bare terminal, `Write` accept

1. `./build/claude`
2. `Use the Write tool to create /tmp/menu-smoke-B.txt with body "scenario B"`
3. **Diff preview shown above the menu.**
4. **Menu appears**, press `Enter`.
5. **File is written**: in another shell, `cat /tmp/menu-smoke-B.txt` prints `scenario B`.

### C — bare terminal, "Yes, allow all this session"

1. `./build/claude`
2. `Run this bash: echo first-C`
3. Menu appears.  Press `↓` once to highlight "Yes, allow all Bash this session", press `Enter`.
4. **First command runs.**
5. Now type: `Run this bash: echo second-C`
6. **No menu appears** — second invocation auto-approves silently.
7. **Both echoes are visible in the chat history.**

### D — bare terminal, deny

1. `./build/claude`
2. `Run this bash: echo should-not-run`
3. Menu appears.  Press `↓` `↓` to highlight "No", press `Enter`.
4. **Tool is denied**, model produces a final response, REPL returns to a fresh prompt.
5. **No `should-not-run` appears anywhere in the chat history.**

### E — bare terminal, ESC during streaming

1. `./build/claude`
2. Type: `Write a long essay about the Haiku BFS filesystem (multiple paragraphs).`
3. While the response is streaming, press `Esc` once.
4. **Streaming halts cleanly**, REPL returns to a fresh prompt with no garbled output, no leftover spinner, and the status bar repaints.

### F — **inside tmux**, `Bash` accept (the original bug)

This is the scenario that commit `4c8cafa` fixed.

1. From a fresh terminal: `tmux new -s menu-test`
2. Inside tmux: `cd /Data/Code/Projects/haiku-claude-cli && ./build/claude`
3. `Run this bash: echo hello-F`
4. **Menu renders fully visible** (not hidden under the status bar — the DECSTBM scroll region must have been suspended for the menu).
5. Press `Enter`.
6. **Within ~2 s the tool runs, output streams, turn completes.** No hang.
7. `/exit`, `tmux kill-session -t menu-test`.

### G — inside tmux, status bar redraws after menu

1. `tmux new -s menu-test`
2. `./build/claude`
3. Trigger any permission-gated tool, accept the menu, let the turn complete.
4. **The status bar at the bottom is intact** — divider line, model name, turn counter, hint text — no leftover menu rows or torn-up dividers.
5. Trigger a second permission-gated tool.
6. **Menu still renders correctly the second time** (no cumulative drift).

### H — inside tmux, ESC during streaming

1. `tmux new -s menu-test`
2. `./build/claude`
3. Trigger a long streaming response, press `Esc`.
4. **Same clean behaviour as scenario E**, plus: status bar still intact.

### I — `/ludicrous` mode skips the menu

1. `./build/claude` (tmux or bare terminal, either is fine).
2. Type `/ludicrous`, press `Enter`.
3. **`⚡ LUDICROUS MODE ENGAGED` banner appears**, status bar shows the LUDICROUS marker.
4. Type: `Run this bash: echo ludi`
5. **No menu**, "auto-approved Bash" line shown, command runs.


## Acceptance

A release is good to tag when **A through I all pass** on Taurus.
`tests/tmux_smoke_test.sh` covers the non-menu plumbing automatically;
this file covers everything that interacts with `SelectOption()`.

If anything in F / G fails, that is a regression of commit `4c8cafa`
and must block release.
