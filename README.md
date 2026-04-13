# haiku-claude-cli

A minimal Claude CLI for Haiku OS.

Single-binary C++17 tool that talks to the Anthropic Messages API over
libcurl + nlohmann_json. Supports both API-key and OAuth (Pro/Max
subscription) authentication, streams responses token-by-token, reads
piped stdin, and ships a multi-turn REPL.

## Features

- **Streaming** — responses appear live via Server-Sent Events.
- **Two auth modes**:
  - `claude login` → OAuth 2.0 + PKCE against claude.ai; charges against
    your Claude Pro/Max subscription quota.
  - `ANTHROPIC_API_KEY` env var → standard Console API key, billed per
    token.
- **Stdin piping** — `cat file.txt | claude "summarize this"`; stdin is
  appended to the message when it isn't a terminal.
- **Interactive REPL** — `claude -i` for multi-turn conversations with
  full history preserved across turns.
- **Flag overrides** — `-m/--model`, `-t/--max-tokens`.
- **Haiku-native storage** — OAuth credentials live at
  `~/config/settings/claude-cli/credentials.json` on Haiku (XDG path
  elsewhere).

## Build

### On Haiku (Taurus, gcc13)

```bash
git clone ssh://git@gitea.microgeni.synology.me:2222/daniel/haiku-claude-cli.git
cd haiku-claude-cli
make
./build/claude --help
```

Required packages (install via `pkgman` if missing):

```
pkgman install devel:libcurl devel:libssl nlohmann_json pkgconfig libedit_devel
```

### On macOS (nix)

```bash
nix develop       # enters shell with gnumake, pkg-config, curl, nlohmann_json, openssl
make
./build/claude --help
```

### Install

```bash
make install      # installs to /boot/system/non-packaged/bin/claude on Haiku
```

Override `PREFIX` to install elsewhere.

## Usage

### One-shot

```bash
claude "Write a haiku about BeOS."
```

### Pipe input

```bash
cat README.md | claude "Turn this into a two-sentence elevator pitch."
```

### Interactive REPL

```bash
claude -i
```

Exit with `exit`, `quit`, `:q`, or Ctrl+D.

### Flags

```
-i, --interactive    Start a multi-turn REPL session.
-m, --model MODEL    Override the default model (claude-sonnet-4-6).
-t, --max-tokens N   Override the default max tokens (1024).
-h, --help           Show help.
```

## Authentication

### OAuth (Pro/Max subscription)

```bash
claude login
```

This opens a browser, walks you through claude.ai authorization, and
saves a short-lived access token plus a refresh token. Subsequent
`claude` calls automatically refresh expired tokens. Requests count
against your Pro/Max quota rather than per-token Console billing.

```bash
claude logout
```

Deletes stored credentials.

### API key

```bash
export ANTHROPIC_API_KEY=sk-ant-...
claude "hello"
```

Billed per token against your Console account. OAuth takes precedence
when both are available.

## Development

See `docs/GIT_WORKFLOW.md` for the branching model. Active development
lands on `dev`; releases are tagged on `main`.

CI runs via Gitea Actions on every push — the macOS runner checks out
the repo, syncs to Taurus over SSH, and runs the build + test phases
on real Haiku hardware. Workflow lives in
`.gitea/workflows/build-test.yml`; the shell underneath lives in
`ci_scripts/`.

## License

See `LICENSE`.
