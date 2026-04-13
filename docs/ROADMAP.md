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

### v0.2 — Quality of life

Make the existing text-in/text-out loop noticeably nicer to use
without adding any new capabilities.

- [ ] Config file at `~/config/settings/claude-cli/config.json` with
      defaults for model, max-tokens, system prompt, and show-usage.
- [ ] `/model MODEL` slash command in the REPL to swap models mid-session.
- [ ] `/cost` slash command showing cumulative tokens for the current
      session and, for the API-key path, a rough price estimate.
- [ ] `/clear` slash command to reset the running conversation.
- [ ] `/compact` slash command that asks Claude to summarize the
      running conversation and replaces history with the summary.
- [ ] Ctrl+C during a streaming response cancels the in-flight request
      cleanly instead of killing the process.
- [ ] Optional ANSI color in the REPL prompt with graceful fallback
      when stdout is not a TTY.

### v0.3 — Read-only tool use

Let Claude see the local project without being able to modify it.
This is the biggest architectural jump in the roadmap: introducing the
server-side tool-use loop, where Claude requests a tool, the CLI runs
it, and the result is fed back as a `tool_result` turn.

- [ ] `Bash` tool — run a shell command, return stdout/stderr, with a
      read-only heuristic (deny writes unless explicitly opted in).
- [ ] `Read` tool — read a file or a line range and return contents.
- [ ] `Glob` tool — shell-pattern file matching, results sorted by mtime.
- [ ] `Grep` tool — content search (ripgrep if available, `grep -R`
      fallback).
- [ ] Per-tool permission prompts with "allow once", "allow this session",
      "deny" choices.
- [ ] Wire the Messages API `tools` parameter and handle the
      tool_use / tool_result turn-taking correctly across streaming.

### v0.4 — Write tools

Claude can now modify files — with safety rails.

- [ ] `Write` tool — create or overwrite a file.
- [ ] `Edit` tool — exact-string replacement, with a `replace_all` mode.
- [ ] Preview every write/edit as a unified diff before the permission
      prompt.
- [ ] Auto-deny writes outside the current working directory unless
      the user opts in.

### v0.5 — Project memory

Absorb per-project context without pasting it every turn.

- [ ] Load `CLAUDE.md` from the current working directory as a
      project preamble (appended after the required Claude Code prefix
      when OAuth is used).
- [ ] Load `~/config/settings/claude-cli/CLAUDE.md` as a user-level
      preamble.
- [ ] `/memory` slash command to open the project `CLAUDE.md` in
      `$EDITOR`.

### v0.6 — Slash commands (custom + built-in polish)

- [ ] Namespace for built-in commands (`/help`, `/model`, `/cost`,
      `/clear`, `/compact`, `/memory`).
- [ ] User-defined commands loaded from `.claude/commands/*.md` in
      the project and `~/config/settings/claude-cli/commands/` globally.
- [ ] Argument support in custom commands via `{{args}}` substitution.
- [ ] Tab completion in the REPL for slash command names.

### v0.7 — Hooks

Let the user react to lifecycle events with plain shell scripts.

- [ ] Hook types: `UserPromptSubmit`, `PreToolUse`, `PostToolUse`,
      `Stop`, `SessionStart`.
- [ ] Hooks declared in `settings.json` at project or user scope.
- [ ] Each hook is a shell command; stdin receives a JSON event
      payload; non-zero exit or specific output can block a tool call
      or inject extra context.

### v0.8 — MCP (Model Context Protocol)

Interoperate with MCP servers so every MCP tool is available to this
CLI.

- [ ] stdio transport — spawn a server subprocess, speak JSON-RPC.
- [ ] `~/config/settings/claude-cli/mcp.json` for server config.
- [ ] Advertise MCP-provided tools and resources to Claude via the
      Messages API `tools` array.
- [ ] HTTP/SSE transport for remote MCP servers (follow-up).

### v0.9 — Advanced built-ins

Fill in the remaining Claude Code tools that make sense on a personal
dev machine.

- [ ] `WebFetch` tool — fetch a URL, render to Markdown, pass to Claude.
- [ ] `WebSearch` tool (depends on a search API key).
- [ ] `Task` tool — spawn sub-agents with a focused context budget
      for parallelizable work.
- [ ] Todo list tools (`TaskCreate` / `TaskUpdate` / `TaskList`) backed
      by an in-session markdown file.

### v1.0 — Stable release

Polish, docs, and Haiku-native integration.

- [ ] Man page installed to `/boot/system/documentation/man/`.
- [ ] `claude --version` with build info.
- [ ] Structured logs under `~/config/settings/claude-cli/logs/`.
- [ ] Graceful handling of quota exhaustion and token expiry mid-stream.
- [ ] End-to-end README walkthrough covering every feature.
- [ ] HaikuDepot package submission (optional — needs a packaging
      reviewer).

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
