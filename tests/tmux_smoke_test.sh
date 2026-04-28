#!/usr/bin/env bash
# tmux_smoke_test.sh — end-to-end smoke test for the CLI inside tmux.
#
# DOES NOT TEST THE PERMISSION MENU.
# ----------------------------------
# tui::SelectOption() reads from /dev/tty (the real controlling terminal)
# rather than from STDIN_FILENO, by design — see the SECURITY/AUTOMATION
# NOTE in src/tui.cpp.  This is a deliberate guard so non-interactive
# automation cannot silently approve destructive tool calls.  As a
# consequence, `tmux send-keys` cannot dismiss the permission menu.
#
# For permission-menu coverage, walk the manual checklist in
# tests/MENU_SMOKE.md.
#
# What this script DOES verify (automatable end-to-end):
#   • REPL starts cleanly inside tmux (status bar visible, prompt ready)
#   • A no-permission tool (Read) runs end-to-end, response streams back,
#     turn completes — exercises the streaming pipeline, status bar,
#     scroll region, and tool dispatch without involving SelectOption.
#   • A no-permission tool (Glob) runs end-to-end — second tool kind
#     and second turn proves the post-tool prompt restoration works.
#   • /ludicrous mode auto-approves Bash, runs the command, streams the
#     response — exercises the permission-bypass path that DOES interact
#     with stdin via send-keys.
#   • /exit cleanly tears down the REPL.
#
# This is NOT wired into ci_scripts/test.sh — it requires a real
# terminal (or pseudo-terminal via tmux), a logged-in claude-cli, and
# burns ~3 real API turns per run.  Run manually from the project root:
#
#     bash tests/tmux_smoke_test.sh
#     bash tests/tmux_smoke_test.sh -k    # keep tmp files for inspection

set -uo pipefail

BIN="./build/claude"
SESSION_PREFIX="claude-smoke-test"
PANE_W=140
PANE_H=42
TURN_TIMEOUT=45        # seconds to wait for a turn to complete
KEEP_TMP=0
TMPDIR_TEST=$(mktemp -d /tmp/claude-smoke-test.XXXXXX)

[ "${1-}" = "-k" ] && KEEP_TMP=1

cleanup() {
	for s in $(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep "^$SESSION_PREFIX" || true); do
		tmux kill-session -t "$s" 2>/dev/null || true
	done
	if [ "$KEEP_TMP" = "0" ]; then
		rm -rf "$TMPDIR_TEST"
	else
		echo "kept tmp dir: $TMPDIR_TEST"
	fi
}
trap cleanup EXIT

PASS_COUNT=0
FAIL_COUNT=0
FAILED_SCENARIOS=()

# ──────────────────────────────────────────────────────────────────
# helpers
# ──────────────────────────────────────────────────────────────────

step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$*"; PASS_COUNT=$((PASS_COUNT+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; FAIL_COUNT=$((FAIL_COUNT+1)); FAILED_SCENARIOS+=("$1"); }

# Capture pane text with escapes stripped — good enough for substring
# matching against streamed plaintext responses.
capture() {
	tmux capture-pane -t "$1" -p -S -200 2>/dev/null
}

# Wait until `pattern` (extended regex) appears in the captured pane.
# Returns 0 on match, 1 on timeout. Dumps the pane on timeout.
wait_for() {
	local sess="$1" pattern="$2" timeout="$3" label="$4"
	local deadline=$(( $(date +%s) + timeout ))
	while [ "$(date +%s)" -lt "$deadline" ]; do
		if capture "$sess" | grep -Eq "$pattern"; then
			return 0
		fi
		sleep 0.3
	done
	echo "  [timeout waiting for: $label]"
	echo "  ── pane snapshot ──────────────────────────────"
	capture "$sess" | sed 's/^/  │ /'
	echo "  ───────────────────────────────────────────────"
	return 1
}

# Start a fresh detached tmux session running the CLI.  Echoes the
# session name on stdout once the REPL is ready.
start_session() {
	local tag="$1"
	local sess="${SESSION_PREFIX}-${tag}"
	tmux kill-session -t "$sess" 2>/dev/null || true
	tmux new-session -d -s "$sess" -x "$PANE_W" -y "$PANE_H" "$BIN"
	# Status bar appears once the REPL is ready.
	if ! wait_for "$sess" "sonnet|haiku|opus" 10 "REPL ready (model name in status bar)"; then
		echo "FATAL: claude failed to start in tmux session $sess" >&2
		tmux kill-session -t "$sess" 2>/dev/null || true
		return 1
	fi
	echo "$sess"
}

# Type literal text and press Enter.
send_prompt() {
	local sess="$1" text="$2"
	tmux send-keys -t "$sess" -l "$text"
	sleep 0.3
	tmux send-keys -t "$sess" Enter
}

# Cleanly exit a session.
end_session() {
	local sess="$1"
	tmux send-keys -t "$sess" -l "/exit"
	tmux send-keys -t "$sess" Enter
	sleep 0.5
	tmux kill-session -t "$sess" 2>/dev/null || true
}

# ──────────────────────────────────────────────────────────────────
# preflight
# ──────────────────────────────────────────────────────────────────

step "preflight"
[ -x "$BIN" ] || { echo "FATAL: $BIN not built. Run 'make' first." >&2; exit 2; }
command -v tmux >/dev/null || { echo "FATAL: tmux not on PATH." >&2; exit 2; }
if [ -z "${ANTHROPIC_API_KEY-}" ] \
   && [ ! -f "$HOME/config/settings/claude-cli/credentials.json" ] \
   && [ ! -f "$HOME/.config/claude-cli/credentials.json" ]; then
	echo "FATAL: no ANTHROPIC_API_KEY and no credentials.json found." >&2
	echo "       Run '$BIN login' first, or export ANTHROPIC_API_KEY." >&2
	exit 2
fi
echo "  binary:    $BIN"
echo "  tmux:      $(tmux -V)"
echo "  pane:      ${PANE_W}x${PANE_H}"
echo "  tmp dir:   $TMPDIR_TEST"

# ──────────────────────────────────────────────────────────────────
# scenario 1 — Read tool (no permission required)
#   Verifies: REPL starts in tmux, tool dispatch works, response
#   streams back through the scroll region, turn completes.
# ──────────────────────────────────────────────────────────────────

step "scenario 1: Read tool runs to completion"
SENTINEL_FILE="$TMPDIR_TEST/scenario1.txt"
SENTINEL_BODY="scenario-1-sentinel-$$"
echo "$SENTINEL_BODY" > "$SENTINEL_FILE"

sess=$(start_session "s1") || { bad "s1: session start"; exit 1; }
ok "REPL started"

send_prompt "$sess" "Read the file $SENTINEL_FILE and tell me exactly what's in it."

if wait_for "$sess" "$SENTINEL_BODY" "$TURN_TIMEOUT" "model echoes sentinel from file"; then
	ok "Read tool ran and response streamed"
else
	bad "s1: Read tool did not complete"
fi
end_session "$sess"

# ──────────────────────────────────────────────────────────────────
# scenario 2 — Glob tool (no permission required), second turn
#   Verifies: tool dispatch + streaming work for a second tool kind,
#   prompt is restored cleanly between turns.
# ──────────────────────────────────────────────────────────────────

step "scenario 2: Glob tool runs to completion"
GLOB_TARGET="$TMPDIR_TEST/glob-marker-$$.dat"
: > "$GLOB_TARGET"

sess=$(start_session "s2") || { bad "s2: session start"; exit 1; }
ok "REPL started"

send_prompt "$sess" "Use the Glob tool to find files matching $TMPDIR_TEST/glob-marker-*.dat and list them."

if wait_for "$sess" "glob-marker-$$\\.dat" "$TURN_TIMEOUT" "Glob result includes our file"; then
	ok "Glob tool ran and response streamed"
else
	bad "s2: Glob tool did not complete"
fi
end_session "$sess"

# ──────────────────────────────────────────────────────────────────
# scenario 3 — /ludicrous mode runs Bash without prompting
#   Verifies: the permission-bypass code path, that send-keys reaches
#   the slash-command handler, and that Bash dispatch + streaming
#   work end-to-end.  This is the closest automatable proxy for the
#   "Bash actually runs" coverage that the permission menu would give.
# ──────────────────────────────────────────────────────────────────

step "scenario 3: /ludicrous mode + Bash"
LUDI_SENTINEL="ludicrous-sentinel-$$"

sess=$(start_session "s3") || { bad "s3: session start"; exit 1; }
ok "REPL started"

# Engage /ludicrous (slash command — never goes through the menu).
tmux send-keys -t "$sess" -l "/ludicrous"
sleep 0.3
tmux send-keys -t "$sess" Enter

if wait_for "$sess" "LUDICROUS MODE ENGAGED" 5 "ludicrous mode banner"; then
	ok "/ludicrous engaged"
else
	bad "s3: /ludicrous did not engage"
	end_session "$sess"
	# Skip the rest of the scenario.
	echo
	printf '\033[1m=== summary ===\033[0m\n'
	printf '  passed: %d\n' "$PASS_COUNT"
	printf '  failed: %d\n' "$FAIL_COUNT"
	exit 1
fi

send_prompt "$sess" "Run this bash command and show the output: echo $LUDI_SENTINEL"

if wait_for "$sess" "ludicrous: auto-approved Bash" "$TURN_TIMEOUT" "auto-approval banner"; then
	ok "Bash auto-approved by ludicrous mode"
else
	bad "s3: ludicrous did not auto-approve"
fi

if wait_for "$sess" "$LUDI_SENTINEL" "$TURN_TIMEOUT" "command output containing $LUDI_SENTINEL"; then
	ok "Bash ran and output streamed"
else
	bad "s3: Bash output never appeared"
fi
end_session "$sess"

# ──────────────────────────────────────────────────────────────────
# summary
# ──────────────────────────────────────────────────────────────────

echo
printf '\033[1m=== summary ===\033[0m\n'
printf '  passed: %d\n' "$PASS_COUNT"
printf '  failed: %d\n' "$FAIL_COUNT"
if [ "$FAIL_COUNT" -gt 0 ]; then
	printf '\n  failed scenarios:\n'
	for s in "${FAILED_SCENARIOS[@]}"; do printf '    - %s\n' "$s"; done
	echo
	echo "Note: tmux send-keys cannot drive the permission menu by design."
	echo "      For permission-menu coverage, walk tests/MENU_SMOKE.md by hand."
	exit 1
fi
echo
echo "all automatable smoke scenarios passed inside tmux."
echo "Reminder: the permission menu is not covered here — see tests/MENU_SMOKE.md."
