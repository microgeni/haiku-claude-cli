# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

A native Claude CLI for Haiku OS. Single C++17 binary; runtime
dependencies are libcurl, OpenSSL, libedit, and nlohmann/json.
Targets Haiku x86_64 (primary) and macOS (dev/prototype only).

Source lives in `src/`. One translation unit per module; each module
is a `.cpp` + `.h` pair under a lowercase namespace matching the
filename (`tui::`, `repl::`, `tools::`, etc.). `main.cpp` is the
entry point and contains the REPL loop, tool dispatch, and Claude
API integration.


## Build & Test

```bash
# ── On Taurus (Haiku — canonical target) ──
make                     # dev build  → build/claude        (-O2)
make release             # opt build  → build-release/claude (-O3 -flto -s)
make test                # run tests
make install             # install to /boot/system/non-packaged
make package             # build HPKG (requires Haiku `package` tool)

# Static analysis and security audit
make lint                # cppcheck  — warning/performance/portability
make security            # flawfinder — CWE security audit (level 3+)
make security-full       # flawfinder — full scan (level 2+)
make check               # lint + security in sequence (CI / pre-release)

# ── macOS / nix (prototype only) ──
nix develop              # enter dev shell (curl, openssl, libedit, nlohmann_json)
nix develop -c make      # build
nix develop -c make test # test
```

Build artifacts go to `build/` (dev) or `build-release/` (release).
The Makefile uses `pkg-config` for all dependency flags and works
with both gcc (Haiku) and clang (macOS/nix) without modification.


## Source Layout

```
src/
  main.cpp       — entry point, REPL loop, Claude API, tool dispatch
  tui.cpp/h      — terminal UI: color, spinner, markdown renderer,
                   status bar (DECSTBM), select_option menu
  repl.cpp/h     — libedit wrapper: line editing, history, tab
                   completion, bracketed paste, multi-line input
  tools.cpp/h    — tool registry, permission checks, previews
  commands.cpp/h — slash-command dispatch (/help, /model, /compact…)
  hooks.cpp/h    — lifecycle hooks (SessionStart, PreToolUse, etc.)
  mcp.cpp/h      — MCP stdio-transport client
  oauth.cpp/h    — OAuth 2.0 + PKCE flow
  paths.cpp/h    — platform config/data paths
  stats.cpp/h    — per-turn token accounting
  notify.cpp/h   — Haiku BNotification wrapper
  telegram.cpp/h — Telegram bridge (optional)
tests/           — shell-based functional tests
ci_scripts/      — test.sh and release helpers
docs/            — GIT_WORKFLOW.md, ROADMAP.md, claude.1 man page
assets/          — claude-icon.hvif (Haiku vector icon)
```


## Code Style

This project follows the **Haiku Coding Guidelines**:
https://www.haiku-os.org/development/coding-guidelines

Key rules in practice:

- **Indentation**: tabs, not spaces.
- **Braces**: opening brace on the same line for control flow; own
  line for function/class/namespace bodies (Haiku style).
- **Naming**:
  - Types and classes: `PascalCase` (`MarkdownRenderer`, `Spinner`)
  - Functions and methods: `PascalCase` (`SendRequest()`, `RenderLine()`)  
    *Exception*: free functions inside anonymous namespaces that are
    purely internal helpers may use `snake_case` to match the C
    stdlib idiom already present in the codebase (`wrap_for_readline`,
    `display_width`, etc.). New public API should use `PascalCase`.
  - Variables and parameters: `camelCase` (`lineBuffer`, `chosenIdx`)
  - Member variables: `f` prefix (`fWindow`, `fLabel`) — apply to
    new classes; existing plain members in structs are grandfathered.
  - Constants: `kPascalCase` (`kStatusBarRows`, `kEndLen`)
  - Namespaces: `lowercase` matching the filename (`tui`, `repl`)
  - Macros: `ALL_CAPS` (avoid where possible; prefer `constexpr`)
- **Comments**: full sentences, period at end. Public functions and
  non-trivial logic get a comment. Prefer a block comment above the
  function over inline commentary scattered through the body.
- **`extern "C"`** blocks for libedit callbacks (already established).
- **`nullptr`** over `NULL`; `static_cast<>` over C casts.
- **RAII** for all resources; no naked `new`/`delete` in new code.
- **`#pragma once`** is fine; the codebase uses `#ifndef` guards —
  stay consistent with the existing file when editing it.

Use **conventional commits** for commit messages:
`feat:`, `fix:`, `refactor:`, `docs:`, `build:`, `test:`, `chore:`


## Git Workflow

- **`dev`** — active development; all work lands here.
- **`main`** — stable releases only; never commit directly.
- Tags: `v{MAJOR}.{MINOR}.{PATCH}` semantic versioning.

### Release checklist (in order)
1. `bash ci_scripts/test.sh` — all tests must pass
2. **Version sync** — confirm `PKG_VERSION` in `Makefile` and `kVersion` in
   `src/config.cpp` show the same `x.y.z` string; update whichever is behind.
   The HPKG metadata and the running binary must agree or users see the wrong
   version (e.g. `/version`, `--version`, `claude -v`).
3. Update `CHANGELOG.md` — add `## [x.y.z] - YYYY-MM-DD` section
4. `git add CHANGELOG.md && git commit -m "docs: changelog for vx.y.z"`
5. `make release` — verify the binary builds clean
6. `git tag -a vx.y.z -m "Release vx.y.z — <one-line summary>"`
7. `git push origin main --tags`

CI (Gitea Actions, `.gitea/workflows/build-test.yml`) builds and
tests every push by SSHing to Taurus (real Haiku hardware).


## Haiku-Specific Notes

- **BFS extended attributes**: use `ReadAttr`/`WriteAttr`/`Query`
  tools for file metadata. Always check `claude:summary` with
  `ReadAttr` *before* reading a source file — summaries cost ~10–30
  tokens vs. thousands for a full read. Write a `claude:summary`
  after reading a file for the first time so future sessions can skip
  the full read. Only write in the `claude:*` namespace; never
  overwrite `BEOS:*`, `MAIL:*`, `Audio:*`, or other system attributes.
- **`addattr`**: the Makefile stamps `BEOS:ICON` (HVIF) and
  `BEOS:APP_SIG` onto the binary at link time. This is Haiku-only;
  the step is skipped gracefully on macOS.
- **`notify.cpp`**: wraps `BNotification` for desktop alerts. Guard
  any Haiku Kit includes behind `#ifdef __HAIKU__` if they appear
  in headers shared with the macOS build.
- **Paths**: `paths::config_path()` returns the Haiku-native
  `~/config/settings/claude-cli/` on Haiku and falls back to
  `$XDG_CONFIG_HOME` / `~/.config/claude-cli/` elsewhere.


## Files with Existing `claude:summary` Attributes

Check these with `ReadAttr` before opening the full file:

```
src/main.cpp        src/tui.cpp         src/tui.h
src/repl.cpp        src/repl.h          src/tools.h
src/paths.cpp       src/notify.cpp      src/notify.h
src/stats.h         src/telegram.cpp    src/telegram.h
Makefile            README.md
ci_scripts/test.sh  docs/GIT_WORKFLOW.md  docs/ROADMAP.md
tests/bfs_tools_test.sh
```
