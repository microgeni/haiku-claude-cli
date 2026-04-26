#!/usr/bin/env bash
# Tests for v1.4 (cancel-and-retype) and v1.4.1 (BFS snapshot refresh).
#
# All tests are static source-inspection or binary smoke tests —
# no live API key or network access required.
#
# Tests B1–B30.  Run from the project root after a successful build:
#
#   bash tests/async_features_test.sh

set -euo pipefail

BIN="./build/claude"
API="src/api.cpp"
APIH="src/api.h"
CFG="src/config.cpp"
CFGH="src/config.h"
REPL="src/repl.cpp"
REPLH="src/repl.h"
SESS="src/session.cpp"
CMDS="src/commands.cpp"

PASS=0
FAIL=0
SKIP=0

step() { echo; echo "--- $* ---"; }
pass() { echo "PASS"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }
skip() { echo "SKIP: $*"; SKIP=$((SKIP+1)); }

echo "=== v1.4 / v1.4.1 Async-Feature Tests ==="

# ════════════════════════════════════════════════════════════════════════════
# v1.4 — Cancel-and-retype (Ctrl+X)
# ════════════════════════════════════════════════════════════════════════════

# ── B1: g_cancel_retype is declared in api.h ─────────────────────────────────
step "B1: g_cancel_retype is declared as extern volatile sig_atomic_t in api.h"
grep -q 'g_cancel_retype' "$APIH" \
    || fail "g_cancel_retype not declared in $APIH"
grep -q 'extern volatile sig_atomic_t g_cancel_retype' "$APIH" \
    || fail "g_cancel_retype must be declared 'extern volatile sig_atomic_t' in $APIH"
pass

# ── B2: g_cancel_retype is defined in api.cpp ────────────────────────────────
step "B2: g_cancel_retype is defined (= 0) in api.cpp"
grep -q 'g_cancel_retype = 0' "$API" \
    || fail "g_cancel_retype initial value not found in $API"
pass

# ── B3: EscInterruptGuard watches for Ctrl+X (0x18) ──────────────────────────
step "B3: EscInterruptGuard detects 0x18 (Ctrl+X) and sets g_cancel_retype"
grep -q '\\x18' "$API" \
    || fail "0x18 (Ctrl+X) byte not found in EscInterruptGuard in $API"
grep -q 'g_cancel_retype = 1' "$API" \
    || fail "g_cancel_retype = 1 not set on Ctrl+X in $API"
pass

# ── B4: Ctrl+X also sets g_interrupted (aborts the curl transfer) ────────────
step "B4: Ctrl+X branch sets both g_cancel_retype and g_interrupted"
# Both assignments must appear close together (within 5 lines) in the
# Ctrl+X branch.
LINE_CR=$(grep -n 'g_cancel_retype = 1' "$API" | head -1 | cut -d: -f1)
LINE_GI=$(awk -v after="$LINE_CR" \
    'NR > after && NR <= after+5 && /g_interrupted.*= 1/ {print NR; exit}' "$API")
[ -n "$LINE_CR" ] || fail "g_cancel_retype = 1 not found in $API"
[ -n "$LINE_GI" ] || fail "g_interrupted = 1 not found within 5 lines of g_cancel_retype = 1"
pass

# ── B5: Ctrl+X branch is distinct from the ESC branch ────────────────────────
step "B5: ESC (0x1b) and Ctrl+X (0x18) are handled as separate if-branches"
# Verify 0x1b and 0x18 appear as separate comparisons in the watcher thread.
ESC_LINE=$(grep -n "'\\\\x1b'" "$API" | head -1 | cut -d: -f1)
CTX_LINE=$(grep -n "'\\\\x18'" "$API" | head -1 | cut -d: -f1)
[ -n "$ESC_LINE" ] || fail "0x1b comparison not found in $API"
[ -n "$CTX_LINE" ] || fail "0x18 comparison not found in $API"
[ "$ESC_LINE" -ne "$CTX_LINE" ] \
    || fail "0x1b and 0x18 must be separate branches, not on the same line"
pass

# ── B6: TurnResult has a cancelledInput field ─────────────────────────────────
step "B6: TurnResult declares a cancelledInput field"
grep -q 'cancelledInput' "$SESS" \
    || fail "cancelledInput field not found in TurnResult in $SESS"
# Verify it's inside the TurnResult struct (before the struct closes).
LINE_TR=$(grep -n 'struct TurnResult' "$SESS" | head -1 | cut -d: -f1)
LINE_TR_END=$(awk -v after="$LINE_TR" 'NR > after && /^};/ {print NR; exit}' "$SESS")
FIELD=$(awk -v s="$LINE_TR" -v e="$LINE_TR_END" \
    'NR>=s && NR<=e && /cancelledInput/' "$SESS")
[ -n "$FIELD" ] || fail "cancelledInput not inside TurnResult struct body"
pass

# ── B7: LocalWorkerFunc sets cancelledInput when g_cancel_retype is set ───────
step "B7: LocalWorkerFunc populates result.cancelledInput from job.userText on Ctrl+X"
grep -q 'result\.cancelledInput = job\.userText' "$SESS" \
    || fail "result.cancelledInput = job.userText not found in $SESS"
# The assignment must be guarded by g_cancel_retype.
LINE_CI=$(grep -n 'result\.cancelledInput = job\.userText' "$SESS" | head -1 | cut -d: -f1)
GUARD=$(awk -v s="$((LINE_CI-5))" -v e="$LINE_CI" \
    'NR>=s && NR<=e && /g_cancel_retype/' "$SESS")
[ -n "$GUARD" ] \
    || fail "g_cancel_retype guard not found within 5 lines before cancelledInput assignment"
pass

# ── B8: LocalWorkerFunc clears g_cancel_retype after recording it ─────────────
step "B8: LocalWorkerFunc clears g_cancel_retype = 0 after setting cancelledInput"
LINE_CI=$(grep -n 'result\.cancelledInput = job\.userText' "$SESS" | head -1 | cut -d: -f1)
CLEAR=$(awk -v after="$LINE_CI" \
    'NR > after && NR <= after+3 && /g_cancel_retype = 0/' "$SESS")
[ -n "$CLEAR" ] \
    || fail "g_cancel_retype = 0 not cleared within 3 lines of cancelledInput assignment"
pass

# ── B9: repl::RestoreInput is declared in repl.h ─────────────────────────────
step "B9: repl::RestoreInput(const std::string&) is declared in repl.h"
grep -q 'RestoreInput' "$REPLH" \
    || fail "RestoreInput not declared in $REPLH"
grep -q 'void RestoreInput' "$REPLH" \
    || fail "RestoreInput must return void"
pass

# ── B10: repl::RestoreInput is implemented in repl.cpp ───────────────────────
step "B10: RestoreInput is implemented using rl_stuff_char in repl.cpp"
grep -q 'void.*repl::RestoreInput\|RestoreInput.*text' "$REPL" \
    || fail "RestoreInput implementation not found in $REPL"
grep -q 'rl_stuff_char' "$REPL" \
    || fail "RestoreInput must use rl_stuff_char to seed the edit buffer"
pass

# ── B11: RestoreInput pushes bytes in reverse (LIFO queue) ───────────────────
step "B11: RestoreInput iterates in reverse order (rl_stuff_char is LIFO)"
# The reverse loop: i = size()-1 down to 0.
grep -q 'size() - 1; i >= 0; --i\|static_cast.*size.*- 1' "$REPL" \
    || fail "RestoreInput does not iterate in reverse — rl_stuff_char queue will be backwards"
pass

# ── B12: InteractiveLoop calls RestoreInput when cancelledInput is non-empty ──
step "B12: InteractiveLoop calls repl::RestoreInput on non-empty cancelledInput"
grep -q 'repl::RestoreInput' "$SESS" \
    || fail "repl::RestoreInput not called in $SESS"
# Must appear inside the !result.ok failure path.
LINE_FAIL=$(grep -n '// messages\[\] was already rolled back' "$SESS" | head -1 | cut -d: -f1)
LINE_RESTORE=$(grep -n 'repl::RestoreInput' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_FAIL"    ] || fail "rollback comment not found in $SESS"
[ -n "$LINE_RESTORE" ] || fail "repl::RestoreInput not found in $SESS"
[ "$LINE_RESTORE" -gt "$LINE_FAIL" ] \
    || fail "RestoreInput (L$LINE_RESTORE) must appear after the failure-branch rollback (L$LINE_FAIL)"
pass

# ── B13: RestoreInput is guarded — only called when cancelledInput non-empty ──
step "B13: RestoreInput call is guarded by !result.cancelledInput.empty()"
grep -q '!result\.cancelledInput\.empty()' "$SESS" \
    || fail "!result.cancelledInput.empty() guard missing — RestoreInput called unconditionally"
pass

# ── B14: status bar shows ctrl+x hint while worker is active ─────────────────
step "B14: status bar is updated with ctrl+x: amend hint after job is enqueued"
grep -q 'ctrl+x: amend\|ctrl+x:amend' "$SESS" \
    || fail "ctrl+x: amend hint not found in $SESS"
# The hint must appear after notify_one() (i.e. after the job is dispatched).
LINE_NOTIFY=$(grep -n 'worker\.fJobCv\.notify_one()' "$SESS" | head -1 | cut -d: -f1)
LINE_HINT=$(grep -n 'ctrl+x' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_NOTIFY" ] || fail "fJobCv.notify_one() not found in $SESS"
[ -n "$LINE_HINT"   ] || fail "ctrl+x hint not found in $SESS"
[ "$LINE_HINT" -gt "$LINE_NOTIFY" ] \
    || fail "ctrl+x hint (L$LINE_HINT) must appear after notify_one() (L$LINE_NOTIFY)"
pass

# ── B15: status bar is restored after the wait (hint removed) ────────────────
step "B15: status bar is reset to compose_status() after fDisplayCv.wait returns"
LINE_WAIT=$(grep -n 'fDisplayCv\.wait' "$SESS" | head -1 | cut -d: -f1)
LINE_RESTORE_STATUS=$(awk -v after="$LINE_WAIT" \
    'NR > after && NR <= after+8 && /tui::SetStatusBar.*compose_status/ {print NR; exit}' "$SESS")
[ -n "$LINE_WAIT"           ] || fail "fDisplayCv.wait not found in $SESS"
[ -n "$LINE_RESTORE_STATUS" ] \
    || fail "tui::SetStatusBar(compose_status()) not found within 8 lines after fDisplayCv.wait"
pass

# ════════════════════════════════════════════════════════════════════════════
# v1.4.1 — BFS snapshot background refresh
# ════════════════════════════════════════════════════════════════════════════

# ── B16: config::ReloadBfsSummaries is declared in config.h ──────────────────
step "B16: config::ReloadBfsSummaries() is declared in config.h"
grep -q 'ReloadBfsSummaries' "$CFGH" \
    || fail "ReloadBfsSummaries not declared in $CFGH"
pass

# ── B17: config::RefreshSummarySnapshot is declared in config.h ──────────────
step "B17: config::RefreshSummarySnapshot(const std::vector<std::string>&) declared in config.h"
grep -q 'RefreshSummarySnapshot' "$CFGH" \
    || fail "RefreshSummarySnapshot not declared in $CFGH"
grep -q 'vector.*string.*paths\|RefreshSummarySnapshot.*vector' "$CFGH" \
    || fail "RefreshSummarySnapshot must accept a vector<string> parameter"
pass

# ── B18: ReloadBfsSummaries resets g_bfs_loaded before re-scanning ───────────
step "B18: ReloadBfsSummaries clears g_bfs_loaded and g_bfs_snapshot before re-scan"
grep -q 'g_bfs_loaded.*false\|g_bfs_loaded = false' "$CFG" \
    || fail "g_bfs_loaded = false not found in $CFG (needed by ReloadBfsSummaries)"
grep -q 'g_bfs_snapshot\.clear()' "$CFG" \
    || fail "g_bfs_snapshot.clear() not found in $CFG (needed by ReloadBfsSummaries)"
pass

# ── B19: RefreshSummarySnapshot replaces existing entries by prefix ───────────
step "B19: RefreshSummarySnapshot removes old entry before inserting new one"
# The implementation must scan g_bfs_snapshot line-by-line and skip
# lines that start with the path prefix.
grep -q 'rfind.*prefix.*!= 0\|rfind(prefix' "$CFG" \
    || fail "prefix-based line removal (rfind) not found in RefreshSummarySnapshot in $CFG"
pass

# ── B20: RefreshSummarySnapshot only inserts non-empty valid UTF-8 values ─────
step "B20: RefreshSummarySnapshot guards insertion with !value.empty() and IsValidUtf8"
# Both guards must appear after the "remove old entry" loop.
LINE_RFIND=$(grep -n 'rfind.*prefix' "$CFG" | head -1 | cut -d: -f1)
LINE_EMPTY=$(awk -v after="$LINE_RFIND" \
    'NR > after && /!value\.empty\(\)/ {print NR; exit}' "$CFG")
LINE_UTF8=$(awk -v after="$LINE_RFIND" \
    'NR > after && /IsValidUtf8/ {print NR; exit}' "$CFG")
[ -n "$LINE_EMPTY" ] || fail "!value.empty() guard not found after rfind loop in $CFG"
[ -n "$LINE_UTF8"  ] || fail "IsValidUtf8 guard not found after rfind loop in $CFG"
pass

# ── B21: TurnResult has a writtenSummaryPaths field ──────────────────────────
step "B21: TurnResult declares a writtenSummaryPaths field"
grep -q 'writtenSummaryPaths' "$SESS" \
    || fail "writtenSummaryPaths not found in $SESS"
LINE_TR=$(grep -n 'struct TurnResult' "$SESS" | head -1 | cut -d: -f1)
LINE_TR_END=$(awk -v after="$LINE_TR" 'NR > after && /^};/ {print NR; exit}' "$SESS")
FIELD=$(awk -v s="$LINE_TR" -v e="$LINE_TR_END" \
    'NR>=s && NR<=e && /writtenSummaryPaths/' "$SESS")
[ -n "$FIELD" ] || fail "writtenSummaryPaths not inside TurnResult struct body"
pass

# ── B22: api::DrainWrittenSummaryPaths is declared in api.h ──────────────────
step "B22: api::DrainWrittenSummaryPaths() is declared in api.h"
grep -q 'DrainWrittenSummaryPaths' "$APIH" \
    || fail "DrainWrittenSummaryPaths not declared in $APIH"
grep -q 'std::vector.*string.*DrainWrittenSummaryPaths\|DrainWrittenSummaryPaths.*vector' "$APIH" \
    || fail "DrainWrittenSummaryPaths must return vector<string>"
pass

# ── B23: api.cpp accumulates paths in tl_written_summary_paths ───────────────
step "B23: api.cpp has a thread_local accumulator for written summary paths"
grep -q 'tl_written_summary_paths\|thread_local.*written_summary' "$API" \
    || fail "thread-local accumulator for written summary paths not found in $API"
pass

# ── B24: accumulator is populated only for WriteAttr + claude:summary ─────────
step "B24: accumulator only records WriteAttr calls where attr name == claude:summary"
grep -q '"claude:summary"' "$API" \
    || fail '"claude:summary" check not found in $API WriteAttr tracking block'
# Both the tool-name check and attribute-name check must be present.
grep -q 'tname.*WriteAttr\|WriteAttr.*tname' "$API" \
    || fail "WriteAttr tool-name check not found near summary-path accumulator in $API"
pass

# ── B25: DrainWrittenSummaryPaths swaps (clears) the accumulator ─────────────
step "B25: DrainWrittenSummaryPaths uses swap to clear the accumulator"
grep -q '\.swap\(tl_written_summary_paths\)\|tl_written_summary_paths.*swap\|out\.swap' "$API" \
    || fail "swap-drain of tl_written_summary_paths not found in DrainWrittenSummaryPaths"
pass

# ── B26: LocalWorkerFunc calls DrainWrittenSummaryPaths after SendWithTools ───
step "B26: LocalWorkerFunc calls api::DrainWrittenSummaryPaths() and stores result"
grep -q 'DrainWrittenSummaryPaths' "$SESS" \
    || fail "api::DrainWrittenSummaryPaths not called in $SESS"
# Must appear after SendWithTools.
LINE_SEND=$(grep -n 'api::SendWithTools' "$SESS" | head -1 | cut -d: -f1)
LINE_DRAIN=$(grep -n 'DrainWrittenSummaryPaths' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_SEND"  ] || fail "api::SendWithTools not found in $SESS"
[ -n "$LINE_DRAIN" ] || fail "DrainWrittenSummaryPaths not found in $SESS"
[ "$LINE_DRAIN" -gt "$LINE_SEND" ] \
    || fail "DrainWrittenSummaryPaths (L$LINE_DRAIN) must follow SendWithTools (L$LINE_SEND)"
pass

# ── B27: InteractiveLoop calls RefreshSummarySnapshot after each turn ─────────
step "B27: InteractiveLoop calls config::RefreshSummarySnapshot with writtenSummaryPaths"
grep -q 'config::RefreshSummarySnapshot' "$SESS" \
    || fail "config::RefreshSummarySnapshot not called in $SESS"
grep -q 'result\.writtenSummaryPaths' "$SESS" \
    || fail "result.writtenSummaryPaths not passed to RefreshSummarySnapshot in $SESS"
# Verify it is guarded (only called when non-empty).
grep -q '!result\.writtenSummaryPaths\.empty()' "$SESS" \
    || fail "RefreshSummarySnapshot call is not guarded by !result.writtenSummaryPaths.empty()"
pass

# ── B28: /compact calls config::ReloadBfsSummaries after SaveHistory ──────────
step "B28: /compact calls config::ReloadBfsSummaries() after config::SaveHistory"
grep -q 'config::ReloadBfsSummaries' "$CMDS" \
    || fail "config::ReloadBfsSummaries not called in $CMDS (/compact)"
LINE_SAVE=$(grep -n 'config::SaveHistory' "$CMDS" | head -1 | cut -d: -f1)
LINE_RELOAD=$(grep -n 'config::ReloadBfsSummaries' "$CMDS" | head -1 | cut -d: -f1)
[ -n "$LINE_SAVE"   ] || fail "config::SaveHistory not found in $CMDS"
[ -n "$LINE_RELOAD" ] || fail "config::ReloadBfsSummaries not found in $CMDS"
[ "$LINE_RELOAD" -gt "$LINE_SAVE" ] \
    || fail "ReloadBfsSummaries (L$LINE_RELOAD) must appear after SaveHistory (L$LINE_SAVE)"
pass

# ── B29: non-Haiku stubs for ReloadBfsSummaries and RefreshSummarySnapshot ────
step "B29: non-Haiku no-op stubs exist for ReloadBfsSummaries and RefreshSummarySnapshot"
# Count the #else stubs (empty body).
RELOAD_STUBS=$(grep -c 'void ReloadBfsSummaries().*{}' "$CFG" || true)
REFRESH_STUBS=$(grep -c 'void RefreshSummarySnapshot.*{}' "$CFG" || true)
[ "$RELOAD_STUBS"  -ge 1 ] || fail "non-Haiku stub for ReloadBfsSummaries not found in $CFG"
[ "$REFRESH_STUBS" -ge 1 ] || fail "non-Haiku stub for RefreshSummarySnapshot not found in $CFG"
pass

# ── B30: binary still exits cleanly (no regression from new code) ─────────────
step "B30: binary exits 0 for --help and non-zero without auth (smoke test)"
[ -x "$BIN" ] || fail "$BIN not executable"
"$BIN" --help </dev/null >/dev/null 2>&1 || fail "--help exited non-zero"
home=$(mktemp -d /tmp/claude-async-test.XXXXXX)
set +e
timeout 10 env -u ANTHROPIC_API_KEY HOME="$home" "$BIN" "hi" </dev/null >/dev/null 2>&1
rc=$?
set -e
rm -rf "$home"
[ "$rc" -ne 124 ] || fail "binary timed out (possible thread hang)"
[ "$rc" -lt 128 ] || fail "binary crashed (exit $rc — possible signal)"
[ "$rc" -ne 0   ] || fail "expected non-zero exit without auth"
pass

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
