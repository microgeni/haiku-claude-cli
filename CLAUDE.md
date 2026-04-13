# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Pre-implementation. The repository currently contains only `README.md`, `LICENSE`, and `.gitignore` — no source, build system, or tests yet.

Goal (from README): a Claude CLI for HaikuOS.

Update this file once a language/toolchain is chosen and the initial scaffolding lands (build/run/test commands, module layout, any Haiku-specific build steps).


## Git Workflow

See `docs/GIT_WORKFLOW.md` for full details.

- **`dev`** branch: active development
- **`main`** branch: stable releases only
- **Tags**: semantic versioning `v{MAJOR}.{MINOR}.{PATCH}`
- **CI**: Gitea Actions builds and tests every push (`.gitea/workflows/build-test.yml`)


## Build Environment

- **Target machine**: `taurus.microgeni.synology.me` (Haiku x86_64, also ARM64 in future)
- **Dev machine**: macOS with **nix-darwin** (flakes enabled)
- **Dev shell**: `nix develop` provides OpenSSL, libargon2, pkg-config
- **Build**: Standard Makefile, `CC ?= cc` (works with both gcc on Haiku and clang via nix on macOS)
- **CI**: Gitea Actions runner on macOS, SSHs to Taurus for Haiku builds/tests
- **Workflow**: Prototype locally on macOS via nix, final build and test on Taurus (real Haiku hardware with AES-NI)


## Useful Commands

```bash
# ── Local development (macOS with nix) ──
nix develop              # Enter dev shell
nix develop -c make      # Build
nix develop -c make test # Run tests
nix develop -c make clean

# ── On Taurus (Haiku) ──
ssh user@taurus.microgeni.synology.me
cd /Data/Code/Projects/haiku-claude-cli
make                     # Build (gcc)
make test                # Run tests


## Code Style

- Follow Haiku coding guidelines
https://www.haiku-os.org/development/coding-guidelines


