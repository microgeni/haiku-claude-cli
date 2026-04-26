#!/usr/bin/env bash
# LocalWorker async-dispatch tests — verify the structural invariants of the
# background-thread turn-dispatch introduced in session.cpp.
#
# These are static source-code tests (grep/awk on session.cpp) plus a handful
# of binary-level smoke tests.  They do NOT require a live Claude API key.
#
# Tests A1–A25.  Run from the project root after a successful build:
#
#   bash tests/local_worker_test.sh
#
# All tests are expected to PASS without any network access.

set -euo pipefail

BIN="./build/claude"
SRC="src/session.cpp"

PASS=0
FAIL=0
SKIP=0

step() { echo; echo "--- $* ---"; }
pass() { echo "PASS"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }
skip() { echo "SKIP: $*"; SKIP=$((SKIP+1)); }

echo "=== LocalWorker Async-Dispatch Tests ==="

# ── A1: Core structs are present ──────────────────────────────────────────────
step "A1: TurnJob, TurnResult, and LocalWorker structs are defined in session.cpp"
grep -q 'struct TurnJob'     "$SRC" || { fail "struct TurnJob not found in $SRC";     }
grep -q 'struct TurnResult'  "$SRC" || { fail "struct TurnResult not found in $SRC";  }
grep -q 'struct LocalWorker' "$SRC" || { fail "struct LocalWorker not found in $SRC"; }
pass

# ── A2: LocalWorkerFunc is a static file-scoped function ──────────────────────
step "A2: LocalWorkerFunc is declared static (not exposed as a public symbol)"
grep -q 'static void LocalWorkerFunc' "$SRC" \
    || fail "LocalWorkerFunc must be declared 'static void LocalWorkerFunc' in $SRC"
pass

# ── A3: Worker thread is started in InteractiveLoop ───────────────────────────
step "A3: std::thread is created from LocalWorkerFunc inside InteractiveLoop"
# Verify both the thread creation and the function reference appear in the
# section after the InteractiveLoop signature.
LINE_LOOP=$(grep -n 'int InteractiveLoop' "$SRC" | head -1 | cut -d: -f1)
LINE_THREAD=$(grep -n 'std::thread.*LocalWorkerFunc' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_LOOP"   ] || fail "InteractiveLoop not found in $SRC"
[ -n "$LINE_THREAD" ] || fail "std::thread(LocalWorkerFunc,...) not found in $SRC"
[ "$LINE_THREAD" -gt "$LINE_LOOP" ] \
    || fail "thread creation (L$LINE_THREAD) must appear after InteractiveLoop (L$LINE_LOOP)"
pass

# ── A4: WorkerGuard RAII sets fShutdown and joins the thread ──────────────────
step "A4: WorkerGuard destructor sets fShutdown=true and joins the worker thread"
grep -q 'struct WorkerGuard' "$SRC" \
    || fail "WorkerGuard struct not found in $SRC"
# fShutdown = true must appear inside the WorkerGuard destructor body.
LINE_GUARD=$(grep -n 'struct WorkerGuard' "$SRC" | head -1 | cut -d: -f1)
LINE_SHUTDOWN=$(awk -v after="$LINE_GUARD" 'NR > after && /fShutdown = true/ {print NR; exit}' "$SRC")
[ -n "$LINE_SHUTDOWN" ] \
    || fail "fShutdown = true not found after WorkerGuard definition in $SRC"
# join() must appear within 10 lines of fShutdown = true.
LINE_JOIN=$(awk -v after="$LINE_SHUTDOWN" 'NR > after && /\.join\(\)/ {print NR; exit}' "$SRC")
[ -n "$LINE_JOIN" ] || fail "thread join() not found after fShutdown = true in $SRC"
DIFF=$(( LINE_JOIN - LINE_SHUTDOWN ))
[ "$DIFF" -le 10 ] \
    || fail "join() (L$LINE_JOIN) is $DIFF lines after fShutdown=true (L$LINE_SHUTDOWN) — should be inside WorkerGuard dtor"
pass

# ── A5: Display lock is set before job lock when enqueuing ────────────────────
# Invariant: fWorkerOwnsDisplay = true must be set (under fDisplayMu) BEFORE
# fPendingJob is assigned and fJobCv is notified.  This prevents a race where
# the worker could complete and clear fWorkerOwnsDisplay before the main thread
# even sets it.
step "A5: fWorkerOwnsDisplay=true is set before fPendingJob is assigned (no race)"
LINE_OWN=$(grep -n 'fWorkerOwnsDisplay = true' "$SRC" | head -1 | cut -d: -f1)
LINE_JOB=$(grep -n 'fPendingJob = std::move' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_OWN" ] || fail "fWorkerOwnsDisplay = true not found in $SRC"
[ -n "$LINE_JOB" ] || fail "fPendingJob = std::move not found in $SRC"
[ "$LINE_OWN" -lt "$LINE_JOB" ] \
    || fail "fWorkerOwnsDisplay=true (L$LINE_OWN) must precede fPendingJob assignment (L$LINE_JOB)"
pass

# ── A6: Main thread waits on fDisplayCv for !fWorkerOwnsDisplay ───────────────
step "A6: fDisplayCv.wait used for Ctrl+D/EOF drain path (turn completion wait)"
# The non-blocking design polls fWorkerOwnsDisplay at the top of the loop;
# the only remaining fDisplayCv.wait is in the EOF/Ctrl+D drain path.
grep -q 'fDisplayCv.wait' "$SRC" \
    || fail "fDisplayCv.wait not found in $SRC (needed for EOF drain)"
grep -q 'fWorkerOwnsDisplay' "$SRC" \
    || fail "fWorkerOwnsDisplay not referenced in $SRC"
pass

# ── A7: Worker releases display under fDisplayMu, then notifies fDisplayCv ────
step "A7: worker clears fWorkerOwnsDisplay under fDisplayMu before notify_all"
# In LocalWorkerFunc: the lock_guard(fDisplayMu) block must come before
# fDisplayCv.notify_all().
LINE_FUNC=$(grep -n 'static void LocalWorkerFunc' "$SRC" | head -1 | cut -d: -f1)
LINE_DISP_LOCK=$(awk -v after="$LINE_FUNC" \
    'NR > after && /lock_guard.*fDisplayMu/ {print NR; exit}' "$SRC")
LINE_NOTIFY_ALL=$(awk -v after="$LINE_FUNC" \
    'NR > after && /fDisplayCv\.notify_all/ {print NR; exit}' "$SRC")
[ -n "$LINE_DISP_LOCK"  ] || fail "lock_guard(fDisplayMu) not found in LocalWorkerFunc"
[ -n "$LINE_NOTIFY_ALL" ] || fail "fDisplayCv.notify_all() not found in LocalWorkerFunc"
[ "$LINE_DISP_LOCK" -lt "$LINE_NOTIFY_ALL" ] \
    || fail "fDisplayMu lock (L$LINE_DISP_LOCK) must precede notify_all (L$LINE_NOTIFY_ALL)"
# Also verify the false assignment is inside the lock block (within 3 lines).
LINE_CLEAR=$(awk -v after="$LINE_DISP_LOCK" \
    'NR > after && NR <= after+4 && /fWorkerOwnsDisplay = false/ {print NR; exit}' "$SRC")
[ -n "$LINE_CLEAR" ] \
    || fail "fWorkerOwnsDisplay = false not found within 4 lines of lock_guard(fDisplayMu)"
pass

# ── A8: Result is drained with has_value() guard and reset after move ─────────
step "A8: main thread drains result with has_value() check and resets fResult"
grep -q 'fResult\.has_value()' "$SRC" \
    || fail "fResult.has_value() guard not found in $SRC"
grep -q 'fResult\.reset()' "$SRC" \
    || fail "fResult.reset() not found — result not cleared after drain in $SRC"
# Both must appear close together (within 8 lines) in the drain block.
LINE_HAS=$(grep -n 'fResult\.has_value' "$SRC" | head -1 | cut -d: -f1)
LINE_RST=$(grep -n 'fResult\.reset'     "$SRC" | head -1 | cut -d: -f1)
DIFF=$(( LINE_RST - LINE_HAS ))
[ "$DIFF" -ge 1 ] && [ "$DIFF" -le 8 ] \
    || fail "fResult.reset() (L$LINE_RST) should be within 8 lines of has_value() (L$LINE_HAS), diff=$DIFF"
pass

# ── A9: Worker rolls back messages on failure ──────────────────────────────────
step "A9: LocalWorkerFunc rolls *fMessages back to job.snapshot on API failure"
LINE_FUNC=$(grep -n 'static void LocalWorkerFunc' "$SRC" | head -1 | cut -d: -f1)
# The rollback expression: *w.fMessages = job.snapshot
ROLLBACK=$(awk -v after="$LINE_FUNC" \
    'NR > after && /\*w\.fMessages = job\.snapshot/ {print NR; exit}' "$SRC")
[ -n "$ROLLBACK" ] \
    || fail "*w.fMessages = job.snapshot rollback not found in LocalWorkerFunc"
pass

# ── A10: fWorkerOwnsDisplay uses a separate mutex from the job queue ───────────
step "A10: fDisplayMu is a separate std::mutex from fMu (deadlock-safe design)"
# Verify both mutex fields are declared inside LocalWorker.
LINE_WORKER=$(grep -n 'struct LocalWorker' "$SRC" | head -1 | cut -d: -f1)
# Find the closing brace of LocalWorker (first bare '};' after the struct).
LINE_END=$(awk -v after="$LINE_WORKER" 'NR > after && /^};/ {print NR; exit}' "$SRC")
[ -n "$LINE_END" ] || fail "closing brace of LocalWorker not found"
FMU_COUNT=$(awk -v s="$LINE_WORKER" -v e="$LINE_END" \
    'NR>=s && NR<=e && /std::mutex/ {c++} END {print c+0}' "$SRC")
[ "$FMU_COUNT" -ge 2 ] \
    || fail "LocalWorker needs >= 2 std::mutex members (fMu + fDisplayMu), found $FMU_COUNT"
# fDisplayMu must be named explicitly.
awk -v s="$LINE_WORKER" -v e="$LINE_END" 'NR>=s && NR<=e' "$SRC" \
    | grep -q 'fDisplayMu' \
    || fail "fDisplayMu not found inside LocalWorker struct"
pass

# ── A11: fRemote is nulled when /remote-control stops ─────────────────────────
step "A11: worker.fRemote is set to nullptr when /remote-control is stopped"
grep -q 'worker\.fRemote = nullptr' "$SRC" \
    || fail "worker.fRemote = nullptr not found in $SRC — remote pointer may dangle on stop"
# Verify the null happens near remote->Stop().
LINE_STOP=$(grep -n 'remote->Stop()' "$SRC" | head -1 | cut -d: -f1)
LINE_NULL=$(grep -n 'worker\.fRemote = nullptr' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_STOP" ] || fail "remote->Stop() not found in $SRC"
[ -n "$LINE_NULL" ] || fail "worker.fRemote = nullptr not found in $SRC"
DIFF=$(( LINE_NULL - LINE_STOP ))
[ "$DIFF" -ge -2 ] && [ "$DIFF" -le 5 ] \
    || fail "worker.fRemote=nullptr (L$LINE_NULL) should be within 5 lines of remote->Stop() (L$LINE_STOP), diff=$DIFF"
pass

# ── A12: fRemote is updated when /remote-control starts ───────────────────────
step "A12: worker.fRemote is updated to remote.get() after /remote-control starts"
grep -q 'worker\.fRemote = remote\.get()' "$SRC" \
    || fail "worker.fRemote = remote.get() not found in $SRC"
# The update must appear after remote->Start() in the file.
LINE_START=$(grep -n 'remote->Start()' "$SRC" | head -1 | cut -d: -f1)
LINE_SET=$(grep -n 'worker\.fRemote = remote\.get()' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_START" ] || fail "remote->Start() not found in $SRC"
[ -n "$LINE_SET"   ] || fail "worker.fRemote = remote.get() not found in $SRC"
[ "$LINE_SET" -gt "$LINE_START" ] \
    || fail "worker.fRemote update (L$LINE_SET) must come after remote->Start() (L$LINE_START)"
pass

# ── A13: Worker shutdown drains pending job before exiting ────────────────────
step "A13: LocalWorkerFunc checks 'fShutdown && !fPendingJob.has_value()' before break"
# This ensures an enqueued job is not silently dropped on shutdown.
grep -q 'fShutdown && !w\.fPendingJob\.has_value()' "$SRC" \
    || fail "shutdown drain check 'fShutdown && !w.fPendingJob.has_value()' not found in $SRC"
pass

# ── A14: Snapshot is taken before dispatch, not after ─────────────────────────
step "A14: job.snapshot is the pre-turn messages[] (captured before user push_back)"
# The main thread captures the snapshot and passes it to dispatch_turn.
# The worker then does push_back(user-turn) using job.apiContent.
# Verify that 'const json snapshot = messages' appears in the file and that
# job.snapshot is assigned from it (inside the dispatch_turn lambda or call site).
LINE_SNAP=$(grep -n 'const json snapshot = messages' "$SRC" | head -1 | cut -d: -f1)
LINE_JOB_SNAP=$(grep -n 'job\.snapshot.*=.*snapshot\|job\.snapshot.*=.*std::move(snapshot' "$SRC" \
    | grep -v 'fMessages' | head -1 | cut -d: -f1)
[ -n "$LINE_SNAP"     ] || fail "'const json snapshot = messages' not found in $SRC"
[ -n "$LINE_JOB_SNAP" ] || fail "job.snapshot assignment not found in $SRC"
# Worker push_back must not exist in main thread between snapshot and dispatch call.
LINE_DISPATCH=$(grep -n 'dispatch_turn(' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_DISPATCH" ] || fail "dispatch_turn() call not found in $SRC"
BAD=$(awk -v s="$LINE_SNAP" -v e="$LINE_DISPATCH" \
    'NR>s && NR<e && /messages\.push_back/' "$SRC")
[ -z "$BAD" ] \
    || fail "messages.push_back found between snapshot capture and dispatch_turn() call"
pass

# ── A15: WorkerGuard uses notify_all, not notify_one, for shutdown ─────────────
step "A15: WorkerGuard destructor calls fJobCv.notify_all() (wakes worker reliably)"
LINE_GUARD=$(grep -n 'struct WorkerGuard' "$SRC" | head -1 | cut -d: -f1)
NOTIFY_ALL=$(awk -v after="$LINE_GUARD" \
    'NR > after && NR <= after+20 && /fJobCv\.notify_all/ {print NR; exit}' "$SRC")
[ -n "$NOTIFY_ALL" ] \
    || fail "fJobCv.notify_all() not found within WorkerGuard dtor in $SRC — use notify_all for shutdown wakeup"
pass

# ── A16: TurnResult has all required fields ────────────────────────────────────
step "A16: TurnResult declares all required fields (ok, elapsed, tokens, URLs, text)"
LINE_TR=$(grep -n 'struct TurnResult' "$SRC" | head -1 | cut -d: -f1)
LINE_TR_END=$(awk -v after="$LINE_TR" 'NR > after && /^};/ {print NR; exit}' "$SRC")
BODY=$(awk -v s="$LINE_TR" -v e="$LINE_TR_END" 'NR>=s && NR<=e' "$SRC")
for field in ok elapsed inputTokens outputTokens cacheRead cacheWrite assistantText newUrls; do
    echo "$BODY" | grep -q "$field" \
        || fail "TurnResult missing field '$field'"
done
pass

# ── A17: Post-turn uses TurnResult field names (not api::SendResult names) ─────
step "A17: post-turn bookkeeping uses result.inputTokens/outputTokens (not input_tokens)"
# The drain_turn lambda must use TurnResult camelCase names.
# The old snake_case names (input_tokens, output_tokens) must not appear
# in the drain_turn lambda body.
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SRC" | head -1 | cut -d: -f1)
LINE_SAVE=$(grep -n 'config::SaveHistory' "$SRC" | tail -1 | cut -d: -f1)
[ -n "$LINE_DRAIN" ] || fail "drain_turn lambda not found in $SRC"
[ -n "$LINE_SAVE"  ] || fail "config::SaveHistory not found in $SRC"
BAD=$(awk -v s="$LINE_DRAIN" -v e="$LINE_SAVE" \
    'NR>=s && NR<=e && /result\.input_tokens|result\.output_tokens/' "$SRC")
[ -z "$BAD" ] \
    || fail "old api::SendResult field names found in drain_turn — use TurnResult names"
pass

# ── A18: URL harvest uses result.newUrls (deduped by worker) ──────────────────
step "A18: main thread uses result.newUrls for URL list (worker already deduped)"
grep -q 'result\.newUrls' "$SRC" \
    || fail "result.newUrls not referenced in $SRC"
# notify::ExtractUrls must not appear in drain_turn (belongs in LocalWorkerFunc).
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SRC" | head -1 | cut -d: -f1)
LINE_SAVE=$(grep -n 'config::SaveHistory' "$SRC" | tail -1 | cut -d: -f1)
BAD=$(awk -v s="$LINE_DRAIN" -v e="$LINE_SAVE" \
    'NR>=s && NR<=e && /notify::ExtractUrls/' "$SRC")
[ -z "$BAD" ] \
    || fail "notify::ExtractUrls called in drain_turn — URL extraction belongs in LocalWorkerFunc"
pass

# ── A19: hooks::Fire(Stop) uses result.assistantText ──────────────────────────
step "A19: hooks::Fire(Stop,...) uses result.assistantText from TurnResult"
grep -q 'result\.assistantText' "$SRC" \
    || fail "result.assistantText not referenced in $SRC"
# Verify it appears in the hooks::Fire(Stop) call.
grep -q 'hooks::Fire.*result\.assistantText\|result\.assistantText.*hooks::Fire' "$SRC" \
    || { # Accept multi-line form: assistantText on same line as Stop event.
         LINE_STOP_FIRE=$(grep -n 'hooks::Fire.*Stop\|Event::Stop' "$SRC" | tail -1 | cut -d: -f1)
         LINE_ASST=$(awk -v after="$LINE_STOP_FIRE" \
             'NR >= after && NR <= after+2 && /assistantText/' "$SRC")
         [ -n "$LINE_ASST" ] \
             || fail "hooks::Fire(Stop) does not reference result.assistantText within 2 lines"; }
pass

# ── A20: config::SaveHistory is called after each successful turn ──────────────
step "A20: config::SaveHistory is called in the post-turn success path"
# SaveHistory must appear in the drain_turn lambda, after the !result.ok
# early-return path.
LINE_DRAIN=$(grep -n 'auto drain_turn' "$SRC" | head -1 | cut -d: -f1)
LINE_FAIL_RETURN=$(awk -v after="$LINE_DRAIN" \
    'NR > after && /return true.*session continues\|MirrorCancel/ {print NR; exit}' "$SRC")
LINE_SAVE=$(awk -v after="${LINE_FAIL_RETURN:-0}" \
    'NR > after && /config::SaveHistory/ {print NR; exit}' "$SRC")
[ -n "$LINE_DRAIN"  ] || fail "drain_turn lambda not found in $SRC"
[ -n "$LINE_SAVE"   ] || fail "config::SaveHistory not found after failure path in $SRC"
pass

# ── A21: stats::RecordTurn receives cacheRead/cacheWrite from TurnResult ───────
step "A21: stats::RecordTurn is passed result.cacheRead and result.cacheWrite"
grep -q 'result\.cacheRead' "$SRC" \
    || fail "result.cacheRead not referenced in $SRC — cache stats not forwarded to RecordTurn"
grep -q 'result\.cacheWrite' "$SRC" \
    || fail "result.cacheWrite not referenced in $SRC — cache stats not forwarded to RecordTurn"
# Verify both appear on or very near the stats::RecordTurn call line.
LINE_RECORD=$(grep -n 'stats::RecordTurn' "$SRC" | tail -1 | cut -d: -f1)
[ -n "$LINE_RECORD" ] || fail "stats::RecordTurn not found in $SRC"
RECORD_BLOCK=$(awk -v s="$LINE_RECORD" -v e="$((LINE_RECORD+4))" 'NR>=s && NR<=e' "$SRC")
echo "$RECORD_BLOCK" | grep -q 'cacheRead'  \
    || fail "result.cacheRead not passed to stats::RecordTurn (check within 4 lines of call)"
echo "$RECORD_BLOCK" | grep -q 'cacheWrite' \
    || fail "result.cacheWrite not passed to stats::RecordTurn (check within 4 lines of call)"
pass

# ── A22: Display lock acquired before job lock during enqueue (lock ordering) ──
step "A22: dispatch_turn acquires fDisplayMu before fMu (prevents ABBA deadlock)"
# The dispatch_turn lambda must acquire fDisplayMu (fWorkerOwnsDisplay=true)
# before fMu (fPendingJob assignment). Verify line ordering within the lambda.
LINE_DISPATCH_LAMBDA=$(grep -n 'auto dispatch_turn' "$SRC" | head -1 | cut -d: -f1)
LINE_DISP=$(awk -v after="$LINE_DISPATCH_LAMBDA" \
    'NR > after && /lock_guard.*fDisplayMu/ {print NR; exit}' "$SRC")
LINE_JOBMU=$(awk -v after="${LINE_DISP:-0}" \
    'NR > after && /lock_guard.*fMu/ {print NR; exit}' "$SRC")
[ -n "$LINE_DISP"  ] || fail "lock_guard(fDisplayMu) not found in dispatch_turn"
[ -n "$LINE_JOBMU" ] || fail "lock_guard(fMu) not found after lock_guard(fDisplayMu)"
[ "$LINE_DISP" -lt "$LINE_JOBMU" ] \
    || fail "fDisplayMu (L$LINE_DISP) must be locked before fMu (L$LINE_JOBMU)"
pass

# ── A23: Worker loops and accepts multiple sequential jobs ─────────────────────
step "A23: LocalWorkerFunc has a 'while (true)' main loop (handles multiple turns)"
LINE_FUNC=$(grep -n 'static void LocalWorkerFunc' "$SRC" | head -1 | cut -d: -f1)
WHILE=$(awk -v after="$LINE_FUNC" \
    'NR > after && NR <= after+15 && /while \(true\)/ {print NR; exit}' "$SRC")
[ -n "$WHILE" ] \
    || fail "while(true) loop not found within 15 lines of LocalWorkerFunc — worker won't handle multiple turns"
pass

# ── A24: Binary starts and exits cleanly (no thread-lifecycle crash) ───────────
step "A24: binary exits 0 for --help without hanging or crashing"
[ -x "$BIN" ] || fail "$BIN missing or not executable"
set +e
"$BIN" --help </dev/null >/dev/null 2>&1
rc=$?
set -e
# --help exits 0 and must not be killed by a signal (rc < 128).
[ "$rc" -eq 0 ]   || fail "--help exited with rc=$rc (expected 0)"
pass

# ── A25: Binary exits non-zero without auth and without hanging ────────────────
step "A25: unauthenticated one-shot exits quickly (worker thread does not hang)"
home=$(mktemp -d /tmp/claude-lw-test.XXXXXX)
set +e
timeout 10 env -u ANTHROPIC_API_KEY HOME="$home" "$BIN" "hello" </dev/null >/dev/null 2>&1
rc=$?
set -e
rm -rf "$home"
# exit code 1 or 2 are fine; 124 = timeout (hang); >= 128 = signal (crash).
[ "$rc" -ne 124 ] || fail "binary timed out — worker thread may be blocking shutdown"
[ "$rc" -lt 128 ] || fail "binary exited with signal (rc=$rc) — possible crash in thread teardown"
[ "$rc" -ne 0   ] || fail "expected non-zero exit without auth credentials"
pass

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
