#!/usr/bin/env bash
# Agent Skills + Subagents tests — verify the skills/ and agents/
# loaders parse SKILL.md / *.md frontmatter, expand {{args}} and
# !`cmd` dynamic context, apply project-over-user precedence, and
# resolve subagent model aliases and tool allow-lists.
#
# Tests SK1–SK10.  Run from the project root after a successful build:
#
#   bash tests/skills_agents_test.sh
#
# The harness compiles a small driver against the already-built
# build/skills.o build/agents.o build/paths.o object files, so it
# exercises the real production code paths.

set -euo pipefail

PASS=0
FAIL=0

step()  { echo; echo "--- $* ---"; }
pass()  { echo "PASS"; PASS=$((PASS+1)); }
fail()  { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }

# Locate the nlohmann/json headers the same way the Makefile does.
JSON_CFLAGS=$(pkg-config --cflags nlohmann_json 2>/dev/null || true)

OBJ_SKILLS="build/skills.o"
OBJ_AGENTS="build/agents.o"
OBJ_PATHS="build/paths.o"
for o in "$OBJ_SKILLS" "$OBJ_AGENTS" "$OBJ_PATHS"; do
	[ -f "$o" ] || { echo "FAIL: missing $o — run 'make' first" >&2; exit 1; }
done

WORK=$(mktemp -d /tmp/claude-skills-test.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# ── Fixtures ────────────────────────────────────────────────
mkdir -p "$WORK/skills/summarize-changes"
cat > "$WORK/skills/summarize-changes/SKILL.md" <<'EOF'
---
description: Summarizes changes. Use when the user asks what changed.
allowed-tools: Read Grep
---

## Changes

!`echo INJECTED_OUTPUT`

Summarize for {{args}}.
EOF

mkdir -p "$WORK/skills/deploy"
cat > "$WORK/skills/deploy/SKILL.md" <<'EOF'
---
name: deploy
description: USER deploy
disable-model-invocation: true
---
User deploy body.
EOF

mkdir -p "$WORK/proj/.claude/skills/deploy"
cat > "$WORK/proj/.claude/skills/deploy/SKILL.md" <<'EOF'
---
name: deploy
description: PROJECT deploy override
---
Project deploy body.
EOF

# A manual-only (user-invoke-only) skill that must NOT appear in the
# model-facing system block.
mkdir -p "$WORK/skills/manual-only-skill"
cat > "$WORK/skills/manual-only-skill/SKILL.md" <<'EOF'
---
name: manual-only-skill
description: Never offered to the model
disable-model-invocation: true
---
Manual body.
EOF

mkdir -p "$WORK/agents"
cat > "$WORK/agents/code-reviewer.md" <<'EOF'
---
name: code-reviewer
description: Reviews code for quality
tools: Read, Grep, Glob
model: haiku
color: blue
---
You are a senior code reviewer.
EOF

# ── Driver ──────────────────────────────────────────────────
cat > "$WORK/driver.cpp" <<'EOF'
#include "skills.h"
#include "agents.h"
#include <iostream>
int main(int argc, char** argv) {
	const std::string base = argv[1];
	skills::Load(base + "/skills", base + "/proj/.claude/skills");
	agents::Load(base + "/agents", "/nonexistent");

	// SK: dump a machine-checkable report.
	std::cout << "SKILL_COUNT=" << skills::All().size() << "\n";
	const auto* sc = skills::Find("summarize-changes");
	std::cout << "SC_TOOLS=" << (sc ? sc->allowedTools : "MISSING") << "\n";
	std::cout << "SC_MODELINV=" << (sc && !sc->disableModelInvocation) << "\n";
	const auto* dep = skills::Find("deploy");
	std::cout << "DEPLOY_DESC=" << (dep ? dep->description : "MISSING") << "\n";

	bool found = false;
	std::string exp = skills::Expand("summarize-changes", "the parser", found);
	std::cout << "EXPAND_FOUND=" << found << "\n";
	std::cout << "EXPAND_HAS_INJECT=" << (exp.find("INJECTED_OUTPUT") != std::string::npos) << "\n";
	std::cout << "EXPAND_HAS_ARGS=" << (exp.find("the parser") != std::string::npos) << "\n";
	std::cout << "EXPAND_NO_MARKER=" << (exp.find("{{args}}") == std::string::npos) << "\n";

	const std::string sysblock = skills::SystemBlock();
	std::cout << "SYS_HAS_SUMMARIZE=" << (sysblock.find("summarize-changes") != std::string::npos) << "\n";
	// The project deploy override is model-invocable (no disable flag),
	// so it SHOULD appear. Verify a separate manual-only skill is omitted.
	std::cout << "SYS_HAS_DEPLOY=" << (sysblock.find("deploy") != std::string::npos) << "\n";
	std::cout << "SYS_OMITS_MANUAL=" << (sysblock.find("manual-only-skill") == std::string::npos) << "\n";

	std::cout << "AGENT_COUNT=" << agents::All().size() << "\n";
	const auto* cr = agents::Find("code-reviewer");
	std::cout << "CR_RESOLVED=" << (cr ? agents::ResolveModel(*cr, "parent") : "MISSING") << "\n";
	std::cout << "CR_TOOLN=" << (cr ? agents::ToolAllowList(*cr).size() : 0) << "\n";
	std::cout << "CR_PROMPT_OK=" << (cr && cr->prompt.find("senior code reviewer") != std::string::npos) << "\n";
	return 0;
}
EOF

step "compile skills/agents test driver"
# skills.cpp uses the Be API (BNode/BVolume) for usage attributes on
# Haiku, so the driver links libbe there. Harmless elsewhere: the
# flag is only added when the library exists.
BE_LIB=""
[ "$(uname -s)" = "Haiku" ] && BE_LIB="-lbe"
g++ -std=c++17 -Isrc $JSON_CFLAGS "$WORK/driver.cpp" \
	"$OBJ_SKILLS" "$OBJ_AGENTS" "$OBJ_PATHS" $BE_LIB -o "$WORK/driver" \
	|| { fail "driver failed to compile"; echo "Results: $PASS passed, $((FAIL+1)) failed"; exit 1; }
pass

REPORT=$("$WORK/driver" "$WORK")
get() { echo "$REPORT" | grep "^$1=" | cut -d= -f2-; }

step "SK1: all skills load (user + project, deploy de-duped)"
[ "$(get SKILL_COUNT)" = "3" ] && pass || fail "expected 3 skills (got $(get SKILL_COUNT))"

step "SK2: allowed-tools frontmatter parsed"
[ "$(get SC_TOOLS)" = "Read Grep" ] && pass || fail "tools wrong: $(get SC_TOOLS)"

step "SK3: model-invocable skill flagged invocable"
[ "$(get SC_MODELINV)" = "1" ] && pass || fail "summarize-changes should be model-invocable"

step "SK4: project skill overrides user skill of same name"
[ "$(get DEPLOY_DESC)" = "PROJECT deploy override" ] && pass || fail "override failed: $(get DEPLOY_DESC)"

step "SK5: skill expand resolves by name"
[ "$(get EXPAND_FOUND)" = "1" ] && pass || fail "Expand did not find summarize-changes"

step "SK6: dynamic !\`cmd\` context injected into body"
[ "$(get EXPAND_HAS_INJECT)" = "1" ] && pass || fail "!cmd output not inlined"

step "SK7: {{args}} substituted and marker removed"
[ "$(get EXPAND_HAS_ARGS)" = "1" ] && [ "$(get EXPAND_NO_MARKER)" = "1" ] \
	&& pass || fail "args substitution wrong"

step "SK8: system block lists model-invocable, omits manual-only"
[ "$(get SYS_HAS_SUMMARIZE)" = "1" ] && [ "$(get SYS_HAS_DEPLOY)" = "1" ] \
	&& [ "$(get SYS_OMITS_MANUAL)" = "1" ] \
	&& pass || fail "system block wrong"

step "SK9: subagent loads and resolves model alias (haiku)"
[ "$(get AGENT_COUNT)" = "1" ] && [ "$(get CR_RESOLVED)" = "claude-haiku-4-5" ] \
	&& pass || fail "agent/model resolution wrong: $(get CR_RESOLVED)"

step "SK10: subagent tool allow-list parsed and prompt captured"
[ "$(get CR_TOOLN)" = "3" ] && [ "$(get CR_PROMPT_OK)" = "1" ] \
	&& pass || fail "agent tools/prompt wrong (tools=$(get CR_TOOLN))"

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
