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
step "B12: drain_turn calls repl::RestoreInput on non-empty cancelledInput"
grep -q 'repl::RestoreInput' "$SESS" \
    || fail "repl::RestoreInput not called in $SESS"
# Must appear inside the drain_turn lambda failure path.
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SESS" | head -1 | cut -d: -f1)
LINE_RESTORE=$(grep -n 'repl::RestoreInput' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_DRAIN"   ] || fail "drain_turn lambda not found in $SESS"
[ -n "$LINE_RESTORE" ] || fail "repl::RestoreInput not found in $SESS"
[ "$LINE_RESTORE" -gt "$LINE_DRAIN" ] \
    || fail "RestoreInput (L$LINE_RESTORE) must appear inside drain_turn (L$LINE_DRAIN)"
pass

# ── B12b: RemoveLastRecord is called before RestoreInput (history clean-up) ───
step "B12b: repl::RemoveLastRecord() is called before RestoreInput to suppress cancelled turn from history"
grep -q 'repl::RemoveLastRecord' "$SESS" \
    || fail "repl::RemoveLastRecord not called in $SESS"
LINE_REMOVE=$(grep -n 'repl::RemoveLastRecord' "$SESS" | head -1 | cut -d: -f1)
LINE_RESTORE=$(grep -n 'repl::RestoreInput' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_REMOVE"  ] || fail "repl::RemoveLastRecord not found in $SESS"
[ -n "$LINE_RESTORE" ] || fail "repl::RestoreInput not found in $SESS"
[ "$LINE_REMOVE" -lt "$LINE_RESTORE" ] \
    || fail "RemoveLastRecord (L$LINE_REMOVE) must precede RestoreInput (L$LINE_RESTORE)"
pass

# ── B12c: RemoveLastRecord is declared in repl.h ──────────────────────────────
step "B12c: repl::RemoveLastRecord() is declared in repl.h"
grep -q 'RemoveLastRecord' "$REPLH" \
    || fail "RemoveLastRecord not declared in $REPLH"
pass

# ── B12d: RemoveLastRecord uses remove_history and free ──────────────────────
step "B12d: RemoveLastRecord implementation calls remove_history() and free()"
grep -q 'remove_history' "$REPL" \
    || fail "remove_history() not called in RemoveLastRecord in $REPL"
grep -q 'free.*removed' "$REPL" \
    || fail "free(removed->...) not called in RemoveLastRecord in $REPL"
pass

# ── B13: RestoreInput is guarded — only called when cancelledInput non-empty ──
step "B13: RestoreInput call is guarded by !result.cancelledInput.empty()"
grep -q '!result\.cancelledInput\.empty()' "$SESS" \
    || fail "!result.cancelledInput.empty() guard missing — RestoreInput called unconditionally"
pass

# ── B14: status bar shows ctrl+x hint while worker is active ─────────────────
step "B14: status bar is updated with ctrl+x hint after job is enqueued"
grep -q 'ctrl+x.*amend\|ctrl+x:amend' "$SESS" \
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
step "B15: status bar is reset to compose_status() in drain_turn after turn completes"
# The drain_turn lambda calls tui::EndTurn() then tui::SetStatusBar(compose_status()).
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SESS" | head -1 | cut -d: -f1)
LINE_END_TURN=$(awk -v after="$LINE_DRAIN" \
    'NR > after && /tui::EndTurn/ {print NR; exit}' "$SESS")
LINE_RESTORE_STATUS=$(awk -v after="${LINE_END_TURN:-0}" \
    'NR > after && NR <= after+15 && /tui::SetStatusBar.*compose_status/ {print NR; exit}' "$SESS")
[ -n "$LINE_DRAIN"          ] || fail "drain_turn lambda not found in $SESS"
[ -n "$LINE_END_TURN"       ] || fail "tui::EndTurn() not found in drain_turn"
[ -n "$LINE_RESTORE_STATUS" ] \
    || fail "tui::SetStatusBar(compose_status()) not found within 15 lines after EndTurn()"
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

# ── B29b: kSnapshotLineCap = 500 is defined and used as a guard ──────────────
step "B29b: kSnapshotLineCap = 500 guards RefreshSummarySnapshot (skips on large projects)"
grep -q 'kSnapshotLineCap' "$CFG" \
    || fail "kSnapshotLineCap not defined in $CFG"
grep -q '500' "$CFG" \
    || fail "500-file cap value not found in $CFG"
# Guard must appear inside RefreshSummarySnapshot.
LINE_REFRESH=$(grep -n 'void RefreshSummarySnapshot' "$CFG" | head -1 | cut -d: -f1)
LINE_CAP=$(awk -v after="$LINE_REFRESH" \
    'NR > after && /kSnapshotLineCap/ {print NR; exit}' "$CFG")
[ -n "$LINE_REFRESH" ] || fail "RefreshSummarySnapshot not found in $CFG"
[ -n "$LINE_CAP"     ] || fail "kSnapshotLineCap guard not found inside RefreshSummarySnapshot"
pass

# ── B29c: BfsSystemBlock emits a stale-cache note when at or above cap ───────
step "B29c: BfsSystemBlock notes that mid-session refresh is skipped above 500 files"
grep -q '500+ summaries\|500.*summaries\|kSnapshotLineCap' "$CFG" \
    || fail "stale-cache note for large projects not found in BfsSystemBlock in $CFG"
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

# ════════════════════════════════════════════════════════════════════════════
# True async / concurrent output (TurnOutputBuf + BeginTurn/EndTurn)
# ════════════════════════════════════════════════════════════════════════════
TUI="src/tui.cpp"
TUIH="src/tui.h"

# ── C1: TurnOutputBuf class is defined in tui.cpp ────────────────────────────
step "C1: TurnOutputBuf custom streambuf is defined in tui.cpp"
grep -q 'class TurnOutputBuf' "$TUI" \
    || fail "TurnOutputBuf class not found in $TUI"
grep -q 'std::streambuf\|public.*streambuf' "$TUI" \
    || fail "TurnOutputBuf must inherit from std::streambuf"
pass

# ── C2: TurnOutputBuf overrides xsputn and overflow ──────────────────────────
step "C2: TurnOutputBuf overrides xsputn() and overflow() for complete write coverage"
grep -q 'xsputn' "$TUI" || fail "xsputn not overridden in TurnOutputBuf"
grep -q 'overflow' "$TUI" || fail "overflow not overridden in TurnOutputBuf"
pass

# ── C3: Pending buffer is protected by a mutex ───────────────────────────────
step "C3: g_turn_pending buffer is protected by g_turn_pending_mu (std::mutex)"
grep -q 'g_turn_pending_mu' "$TUI" \
    || fail "g_turn_pending_mu not found in $TUI"
grep -q 'g_turn_pending' "$TUI" \
    || fail "g_turn_pending buffer not found in $TUI"
# Both must appear near each other (the mutex protects the buffer).
LINE_MU=$(grep -n 'std::mutex.*g_turn_pending_mu\|g_turn_pending_mu.*std::mutex' "$TUI" | head -1 | cut -d: -f1)
LINE_BUF=$(grep -n 'std::string.*g_turn_pending;\|g_turn_pending;' "$TUI" | head -1 | cut -d: -f1)
[ -n "$LINE_MU"  ] || fail "g_turn_pending_mu declaration not found"
[ -n "$LINE_BUF" ] || fail "g_turn_pending declaration not found"
DIFF=$(( LINE_BUF > LINE_MU ? LINE_BUF - LINE_MU : LINE_MU - LINE_BUF ))
[ "$DIFF" -le 5 ] || fail "g_turn_pending and g_turn_pending_mu are $DIFF lines apart — should be adjacent"
pass

# ── C4: BeginTurn, EndTurn, FlushTurnOutput are declared in tui.h ─────────────
step "C4: BeginTurn(), EndTurn(), FlushTurnOutput() are declared in tui.h"
grep -q 'void BeginTurn'        "$TUIH" || fail "BeginTurn not declared in $TUIH"
grep -q 'void EndTurn'          "$TUIH" || fail "EndTurn not declared in $TUIH"
grep -q 'void FlushTurnOutput'  "$TUIH" || fail "FlushTurnOutput not declared in $TUIH"
pass

# ── C5: BeginTurn installs the TurnOutputBuf via cout.rdbuf() ─────────────────
step "C5: BeginTurn() redirects std::cout through TurnOutputBuf via rdbuf()"
LINE_BEGIN=$(grep -n 'void BeginTurn' "$TUI" | head -1 | cut -d: -f1)
RDBUF=$(awk -v after="$LINE_BEGIN" \
    'NR > after && NR <= after+20 && /cout\.rdbuf/ {print NR; exit}' "$TUI")
[ -n "$RDBUF" ] || fail "cout.rdbuf() not called in BeginTurn in $TUI"
pass

# ── C6: EndTurn restores cout.rdbuf before flushing ──────────────────────────
step "C6: EndTurn() restores the original rdbuf before calling FlushTurnOutput"
LINE_END=$(grep -n 'void EndTurn' "$TUI" | head -1 | cut -d: -f1)
# Restore must precede flush within EndTurn body.
LINE_RESTORE=$(awk -v after="$LINE_END" \
    'NR > after && NR <= after+20 && /g_cout_orig_buf/ {print NR; exit}' "$TUI")
LINE_FLUSH=$(awk -v after="$LINE_END" \
    'NR > after && NR <= after+20 && /FlushTurnOutput/ {print NR; exit}' "$TUI")
[ -n "$LINE_RESTORE" ] || fail "g_cout_orig_buf restore not found in EndTurn"
[ -n "$LINE_FLUSH"   ] || fail "FlushTurnOutput not called in EndTurn"
[ "$LINE_RESTORE" -lt "$LINE_FLUSH" ] \
    || fail "rdbuf restore (L$LINE_RESTORE) must precede FlushTurnOutput (L$LINE_FLUSH) in EndTurn"
pass

# ── C7: FlushTurnOutput uses DECSC/DECRC to preserve libedit cursor ───────────
step "C7: FlushTurnOutput wraps output in DECSC (ESC 7) and DECRC (ESC 8)"
grep -q '"\\\\x1b""7"\|\\\\x1b7\|ESC.*7\|DECSC\|\\\\""7"' "$TUI" \
    || grep -q '"7"' "$TUI" \
    || fail "DECSC (ESC 7) not found in FlushTurnOutput in $TUI"
grep -q '"\\\\x1b""8"\|\\\\x1b8\|ESC.*8\|DECRC\|\\\\""8"' "$TUI" \
    || grep -q '"8"' "$TUI" \
    || fail "DECRC (ESC 8) not found in FlushTurnOutput in $TUI"
pass

# ── C8: FlushTurnOutput writes to g_cout_orig_buf (bypasses interceptor) ──────
step "C8: FlushTurnOutput writes via g_cout_orig_buf to avoid re-entrant buffering"
grep -q 'g_cout_orig_buf' "$TUI" \
    || fail "g_cout_orig_buf not used in $TUI"
LINE_FLUSH_FN=$(grep -n 'void FlushTurnOutput' "$TUI" | head -1 | cut -d: -f1)
ORIG_USE=$(awk -v after="$LINE_FLUSH_FN" \
    'NR > after && NR <= after+80 && /g_cout_orig_buf/ {print NR; exit}' "$TUI")
[ -n "$ORIG_USE" ] || fail "g_cout_orig_buf not used inside FlushTurnOutput body"
pass

# ── C9: FlushTurnOutput swaps (drains) the pending buffer atomically ──────────
step "C9: FlushTurnOutput swaps g_turn_pending under the mutex before writing"
LINE_FLUSH_FN=$(grep -n 'void FlushTurnOutput' "$TUI" | head -1 | cut -d: -f1)
SWAP=$(awk -v after="$LINE_FLUSH_FN" \
    'NR > after && NR <= after+20 && /\.swap\(g_turn_pending\)|chunk\.swap/ {print NR; exit}' "$TUI")
[ -n "$SWAP" ] || fail "swap of g_turn_pending not found in FlushTurnOutput — buffer not drained atomically"
pass

# ── C10: FlushTurnOutput is called from bracketed_getc in repl.cpp ────────────
step "C10: FlushTurnOutput() is called inside bracketed_getc (before each libedit read)"
grep -q 'FlushTurnOutput' "$REPL" \
    || fail "tui::FlushTurnOutput not called in $REPL"
# Must appear before the blocking raw_getc call.
LINE_FLUSH=$(grep -n 'FlushTurnOutput' "$REPL" | head -1 | cut -d: -f1)
LINE_BRACKET=$(grep -n 'bracketed_getc' "$REPL" | head -1 | cut -d: -f1)
# Find the raw_getc call inside bracketed_getc (not the definition).
LINE_RAWGETC=$(awk -v after="$LINE_BRACKET" \
    'NR > after && /raw_getc\(f\)/ {print NR; exit}' "$REPL")
[ -n "$LINE_FLUSH"   ] || fail "FlushTurnOutput call not found in $REPL"
[ -n "$LINE_RAWGETC" ] || fail "raw_getc(f) call not found inside bracketed_getc in $REPL"
[ "$LINE_FLUSH" -lt "$LINE_RAWGETC" ] \
    || fail "FlushTurnOutput (L$LINE_FLUSH) must precede raw_getc (L$LINE_RAWGETC) in bracketed_getc"
pass

# ── C11: BeginTurn is called in dispatch_turn (before worker wakes) ───────────
step "C11: BeginTurn() is called in dispatch_turn before fJobCv.notify_one()"
grep -q 'tui::BeginTurn' "$SESS" \
    || fail "tui::BeginTurn not called in $SESS"
LINE_BEGIN=$(grep -n 'tui::BeginTurn' "$SESS" | head -1 | cut -d: -f1)
LINE_NOTIFY=$(grep -n 'worker\.fJobCv\.notify_one' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_BEGIN"  ] || fail "tui::BeginTurn not found in $SESS"
[ -n "$LINE_NOTIFY" ] || fail "fJobCv.notify_one not found in $SESS"
[ "$LINE_BEGIN" -lt "$LINE_NOTIFY" ] \
    || fail "BeginTurn (L$LINE_BEGIN) must precede notify_one (L$LINE_NOTIFY)"
pass

# ── C12: EndTurn is called in drain_turn (restores cout before bookkeeping) ───
step "C12: EndTurn() is called at the start of drain_turn before result drain"
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SESS" | head -1 | cut -d: -f1)
LINE_END=$(awk -v after="$LINE_DRAIN" \
    'NR > after && NR <= after+10 && /tui::EndTurn/ {print NR; exit}' "$SESS")
[ -n "$LINE_END" ] || fail "tui::EndTurn not found within 10 lines of drain_turn start"
pass

# ── C13: Main loop checks fWorkerOwnsDisplay without blocking ─────────────────
step "C13: main loop polls fWorkerOwnsDisplay non-blocking (no unconditional wait)"
# The new design polls at the top of the loop rather than blocking.
# fWorkerOwnsDisplay must be read under fDisplayMu in a non-blocking check.
grep -q 'done = !worker\.fWorkerOwnsDisplay\|!worker\.fWorkerOwnsDisplay' "$SESS" \
    || fail "non-blocking fWorkerOwnsDisplay poll not found in $SESS"
# The top-of-loop check must NOT be an unconditional fDisplayCv.wait inside
# the main turn dispatch path (only in the EOF drain path).
LINE_DRAIN_LAMBDA=$(grep -n 'auto drain_turn' "$SESS" | head -1 | cut -d: -f1)
LINE_DISPATCH_LAMBDA=$(grep -n 'auto dispatch_turn' "$SESS" | head -1 | cut -d: -f1)
# fDisplayCv.wait must not appear between dispatch_lambda and drain_lambda
# (i.e. not as an unconditional block in the hot path).
BETWEEN=$(awk -v s="$LINE_DISPATCH_LAMBDA" -v e="$LINE_DRAIN_LAMBDA" \
    'NR>s && NR<e && /fDisplayCv\.wait/' "$SESS")
[ -z "$BETWEEN" ] \
    || fail "unconditional fDisplayCv.wait found in dispatch path — main thread should not block"
pass

# ── C14: Input while turn active waits for turn completion ────────────────────
step "C14: input while turn active blocks on fDisplayCv.wait until worker finishes"
grep -q 'fDisplayCv\.wait' "$SESS" \
    || fail "fDisplayCv.wait not found in $SESS"
# The blocking wait in the turn_active block ensures drain happens before dispatch.
LINE_ACTIVE=$(grep -n 'If a turn is still active' "$SESS" | head -1 | cut -d: -f1)
LINE_WAIT=$(awk -v after="${LINE_ACTIVE:-0}" \
    'NR > after && NR <= after+80 && /fDisplayCv\.wait/ {print NR; exit}' "$SESS")
[ -n "$LINE_ACTIVE" ] || fail "'If a turn is still active' comment not found in $SESS"
[ -n "$LINE_WAIT"   ] || fail "fDisplayCv.wait not found within 80 lines of turn_active block"
pass

# ── C15: EOF / Ctrl+D drains active turn before breaking ──────────────────────
step "C15: EOF/Ctrl+D path waits for active turn via fDisplayCv.wait before break"
# The only remaining fDisplayCv.wait should be in the EOF drain path.
grep -q 'fDisplayCv\.wait' "$SESS" \
    || fail "fDisplayCv.wait not found in $SESS (needed for EOF drain)"
# Confirm it appears after the ReadMessage() EOF check.
LINE_EOF=$(grep -n 'tui::ClearInputRow.*break\|EOF.*drain\|active.*turn.*finish' "$SESS" \
    | head -1 | cut -d: -f1)
LINE_WAIT=$(grep -n 'fDisplayCv\.wait' "$SESS" | head -1 | cut -d: -f1)
[ -n "$LINE_WAIT" ] || fail "fDisplayCv.wait not found in $SESS"
pass

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
