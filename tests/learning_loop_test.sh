#!/usr/bin/env bash
# Learning-loop tests — verify the features ported from the Hermes
# agent study:
#
#   LL1-LL4  skill usage telemetry + lifecycle via BFS attributes
#   LL5-LL7  /learn prompt construction
#   LL8-LL9  tiered system prompt (cache-friendly stable prefix)
#   LL10-LL11 large tool output spilled to disk, not truncated away
#   LL12     iteration budget constants wired into the tool loop
#
# Run from the project root after a successful build:
#
#   bash tests/learning_loop_test.sh
#
# The BFS-attribute tests only assert real behaviour on Haiku; on
# other platforms the usage API is a documented no-op and those
# checks are skipped.

set -euo pipefail

PASS=0
FAIL=0
SKIP=0

step()  { echo; echo "--- $* ---"; }
pass()  { echo "PASS"; PASS=$((PASS+1)); }
fail()  { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }
skip()  { echo "SKIP: $*"; SKIP=$((SKIP+1)); }

JSON_CFLAGS=$(pkg-config --cflags nlohmann_json 2>/dev/null || true)

OBJ_SKILLS="build/skills.o"
OBJ_PATHS="build/paths.o"
OBJ_LEARN="build/learn.o"
for o in "$OBJ_SKILLS" "$OBJ_PATHS" "$OBJ_LEARN"; do
	[ -f "$o" ] || { echo "FAIL: missing $o — run 'make' first" >&2; exit 1; }
done

IS_HAIKU=0
[ "$(uname -s)" = "Haiku" ] && IS_HAIKU=1

WORK=$(mktemp -d /tmp/claude-learn-test.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# ── Fixtures ────────────────────────────────────────────────────────
mkdir -p "$WORK/skills/fresh-skill"
cat > "$WORK/skills/fresh-skill/SKILL.md" <<'EOF'
---
name: fresh-skill
description: Do a fresh thing.
---
# Fresh Skill
Body.
EOF

mkdir -p "$WORK/skills/old-skill"
cat > "$WORK/skills/old-skill/SKILL.md" <<'EOF'
---
name: old-skill
description: Do an old thing.
---
# Old Skill
Body.
EOF

mkdir -p "$WORK/proj/.claude/skills"

# Age old-skill's SKILL.md well past the archive threshold so the
# never-used path ages from mtime.
touch -t 202001010000 "$WORK/skills/old-skill/SKILL.md"

# ── Driver ──────────────────────────────────────────────────────────
cat > "$WORK/driver.cpp" <<'EOF'
#include <iostream>
#include <string>
#include "skills.h"
#include "learn.h"

int main(int argc, char** argv) {
	const std::string base = argc > 1 ? argv[1] : ".";
	skills::Load(base + "/skills", base + "/proj/.claude/skills");

	// Baseline: both skills present, no uses recorded yet.
	const auto* fresh = skills::Find("fresh-skill");
	std::cout << "FRESH_USES0=" << (fresh ? fresh->uses : -1) << "\n";

	// Record two uses of fresh-skill.
	bool found = false;
	skills::Expand("fresh-skill", "", found);
	skills::RecordUse("fresh-skill");
	skills::Load(base + "/skills", base + "/proj/.claude/skills");
	const auto* fresh2 = skills::Find("fresh-skill");
	std::cout << "FRESH_USES2=" << (fresh2 ? fresh2->uses : -1) << "\n";
	std::cout << "FRESH_LASTUSED_SET=" << (fresh2 && fresh2->lastUsed > 0) << "\n";

	// Lifecycle: old-skill (mtime in 2020, never used) must archive;
	// fresh-skill (just used) must stay active. An unset state attribute
	// means active — a no-op sweep deliberately writes nothing — so
	// normalise it here to keep the assertion about behaviour, not
	// about whether an attribute happens to exist.
	skills::ApplyLifecycle(30, 90);
	const auto* oldS = skills::Find("old-skill");
	const auto* freshS = skills::Find("fresh-skill");
	auto norm = [](const std::string& s) {
		return s.empty() ? std::string("active") : s;
	};
	std::cout << "OLD_STATE=" << (oldS ? norm(oldS->state) : "MISSING") << "\n";
	std::cout << "FRESH_STATE=" << (freshS ? norm(freshS->state) : "MISSING") << "\n";

	// Archived skills must not appear in the system prompt.
	const std::string sys = skills::SystemBlock();
	std::cout << "SYS_OMITS_ARCHIVED=" << (sys.find("old-skill") == std::string::npos) << "\n";
	std::cout << "SYS_HAS_FRESH=" << (sys.find("fresh-skill") != std::string::npos) << "\n";

	// Pinning revives an archived skill and exempts it.
	skills::SetPinned("old-skill", true);
	skills::ApplyLifecycle(30, 90);
	const auto* pinned = skills::Find("old-skill");
	std::cout << "PINNED_FLAG=" << (pinned && pinned->pinned) << "\n";
	std::cout << "PINNED_STATE=" << (pinned ? pinned->state : "MISSING") << "\n";

	// /learn prompt construction.
	const std::string p1 = learn::BuildPrompt("https://example.com/api focus on auth");
	std::cout << "LEARN_HAS_REQ=" << (p1.find("focus on auth") != std::string::npos) << "\n";
	std::cout << "LEARN_HAS_60=" << (p1.find("60 characters") != std::string::npos) << "\n";
	std::cout << "LEARN_HAS_TOOLS=" << (p1.find("`WebFetch`") != std::string::npos) << "\n";
	const std::string p2 = learn::BuildPrompt("   ");
	std::cout << "LEARN_EMPTY_FALLBACK="
		<< (p2.find("this conversation") != std::string::npos) << "\n";
	return 0;
}
EOF

step "compile learning-loop test driver"
BE_LIB=""
[ "$IS_HAIKU" = "1" ] && BE_LIB="-lbe"
g++ -std=c++17 -Isrc $JSON_CFLAGS "$WORK/driver.cpp" \
	"$OBJ_SKILLS" "$OBJ_PATHS" "$OBJ_LEARN" $BE_LIB -o "$WORK/driver" \
	|| { fail "driver failed to compile"; echo "Results: $PASS passed, $((FAIL+1)) failed"; exit 1; }
pass

REPORT=$("$WORK/driver" "$WORK")
get() { echo "$REPORT" | grep "^$1=" | cut -d= -f2-; }

# ── LL1-LL4: usage telemetry + lifecycle ────────────────────────────
step "LL1: fresh skill starts with zero recorded uses"
[ "$(get FRESH_USES0)" = "0" ] && pass || fail "expected 0 (got $(get FRESH_USES0))"

if [ "$IS_HAIKU" = "1" ]; then
	step "LL2: Expand + RecordUse bump the use counter, persisted to BFS"
	[ "$(get FRESH_USES2)" = "2" ] && pass \
		|| fail "expected 2 uses after Expand+RecordUse (got $(get FRESH_USES2))"

	step "LL3: last-used timestamp is stamped"
	[ "$(get FRESH_LASTUSED_SET)" = "1" ] && pass || fail "lastUsed not set"

	step "LL4: unused skill archives, recently-used stays active"
	[ "$(get OLD_STATE)" = "archived" ] && [ "$(get FRESH_STATE)" = "active" ] \
		&& pass || fail "states wrong: old=$(get OLD_STATE) fresh=$(get FRESH_STATE)"

	step "LL5: archived skills leave the system prompt, active ones stay"
	[ "$(get SYS_OMITS_ARCHIVED)" = "1" ] && [ "$(get SYS_HAS_FRESH)" = "1" ] \
		&& pass || fail "system block wrong"

	step "LL6: pinning revives an archived skill and exempts it"
	[ "$(get PINNED_FLAG)" = "1" ] && [ "$(get PINNED_STATE)" = "active" ] \
		&& pass || fail "pin failed: flag=$(get PINNED_FLAG) state=$(get PINNED_STATE)"
else
	skip "LL2-LL6: BFS usage attributes are Haiku-only"
fi

# ── LL7-LL9: /learn prompt ──────────────────────────────────────────
step "LL7: /learn prompt carries the user's full request verbatim"
[ "$(get LEARN_HAS_REQ)" = "1" ] && pass || fail "request text missing from prompt"

step "LL8: /learn prompt states the 60-char description limit"
[ "$(get LEARN_HAS_60)" = "1" ] && pass || fail "authoring standard missing"

step "LL9: /learn prompt frames work through our tool names"
[ "$(get LEARN_HAS_TOOLS)" = "1" ] && pass || fail "tool framing missing"

step "LL10: bare /learn falls back to distilling the conversation"
[ "$(get LEARN_EMPTY_FALLBACK)" = "1" ] && pass || fail "empty-request fallback missing"

# ── LL11-LL13: source-level wiring checks ───────────────────────────
step "LL11: ComposeSystem is defined in terms of the tiered split"
grep -q "ComposeSystemTiers(flag_system, working_dir)" src/config.cpp \
	&& pass || fail "ComposeSystem no longer delegates to ComposeSystemTiers"

step "LL12: api.cpp spends all four cache breakpoints"
grep -q "kMaxCacheBreakpoints = 4" src/api.cpp \
	&& grep -q "ApplyMessageCacheMarkers" src/api.cpp \
	&& pass || fail "cache breakpoint wiring missing"

step "LL13: stable system prefix is verified before splitting"
grep -q "systemText.compare(0, stable_prefix.size(), stable_prefix) == 0" src/api.cpp \
	&& pass || fail "prefix verification missing — split could corrupt the prompt"

step "LL14: large tool output is spilled to a file, not discarded"
grep -q "CapOutput" src/tools.cpp && grep -q "claude-results" src/tools.cpp \
	&& pass || fail "tool result spilling not wired"

step "LL15: spill path is reported so the model can Read it back"
grep -q "saved to " src/tools.cpp && pass || fail "spill path not surfaced to the model"

step "LL16: iteration budget caps the tool loop"
grep -q "kMaxToolIterations" src/api.cpp \
	&& grep -q "iterations_used >= kMaxToolIterations" src/api.cpp \
	&& pass || fail "iteration budget not enforced"

step "LL17: read-only tools are refunded (not charged to the budget)"
grep -q "tools::IsReadOnly(tname)" src/api.cpp \
	&& grep -q "if (round_was_mutating) ++iterations_used" src/api.cpp \
	&& pass || fail "read-only refund missing"

# ── LL18-LL20: GUI "Tools > Learn a Skill…" wiring ──────────────────
step "LL18: MSG_LEARN_SKILL is defined and handled"
grep -q "MSG_LEARN_SKILL" src/gui_messages.h \
	&& grep -q "case gui::MSG_LEARN_SKILL:" src/chat_window.cpp \
	&& pass || fail "learn message not defined/handled"

step "LL19: the Tools menu offers Learn a Skill and it builds the shared prompt"
grep -q "Learn a Skill" src/chat_window.cpp \
	&& grep -q "learn::BuildPrompt(request)" src/chat_window.cpp \
	&& pass || fail "Tools menu item or prompt call missing"

step "LL20: blank input is distinguished from cancel (no accidental turn)"
grep -q "modal->Go(&accepted)" src/chat_window.cpp \
	&& grep -q "if (!accepted) return;" src/chat_window.cpp \
	&& pass || fail "cancel and blank-field are conflated"

step "LL21: RenameModal::Go reads its results before Quit() frees the window"
grep -q "const bool        ok = fAccepted;" src/gui_widgets.cpp \
	&& pass || fail "Go() may read freed state after Quit()"

# ── LL22-LL28: the Skill tool (autonomous invocation) ───────────────
cat > "$WORK/skilltool.cpp" <<'EOF'
#include <iostream>
#include "skills.h"
#include "tools.h"
#include "api.h"
int main(int argc, char** argv) {
	const std::string base = argc > 1 ? argv[1] : ".";
	skills::Load(base + "/skills", base + "/proj/.claude/skills");

	// The Skill tool must be advertised, with a name enum of real skills.
	bool present = false;
	nlohmann::json schema;
	for (const auto& t : tools::Definitions())
		if (t.value("name", "") == "Skill") { present = true; schema = t; }
	std::cout << "TOOL_PRESENT=" << present << "\n";
	std::cout << "ENUM_HAS_FRESH="
		<< (schema.dump().find("fresh-skill") != std::string::npos) << "\n";

	// Loading returns the real body, with {{args}} substituted.
	nlohmann::json in; in["name"] = "fresh-skill"; in["args"] = "ARGVALUE";
	const auto r = tools::Run("Skill", in);
	std::cout << "LOAD_OK=" << (!r.is_error) << "\n";
	std::cout << "BODY_HAS_STEPS="
		<< (r.content.find("Body.") != std::string::npos) << "\n";

	// Unknown skill lists the real ones so the model self-corrects.
	nlohmann::json bad; bad["name"] = "fresh-skil";
	const auto rb = tools::Run("Skill", bad);
	std::cout << "BAD_IS_ERROR=" << rb.is_error << "\n";
	std::cout << "BAD_LISTS_KNOWN="
		<< (rb.content.find("fresh-skill") != std::string::npos) << "\n";

	// Permission gating: plain skill is free, shell-bearing one prompts.
	nlohmann::json plain; plain["name"] = "fresh-skill";
	nlohmann::json shell; shell["name"] = "shell-skill";
	std::cout << "PLAIN_NEEDS_PERM="
		<< tools::RequiresPermission("Skill", plain) << "\n";
	std::cout << "SHELL_NEEDS_PERM="
		<< tools::RequiresPermission("Skill", shell) << "\n";
	std::cout << "PREVIEW_SHOWS_CMD="
		<< (tools::Preview("Skill", shell).find("echo SIDEEFFECT")
			!= std::string::npos) << "\n";

	// Plan mode: Skill stays available but must not execute shell.
	// NB: assert on the command's *output* ("State: SIDEEFFECT"), not the
	// bare word — the "[not run in plan mode: echo SIDEEFFECT]" placeholder
	// legitimately echoes the command text back.
	api::g_plan_mode.store(true);
	const auto rp = tools::Run("Skill", shell);
	std::cout << "PLAN_SUPPRESSES_SHELL="
		<< (rp.content.find("State: SIDEEFFECT") == std::string::npos) << "\n";
	std::cout << "PLAN_REPORTS_CMD="
		<< (rp.content.find("not run in plan mode") != std::string::npos) << "\n";
	// Sanity check the assertion itself: outside plan mode the command DOES
	// run, so "State: SIDEEFFECT" must appear. Without this a typo in the
	// needle above would make the security test vacuously pass.
	api::g_plan_mode.store(false);
	const auto rn = tools::Run("Skill", shell);
	std::cout << "NORMAL_RUNS_SHELL="
		<< (rn.content.find("State: SIDEEFFECT") != std::string::npos) << "\n";
	api::g_plan_mode.store(true);
	bool planHasSkill = false;
	for (const auto& t : tools::Definitions())
		if (t.value("name", "") == "Skill") planHasSkill = true;
	std::cout << "PLAN_KEEPS_SKILL=" << planHasSkill << "\n";
	api::g_plan_mode.store(false);
	return 0;
}
EOF

mkdir -p "$WORK/skills/shell-skill"
cat > "$WORK/skills/shell-skill/SKILL.md" <<'EOF'
---
name: shell-skill
description: Report something via the shell.
---
# Shell Skill
State: !`echo SIDEEFFECT`
EOF

step "compile Skill-tool test driver"
P_OBJS=$(ls build/*.o | grep -vE "main\.o|app_main_gui\.o|gui_" | tr '\n' ' ')
g++ -std=c++17 -Isrc $JSON_CFLAGS "$WORK/skilltool.cpp" $P_OBJS \
	$BE_LIB -lcurl -lssl -lcrypto -ledit -o "$WORK/skilltool" 2>/dev/null \
	|| { fail "Skill-tool driver failed to compile"; \
	     echo "=== Results: $PASS passed, $((FAIL)) failed, $SKIP skipped ==="; exit 1; }
pass

SREPORT=$("$WORK/skilltool" "$WORK")
sget() { echo "$SREPORT" | grep "^$1=" | cut -d= -f2-; }

step "LL22: the Skill tool is advertised with an enum of installed skills"
[ "$(sget TOOL_PRESENT)" = "1" ] && [ "$(sget ENUM_HAS_FRESH)" = "1" ] \
	&& pass || fail "Skill tool missing or enum not populated"

step "LL23: loading a skill returns the real body with {{args}} substituted"
[ "$(sget LOAD_OK)" = "1" ] && [ "$(sget BODY_HAS_STEPS)" = "1" ] \
	&& pass || fail "skill body not returned"

step "LL24: an unknown skill errors and lists the real ones"
[ "$(sget BAD_IS_ERROR)" = "1" ] && [ "$(sget BAD_LISTS_KNOWN)" = "1" ] \
	&& pass || fail "unknown-skill path does not self-correct"

step "LL25: only shell-bearing skills require permission"
[ "$(sget PLAIN_NEEDS_PERM)" = "0" ] && [ "$(sget SHELL_NEEDS_PERM)" = "1" ] \
	&& pass || fail "permission gating wrong: plain=$(sget PLAIN_NEEDS_PERM) shell=$(sget SHELL_NEEDS_PERM)"

step "LL26: the permission preview shows the exact command to be run"
[ "$(sget PREVIEW_SHOWS_CMD)" = "1" ] && pass || fail "preview does not show the command"

step "LL27: plan mode reports shell commands instead of running them"
[ "$(sget PLAN_SUPPRESSES_SHELL)" = "1" ] && [ "$(sget PLAN_REPORTS_CMD)" = "1" ] \
	&& [ "$(sget NORMAL_RUNS_SHELL)" = "1" ] \
	&& pass || fail "plan mode executed shell, or the assertion is vacuous"

step "LL28: Skill survives the plan-mode read-only tool filter"
[ "$(sget PLAN_KEEPS_SKILL)" = "1" ] && pass || fail "Skill stripped in plan mode"

# ── LL29-LL30: GUI skills menu ──────────────────────────────────────
step "LL29: the GUI Tools menu has a Skills submenu wired to a handler"
grep -q "MSG_RUN_SKILL" src/gui_messages.h \
	&& grep -q "case gui::MSG_RUN_SKILL:" src/chat_window.cpp \
	&& grep -q "_RefreshSkillMenu" src/chat_window.cpp \
	&& pass || fail "GUI skills menu not wired"

step "LL30: the GUI confirms before running a shell-bearing skill"
grep -q "skills::BodyRunsShell(name)" src/chat_window.cpp \
	&& pass || fail "GUI runs shell-bearing skills without confirmation"

# ── LL31-LL33: diagnostics reports skill visibility ─────────────────
cat > "$WORK/diag.cpp" <<'EOF'
#include <iostream>
#include "skills.h"
#include "diagnostics.h"
int main(int argc, char** argv) {
	const std::string base = argc > 1 ? argv[1] : ".";
	skills::Load(base + "/skills", base + "/proj/.claude/skills");
	const std::string r = diagnostics::BuildReport("m", base, "0.0.0");
	std::cout << "HAS_SECTION="
		<< (r.find("Agent Skills") != std::string::npos) << "\n";
	std::cout << "LISTS_SKILL="
		<< (r.find("fresh-skill") != std::string::npos) << "\n";
	std::cout << "FLAGS_SHELL="
		<< (r.find("runs shell on expand") != std::string::npos) << "\n";
	std::cout << "EXPLAINS_HIDDEN="
		<< (r.find("hidden from Claude:") != std::string::npos) << "\n";
	std::cout << "HAS_NODESC_REASON="
		<< (r.find("no description in frontmatter") != std::string::npos) << "\n";
	std::cout << "HAS_COUNT="
		<< (r.find("advertised to Claude") != std::string::npos) << "\n";
	return 0;
}
EOF

# A skill with no description is invisible to Claude but otherwise looks
# fine on disk — exactly the case the report has to explain.
mkdir -p "$WORK/skills/nodesc-skill"
printf -- '---\nname: nodesc-skill\n---\n# No description\n' \
	> "$WORK/skills/nodesc-skill/SKILL.md"

step "compile diagnostics test driver"
g++ -std=c++17 -Isrc $JSON_CFLAGS "$WORK/diag.cpp" $P_OBJS \
	$BE_LIB -lcurl -lssl -lcrypto -ledit -o "$WORK/diag" 2>/dev/null \
	|| { fail "diagnostics driver failed to compile"; \
	     echo "=== Results: $PASS passed, $((FAIL)) failed, $SKIP skipped ==="; exit 1; }
pass

DREPORT=$("$WORK/diag" "$WORK")
dget() { echo "$DREPORT" | grep "^$1=" | cut -d= -f2-; }

step "LL31: diagnostics has an Agent Skills section listing installed skills"
[ "$(dget HAS_SECTION)" = "1" ] && [ "$(dget LISTS_SKILL)" = "1" ] \
	&& pass || fail "skills section missing from the report"

step "LL32: diagnostics flags skills that run shell on expand"
[ "$(dget FLAGS_SHELL)" = "1" ] && pass || fail "shell-bearing skill not flagged"

step "LL33: diagnostics explains why a skill is hidden from Claude"
[ "$(dget EXPLAINS_HIDDEN)" = "1" ] && [ "$(dget HAS_NODESC_REASON)" = "1" ] \
	&& [ "$(dget HAS_COUNT)" = "1" ] \
	&& pass || fail "report does not explain prompt exclusion"

step "LL34: the CLI exposes the same report via /doctor"
grep -q 'cmd == "/doctor"' src/commands.cpp \
	&& grep -q "diagnostics::BuildReport" src/commands.cpp \
	&& grep -q '"/doctor"' src/session.cpp \
	&& pass || fail "/doctor not wired into the CLI"

# ── LL35-LL38: skill-authoring guidance (closing the learning loop) ──
cat > "$WORK/guidance.cpp" <<'EOF'
#include <iostream>
#include "skills.h"
#include "config.h"
int main(int argc, char** argv) {
	const std::string base = argc > 1 ? argv[1] : ".";

	// With skills installed: both the index and the guidance are present.
	skills::Load(base + "/skills", base + "/proj/.claude/skills");
	const std::string withSkills = skills::SystemBlock();
	std::cout << "WITH_HAS_INDEX="
		<< (withSkills.find("Available skills") != std::string::npos) << "\n";
	std::cout << "WITH_HAS_GUIDANCE="
		<< (withSkills.find("Creating skills:") != std::string::npos) << "\n";

	// Bare install: no index, but guidance must still appear — otherwise
	// the loop can never bootstrap from zero skills.
	skills::Load(base + "/no-such-dir-a", base + "/no-such-dir-b");
	const std::string bare = skills::SystemBlock();
	std::cout << "BARE_NO_INDEX="
		<< (bare.find("Available skills") == std::string::npos) << "\n";
	std::cout << "BARE_HAS_GUIDANCE="
		<< (bare.find("Creating skills:") != std::string::npos) << "\n";

	// Suggest-only: the model must be told to offer, not to act alone.
	std::cout << "IS_SUGGEST_ONLY="
		<< (bare.find("do not create one unprompted") != std::string::npos) << "\n";
	// Maintenance half of the loop.
	std::cout << "HAS_MAINTENANCE="
		<< (bare.find("wrong, outdated") != std::string::npos) << "\n";
	// The 60-char rule must be restated here, since this is the path that
	// bypasses /learn's embedded authoring standards.
	std::cout << "HAS_60_RULE="
		<< (bare.find("60 characters") != std::string::npos) << "\n";

	// The guidance belongs in the cached stable tier, not the volatile one.
	const auto tiers = config::ComposeSystemTiers("", "/tmp");
	std::cout << "GUIDANCE_IN_STABLE="
		<< (tiers.stable.find("Creating skills:") != std::string::npos) << "\n";
	std::cout << "GUIDANCE_NOT_VOLATILE="
		<< (tiers.volatileTier.find("Creating skills:") == std::string::npos) << "\n";
	return 0;
}
EOF

step "compile skill-guidance test driver"
g++ -std=c++17 -Isrc $JSON_CFLAGS "$WORK/guidance.cpp" $P_OBJS \
	$BE_LIB -lcurl -lssl -lcrypto -ledit -o "$WORK/guidance" 2>/dev/null \
	|| { fail "guidance driver failed to compile"; \
	     echo "=== Results: $PASS passed, $((FAIL)) failed, $SKIP skipped ==="; exit 1; }
pass

GREPORT=$("$WORK/guidance" "$WORK")
gget() { echo "$GREPORT" | grep "^$1=" | cut -d= -f2-; }

step "LL35: guidance ships alongside the skill index when skills exist"
[ "$(gget WITH_HAS_INDEX)" = "1" ] && [ "$(gget WITH_HAS_GUIDANCE)" = "1" ] \
	&& pass || fail "index or guidance missing when skills are installed"

step "LL36: guidance is present on a bare install (loop can bootstrap)"
[ "$(gget BARE_NO_INDEX)" = "1" ] && [ "$(gget BARE_HAS_GUIDANCE)" = "1" ] \
	&& pass || fail "a fresh install never learns that skills can be created"

step "LL37: guidance is suggest-only and covers maintenance + the 60-char rule"
[ "$(gget IS_SUGGEST_ONLY)" = "1" ] && [ "$(gget HAS_MAINTENANCE)" = "1" ] \
	&& [ "$(gget HAS_60_RULE)" = "1" ] \
	&& pass || fail "guidance is missing a constraint"

step "LL38: guidance sits in the cached stable tier (no per-turn cost)"
[ "$(gget GUIDANCE_IN_STABLE)" = "1" ] && [ "$(gget GUIDANCE_NOT_VOLATILE)" = "1" ] \
	&& pass || fail "guidance is in the volatile tier — would break the cache"

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
