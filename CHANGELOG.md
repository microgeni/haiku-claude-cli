# Changelog

All notable changes to this project are recorded here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `-s/--system TEXT` — custom system prompt. When OAuth is used the
  required Claude Code prefix is preserved and the user text is
  appended; with an API key the user text is used verbatim.
- `-u/--usage` — after the response, print input/output token counts
  to stderr (`[usage] input: N tokens  output: M tokens`).
- `-r/--resume` — preload the REPL with the last saved session from
  `~/config/settings/claude-cli/history.json` (Haiku) or the XDG path
  elsewhere. Implies `-i`. Conversation history is auto-saved after
  every successful turn.
- `CHANGELOG.md` following Keep a Changelog; release notes on
  Gitea releases are now extracted from it automatically.
- `docs/ROADMAP.md` describing milestones from v0.2 through v1.0
  toward Claude Code feature parity, including a dedicated Terminal
  UI polish milestone.
- Terminal UI polish (`tui` module):
  - ANSI color detection honoring `NO_COLOR`, `CLICOLOR=0`,
    `TERM=dumb`, plus `--plain` / `--color` overrides.
  - Bold cyan `you> ` and bold magenta `claude> ` REPL prompts;
    dim-styled meta notes.
  - Braille "thinking" spinner between request submit and first
    rendered output.
  - Streaming markdown renderer supporting **bold**, *italic* /
    _italic_, \`inline code\`, \`\`\` fenced code blocks, `#`/`##`/`###`
    headings, bullet and numbered lists.
  - Syntax highlighting inside code blocks for C/C++, Python, Shell,
    Rust, and JSON (keywords, strings, numbers, comments, C/C++
    preprocessor lines).
  - Line editing in the REPL via libedit: arrow-key history,
    emacs-style bindings, and persistent history at
    `~/config/settings/claude-cli/repl_history`.

### Changed
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
