#!/usr/bin/env bash
# Synchronous-turn tests — verify the structural invariants of the
# synchronous turn dispatch in session.cpp.
#
# The REPL runs api::SendWithTools inline on the main thread (RunTurn):
# the user cannot type while Claude is working. These are static
# source-code tests (grep/awk on session.cpp / tui.h) plus a couple of
# binary-level smoke tests. They do NOT require a live Claude API key.
#
# Run from the project root after a successful build:
#
#   bash tests/local_worker_test.sh

set -euo pipefail

BIN="./build/claude"
SRC="src/session.cpp"
TUIH="src/tui.h"

PASS=0
FAIL=0
SKIP=0

step() { echo; echo "--- $* ---"; }
pass() { echo "PASS"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }
skip() { echo "SKIP: $*"; SKIP=$((SKIP+1)); }

echo "=== Synchronous Turn Tests ==="

# ── S1: TurnResult struct is present ──────────────────────────────────────────
step "S1: TurnResult struct is defined in session.cpp"
grep -q 'struct TurnResult' "$SRC" || fail "struct TurnResult not found in $SRC"
pass

# ── S2: TurnResult has all required fields ────────────────────────────────────
step "S2: TurnResult declares all required fields"
LINE_TR=$(grep -n 'struct TurnResult' "$SRC" | head -1 | cut -d: -f1)
LINE_TR_END=$(awk -v after="$LINE_TR" 'NR > after && /^};/ {print NR; exit}' "$SRC")
BODY=$(awk -v s="$LINE_TR" -v e="$LINE_TR_END" 'NR>=s && NR<=e' "$SRC")
for field in ok elapsed inputTokens outputTokens cacheRead cacheWrite assistantText newUrls cancelledInput; do
    echo "$BODY" | grep -q "$field" || fail "TurnResult missing field '$field'"
done
pass

# ── S3: RunTurn is a static file-scoped function ──────────────────────────────
step "S3: RunTurn is declared 'static TurnResult RunTurn' in session.cpp"
grep -q 'static TurnResult RunTurn' "$SRC" \
    || fail "RunTurn must be declared 'static TurnResult RunTurn' in $SRC"
pass

# ── S4: The old async worker architecture is gone ─────────────────────────────
step "S4: LocalWorker/dispatch_turn/drain_turn/type-ahead are removed"
for sym in 'struct LocalWorker' 'LocalWorkerFunc' 'dispatch_turn' 'drain_turn' \
           'fWorkerOwnsDisplay' 'fQueuedInput'; do
    grep -q "$sym" "$SRC" && fail "removed symbol '$sym' still present in $SRC"
done
pass

# ── S5: No background thread in session.cpp ───────────────────────────────────
step "S5: session.cpp does not spawn a std::thread"
grep -q 'std::thread' "$SRC" \
    && fail "std::thread found in $SRC — the turn must run synchronously"
pass

# ── S6: RunTurn calls SendWithTools ───────────────────────────────────────────
step "S6: RunTurn calls api::SendWithTools"
grep -q 'api::SendWithTools' "$SRC" \
    || fail "api::SendWithTools not called in $SRC"
pass

# ── S7: Messages roll back on failure ─────────────────────────────────────────
step "S7: RunTurn rolls messages back to the snapshot on API failure"
grep -q 'messages = snapshot' "$SRC" \
    || fail "'messages = snapshot' rollback not found in $SRC"
pass

# ── S8: Written-summary paths are drained ─────────────────────────────────────
step "S8: RunTurn drains api::DrainWrittenSummaryPaths()"
grep -q 'api::DrainWrittenSummaryPaths' "$SRC" \
    || fail "api::DrainWrittenSummaryPaths() not called in $SRC"
pass

# ── S9: Cancel-and-retype records and clears the flag ─────────────────────────
step "S9: RunTurn sets result.cancelledInput and clears g_cancel_retype = 0"
grep -q 'result\.cancelledInput' "$SRC" \
    || fail "result.cancelledInput not set in $SRC"
grep -q 'g_cancel_retype = 0' "$SRC" \
    || fail "g_cancel_retype = 0 (clear) not found in $SRC"
pass

# ── S10: Post-turn bookkeeping present ────────────────────────────────────────
step "S10: post-turn bookkeeping (RecordTurn/SaveHistory/hooks) is present"
grep -q 'stats::RecordTurn'   "$SRC" || fail "stats::RecordTurn not found in $SRC"
grep -q 'config::SaveHistory' "$SRC" || fail "config::SaveHistory not found in $SRC"
grep -q 'hooks::Fire'         "$SRC" || fail "hooks::Fire not found in $SRC"
# Cache stats must be forwarded to RecordTurn (within 4 lines of the call).
LINE_RECORD=$(grep -n 'stats::RecordTurn' "$SRC" | tail -1 | cut -d: -f1)
[ -n "$LINE_RECORD" ] || fail "stats::RecordTurn not found in $SRC"
RECORD_BLOCK=$(awk -v s="$LINE_RECORD" -v e="$((LINE_RECORD+4))" 'NR>=s && NR<=e' "$SRC")
echo "$RECORD_BLOCK" | grep -q 'cacheRead'  \
    || fail "result.cacheRead not passed to stats::RecordTurn (within 4 lines)"
echo "$RECORD_BLOCK" | grep -q 'cacheWrite' \
    || fail "result.cacheWrite not passed to stats::RecordTurn (within 4 lines)"
pass

# ── S11: The loop dispatches synchronously via run_turn ───────────────────────
step "S11: the main loop calls run_turn() to dispatch a turn"
grep -q 'run_turn(' "$SRC" \
    || fail "run_turn() call not found in $SRC"
pass

# ── S12: The tui turn-output interceptor is gone ──────────────────────────────
step "S12: BeginTurn/EndTurn/FlushTurnOutput are removed from tui.h"
grep -q 'void BeginTurn'       "$TUIH" && fail "BeginTurn still declared in $TUIH"
grep -q 'void EndTurn'         "$TUIH" && fail "EndTurn still declared in $TUIH"
grep -q 'void FlushTurnOutput' "$TUIH" && fail "FlushTurnOutput still declared in $TUIH"
pass

# ── S13: Binary starts and exits cleanly ──────────────────────────────────────
step "S13: binary exits 0 for --help without hanging or crashing"
[ -x "$BIN" ] || fail "$BIN missing or not executable"
set +e
"$BIN" --help </dev/null >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "--help exited with rc=$rc (expected 0)"
pass

# ── S14: Unauthenticated one-shot exits quickly (no hang) ─────────────────────
step "S14: unauthenticated one-shot exits quickly (synchronous turn does not hang)"
home=$(mktemp -d /tmp/claude-sync-test.XXXXXX)
set +e
timeout 10 env -u ANTHROPIC_API_KEY HOME="$home" "$BIN" "hello" </dev/null >/dev/null 2>&1
rc=$?
set -e
rm -rf "$home"
[ "$rc" -ne 124 ] || fail "binary timed out — the synchronous turn may be blocking shutdown"
[ "$rc" -lt 128 ] || fail "binary exited with signal (rc=$rc) — possible crash"
[ "$rc" -ne 0   ] || fail "expected non-zero exit without auth credentials"
pass

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
