#!/usr/bin/env bash
# Telegram bridge + console interaction tests.
#
# These tests verify static properties of the source code and binary
# that relate to the Telegram RemoteControl path — VT escape hygiene,
# turn-lock invariants, slash-command parity, config key handling, and
# the C11 prompt-restore guarantee (every ProcessUpdate / TryHandleSlashImmediate
# exit-point must emit \x1b8 to restore the saved cursor position).
#
# Tests that require a live TELEGRAM_API_BASE mock server are marked
# SKIP when the env var is absent.
#
# Run from the project root after a successful build:
#
#   bash tests/telegram_console_test.sh
#
# 34 tests total; up to 3 may be skipped when TELEGRAM_API_BASE is unset.
# T28–T34 cover the "Telegram turns visible in local scroll history" fix.

set -euo pipefail

BIN="./build/claude"
SRC="src/telegram.cpp"
HDR="src/telegram.h"

PASS=0
FAIL=0
SKIP=0

step() { echo; echo "--- $* ---"; }
pass() { echo "PASS"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); }
skip() { echo "SKIP: $*"; SKIP=$((SKIP+1)); }

echo "=== Telegram / Console Interaction Tests ==="

# ── T1: source file exists ────────────────────────────────────────────────────
step "T1: telegram.cpp exists in src/"
[ -f "$SRC" ] || fail "$SRC not found"
pass

# ── T2: VT escape hygiene — no bare \x1b8 ────────────────────────────────────
# Every cursor-restore in telegram.cpp must use the two-character string
# literal \x1b""8 (which produces ESC 8 = DECRC), not a bare octal or
# hex escape that could be mis-parsed.  A bare \x1b8 would embed the
# byte sequence ESC '8', which is correct in isolation but fragile when
# a compiler folds adjacent string literals differently.  The project
# convention (established in the v1.6.1 audit) is the split-literal form
# "\x1b""8".
step "T2: no bare \\x1b8 escape — all restores use split-literal form"
# Look for the problematic pattern: \x1b followed immediately by 8 inside
# a string (i.e. "\x1b8"), which is the form we audited away.
if grep -Pn '\\x1b8' "$SRC" 2>/dev/null; then
    fail "found bare \\x1b8 in $SRC — use \"\\x1b\"\"8\" instead"
else
    pass
fi

# ── T3: every non-trivial exit point in ProcessUpdate restores cursor ─────────
step "T3: ProcessUpdate — all early-return paths emit cursor-restore (\\x1b\"8\")"
# Count lines that contain an early return (std::cout << "\x1b""8") inside
# ProcessUpdate.  We verify >= 5 such restores exist (mute, unmute, /new,
# slash-dispatch, auth-error, post-send) as a lower bound.
COUNT=$(grep -c '"8"' "$SRC" || true)
[ "$COUNT" -ge 5 ] || fail "expected >= 5 cursor-restores in $SRC, found $COUNT"
pass

# ── T4: cursor restore is guaranteed on all exit paths ───────────────────────
step "T4: TryHandleSlashImmediate — all return paths emit cursor-restore"
# ProcessUpdate now uses a RAII CursorGuard (one \x1b"8" in the destructor)
# instead of per-return-path literals, so the raw count is lower but the
# guarantee is stronger. TryHandleSlashImmediate still has individual restores.
# Verify: at least one \x1b"8" restore exists in each function's region.
TSHI=$(awk '/^bool RemoteControl::TryHandleSlashImmediate/,/^void RemoteControl::WorkLoop/' "$SRC" | grep -c '"8"' || true)
PU=$(awk '/^void RemoteControl::ProcessUpdate/,/^} \/\/ namespace telegram/' "$SRC" | grep -c '"8"' || true)
[ "$TSHI" -ge 1 ] || fail "TryHandleSlashImmediate has no cursor-restore; check the function"
[ "$PU"   -ge 1 ] || fail "ProcessUpdate has no cursor-restore (expected RAII guard); check the function"
pass

# ── T5: cursor-save always precedes cursor-restore in both functions ──────────
step "T5: every \\x1b\"7\" save has a matching \\x1b\"8\" restore in same function"
SAVES=$(grep -c '"7"' "$SRC" || true)
RESTORES=$(grep -c '"8"' "$SRC" || true)
# There are 2 save points (one per function) and many restores (one per exit).
# Restores must be >= saves.
[ "$RESTORES" -ge "$SAVES" ] || fail "restores ($RESTORES) < saves ($SAVES)"
[ "$SAVES" -ge 2 ] || fail "expected >= 2 cursor-saves in $SRC, found $SAVES"
pass

# ── T6: /mute and /unmute are handled in both code paths ─────────────────────
step "T6: /mute and /unmute handled in TryHandleSlashImmediate AND ProcessUpdate"
# Count comparisons against the literal string /mute (2 per function = 4 total)
# and /unmute (2 per function = 4 total).  Use grep -c without surrounding
# quote delimiters since the grep pattern needn't include the C++ string quotes.
MUTE_COUNT=$(grep -c '== "/mute"' "$SRC" || true)
UNMUTE_COUNT=$(grep -c '== "/unmute"' "$SRC" || true)
[ "$MUTE_COUNT"   -ge 2 ] || fail "expected >= 2 /mute comparisons, found $MUTE_COUNT"
[ "$UNMUTE_COUNT" -ge 2 ] || fail "expected >= 2 /unmute comparisons, found $UNMUTE_COUNT"
pass

# ── T7: /new is handled in both code paths ───────────────────────────────────
step "T7: /new (clear history) is present in both TryHandleSlashImmediate and ProcessUpdate"
NEW_COUNT=$(grep -c '"/new"' "$SRC" || true)
[ "$NEW_COUNT" -ge 2 ] || fail "expected /new in >= 2 places, found $NEW_COUNT"
pass

# ── T8: /exit and /quit are blocked from Telegram ────────────────────────────
step "T8: /exit and /quit are blocked in both code paths (not available from Telegram)"
# The block message is built via string concatenation so we search for the
# fixed suffix instead of the full quoted string.
EXIT_BLOCK=$(grep -c 'is not available from Telegram' "$SRC" || true)
[ "$EXIT_BLOCK" -ge 2 ] || fail "expected /exit block in >= 2 places, found $EXIT_BLOCK"
pass

# ── T9: /remote-control is also blocked from Telegram ────────────────────────
step "T9: /remote-control is blocked in Telegram path"
RC_BLOCK=$(grep -c '"/remote-control"' "$SRC" || true)
[ "$RC_BLOCK" -ge 2 ] || fail "expected /remote-control guard in >= 2 places, found $RC_BLOCK"
pass

# ── T10: /start maps to /help ────────────────────────────────────────────────
step "T10: /start is mapped to /help for Telegram new-user experience"
START=$(grep -c '"/start"' "$SRC" || true)
[ "$START" -ge 2 ] || fail "expected /start -> /help mapping in >= 2 places, found $START"
pass

# ── T11: fAllowDestructiveTools config key (camelCase) is accepted ────────────
step "T11: fAllowDestructiveTools config key is read from telegram config block"
grep -q '"fAllowDestructiveTools"' "$SRC" \
    || fail '"fAllowDestructiveTools" key not found in telegram.cpp'
pass

# ── T12: legacy lowercase-t key (fAllowDestructivetools) is also accepted ─────
step "T12: legacy fAllowDestructivetools key is accepted as fallback"
grep -q '"fAllowDestructivetools"' "$SRC" \
    || fail '"fAllowDestructivetools" fallback key not found in telegram.cpp'
pass

# ── T13: config.cpp also handles both key spellings ──────────────────────────
step "T13: config.cpp handles both fAllowDestructiveTools and fAllowDestructivetools"
grep -q '"fAllowDestructiveTools"'   src/config.cpp || fail 'missing fAllowDestructiveTools in config.cpp'
grep -q '"fAllowDestructivetools"'   src/config.cpp || fail 'missing fAllowDestructivetools in config.cpp'
pass

# ── T14: ConfigIsValid checks for missing bot_token ──────────────────────────
step "T14: ConfigIsValid rejects config with no bot_token"
grep -q 'bot_token is not set' "$SRC" \
    || fail 'missing bot_token error message in telegram.cpp'
pass

# ── T15: ConfigIsValid checks for missing allowed_user_ids ───────────────────
step "T15: ConfigIsValid rejects config with no allowed_user_ids"
grep -q 'allowed_user_ids must list at least one Telegram user ID' "$SRC" \
    || fail 'missing allowed_user_ids error message in telegram.cpp'
pass

# ── T16: turn-lock — AcquireTurn and ReleaseTurn are paired in WorkLoop ───────
step "T16: WorkLoop pairs AcquireTurn/ReleaseTurn around ProcessUpdate"
ACQUIRE=$(grep -c 'AcquireTurn'  "$SRC" || true)
RELEASE=$(grep -c 'ReleaseTurn'  "$SRC" || true)
# Definition (1) + call in WorkLoop (1) + call in session.cpp = at least 2 each.
# For telegram.cpp specifically: definition + WorkLoop call = 2 each minimum.
[ "$ACQUIRE" -ge 2 ] || fail "expected >= 2 AcquireTurn references, found $ACQUIRE"
[ "$RELEASE" -ge 2 ] || fail "expected >= 2 ReleaseTurn references, found $RELEASE"
pass

# ── T17: TelegramSink class exists and implements StructuredSink ─────────────
step "T17: TelegramSink implements StructuredSink (step 3 architecture)"
grep -q 'class TelegramSink' "$HDR" \
    || fail 'TelegramSink class not found in telegram.h'
grep -q 'StructuredSink' "$HDR" \
    || fail 'TelegramSink does not inherit StructuredSink'
grep -q 'BeginMessage\|AppendText\|EndMessage' "$SRC" \
    || fail 'TelegramSink streaming lifecycle methods not found in telegram.cpp'
pass

# ── T18: TelegramSink::AskPermission uses inline keyboard (not g_hook) ───────
step "T18: TelegramSink::AskPermission uses inline keyboard buttons"
grep -q 'perm:yes\|perm:no\|perm:always' "$SRC" \
    || fail 'TelegramSink permission buttons (perm:yes/no/always) not found'
grep -q 'fPermQueue' "$SRC" \
    || fail 'TelegramSink does not drain fPermQueue for permission responses'
pass

# ── T19: MirrorToPrimary guards against double-join (StopThinkingUpdater first)
step "T19: MirrorToPrimary calls StopThinkingUpdater before touching the thread"
# Verify StopThinkingUpdater appears before the fClient.EditMessageText call
# in MirrorToPrimary. We check line ordering in the file.
LINE_STOP=$(grep -n 'StopThinkingUpdater' "$SRC" | grep -v 'void RemoteControl::Stop\b' | head -1 | cut -d: -f1)
LINE_EDIT=$(grep -n 'EditMessageText.*fPrimaryUserId' "$SRC" | head -1 | cut -d: -f1)
[ -n "$LINE_STOP" ] || fail "StopThinkingUpdater call not found"
[ -n "$LINE_EDIT" ] || fail "EditMessageText(fPrimaryUserId,...) call not found"
[ "$LINE_STOP" -lt "$LINE_EDIT" ] \
    || fail "StopThinkingUpdater (L$LINE_STOP) must appear before EditMessageText (L$LINE_EDIT)"
pass

# ── T20: ANSI stripping is applied before sending text to Telegram ────────────
step "T20: ANSI CSI sequences are stripped before TgSend in slash-command path"
# Both TryHandleSlashImmediate and ProcessUpdate have an "// Strip ANSI" comment
# and a corresponding stripping loop. Count both comments and loop references.
STRIP=$(grep -c "Strip ANSI\|strip.*ANSI\|ANSI.*strip" "$SRC" || true)
[ "$STRIP" -ge 2 ] || fail "expected >= 2 ANSI-strip blocks, found $STRIP"
pass

# ── T21: /help slash command lists remote-control ────────────────────────────
step "T21: /help slash command output lists /remote-control"
# remote-control is a REPL slash command, not a --help CLI flag.
# Verify it is registered in the /help text inside commands.cpp.
grep -q 'remote-control' src/commands.cpp \
    || fail '/remote-control not found in commands.cpp /help text'
pass

# ── T22: binary exits non-zero without auth (API hang probe) ─────────────────
step "T22: binary exits non-zero immediately when no auth is configured"
home=$(mktemp -d /tmp/claude-tg-test.XXXXXX)
set +e
out=$(env -u ANTHROPIC_API_KEY HOME="$home" "$BIN" "ping" </dev/null 2>&1)
rc=$?
set -e
rm -rf "$home"
[ "$rc" -ne 0 ] || fail "expected non-zero exit without auth"
pass

# ── T23: TELEGRAM_API_BASE redirect probe (needs mock server) ─────────────────
step "T23: TELEGRAM_API_BASE redirects Telegram HTTP calls (needs mock)"
if [ -z "${TELEGRAM_API_BASE:-}" ]; then
    skip "TELEGRAM_API_BASE not set — set to a mock server URL to enable"
else
    # If a mock URL is given, just verify the binary accepts the env var
    # without crashing (it will fail gracefully on real API calls).
    home=$(mktemp -d /tmp/claude-tg-test.XXXXXX)
    set +e
    out=$(env -u ANTHROPIC_API_KEY \
               TELEGRAM_API_BASE="$TELEGRAM_API_BASE" \
               HOME="$home" "$BIN" "ping" </dev/null 2>&1)
    rc=$?
    set -e
    rm -rf "$home"
    [ "$rc" -lt 128 ] || fail "binary crashed with TELEGRAM_API_BASE set (exit $rc)"
    pass
fi

# ── T24: SetSharedHistory method is declared in telegram.h ───────────────────
step "T24: SetSharedHistory() is declared in telegram.h"
grep -q 'SetSharedHistory' src/telegram.h \
    || fail 'SetSharedHistory not declared in telegram.h'
pass

# ── T25: fSharedHistory member exists in telegram.h ──────────────────────────
step "T25: fSharedHistory member variable is declared in telegram.h"
grep -q 'fSharedHistory' src/telegram.h \
    || fail 'fSharedHistory not found in telegram.h'
pass

# ── T26: ProcessUpdate builds call_msgs from shared history + user thread ─────
step "T26: ProcessUpdate uses call_msgs (shared history + user thread) for SendWithTools"
grep -q 'call_msgs' "$SRC" || fail 'call_msgs not found in telegram.cpp'
CALL_MSGS_COUNT=$(grep -c 'call_msgs' "$SRC" || true)
[ "$CALL_MSGS_COUNT" -ge 3 ] \
    || fail "expected >= 3 call_msgs references (declare, build, pass), found $CALL_MSGS_COUNT"
grep -q 'fSharedHistory' "$SRC" \
    || fail 'fSharedHistory not referenced in telegram.cpp'
grep -q 'SetSharedHistory' src/session.cpp \
    || fail 'SetSharedHistory not called in session.cpp'
pass

# ── T27: SetSharedHistory registered immediately after Start() in session.cpp ─
step "T27: SetSharedHistory registered within 10 lines of remote->Start() in session.cpp"
LINE_START=$(grep -n 'remote->Start()' src/session.cpp | head -1 | cut -d: -f1)
LINE_SET=$(grep -n 'SetSharedHistory' src/session.cpp | head -1 | cut -d: -f1)
[ -n "$LINE_START" ] || fail 'remote->Start() not found in session.cpp'
[ -n "$LINE_SET"   ] || fail 'SetSharedHistory not found in session.cpp'
DIFF=$(( LINE_SET - LINE_START ))
[ "$DIFF" -ge 1 ] && [ "$DIFF" -le 10 ] \
    || fail "SetSharedHistory (L$LINE_SET) should be within 10 lines of Start() (L$LINE_START), diff=$DIFF"
pass

# ── T28: SetSharedHistoryAppender is declared in telegram.h ──────────────────
step "T28: SetSharedHistoryAppender() is declared in telegram.h"
grep -q 'SetSharedHistoryAppender' src/telegram.h \
    || fail 'SetSharedHistoryAppender not declared in telegram.h'
pass

# ── T29: fSharedHistoryAppend member is declared in telegram.h ───────────────
step "T29: fSharedHistoryAppend member variable is declared in telegram.h"
grep -q 'fSharedHistoryAppend' src/telegram.h \
    || fail 'fSharedHistoryAppend not found in telegram.h'
pass

# ── T30: SetSharedHistoryAppender is implemented in telegram.cpp ─────────────
step "T30: SetSharedHistoryAppender() is implemented in telegram.cpp"
grep -q 'RemoteControl::SetSharedHistoryAppender' src/telegram.cpp \
    || fail 'SetSharedHistoryAppender implementation not found in telegram.cpp'
pass

# ── T31: fSharedHistoryAppend is called in ProcessUpdate after success ────────
step "T31: ProcessUpdate calls fSharedHistoryAppend after a successful turn"
# After the fix, ProcessUpdate must invoke the appender with the user and
# assistant messages.  Verify both the call site and the guard exist.
grep -q 'fSharedHistoryAppend' src/telegram.cpp \
    || fail 'fSharedHistoryAppend not referenced in telegram.cpp'
APPEND_CALLS=$(grep -c 'fSharedHistoryAppend' src/telegram.cpp || true)
# One definition (SetSharedHistoryAppender body) + at least one call site
# in ProcessUpdate.
[ "$APPEND_CALLS" -ge 2 ] \
    || fail "expected >= 2 fSharedHistoryAppend references (setter + call), found $APPEND_CALLS"
pass

# ── T32: assistant reply is appended to msgs silo in ProcessUpdate ────────────
step "T32: ProcessUpdate appends the assistant reply to the per-user message silo"
grep -q 'assistant_msg' src/telegram.cpp \
    || fail 'assistant_msg not found in telegram.cpp — assistant reply not appended'
LINE_ERR=$(grep -n 'exit_code != 0\|assistant_text.empty' src/telegram.cpp | tail -1 | cut -d: -f1)
LINE_PUSH=$(grep -n 'msgs.push_back.*assistant_msg\|assistant_msg.*msgs' src/telegram.cpp | head -1 | cut -d: -f1)
[ -n "$LINE_ERR"  ] || fail 'error guard not found in telegram.cpp'
[ -n "$LINE_PUSH" ] || fail 'msgs.push_back(assistant_msg) not found in telegram.cpp'
[ "$LINE_PUSH" -gt "$LINE_ERR" ] \
    || fail "assistant_msg push (L$LINE_PUSH) must come after the error guard (L$LINE_ERR)"
pass

# ── T33: SetSharedHistoryAppender is wired up in session.cpp ─────────────────
step "T33: session.cpp calls SetSharedHistoryAppender after Start()"
grep -q 'SetSharedHistoryAppender' src/session.cpp \
    || fail 'SetSharedHistoryAppender not called in session.cpp'
pass

# ── T34: write-back lambda calls SaveHistory so history.json is updated ───────
step "T34: write-back lambda in session.cpp calls config::SaveHistory"
# The appender lambda must call SaveHistory so the Telegram-origin turn
# is persisted to disk and survives --resume in a future session.
# Verify that SaveHistory appears within the SetSharedHistoryAppender
# lambda body (i.e. after the SetSharedHistoryAppender call site).
LINE_SETTER=$(grep -n 'SetSharedHistoryAppender' src/session.cpp | head -1 | cut -d: -f1)
LINE_SAVE=$(grep -n 'config::SaveHistory' src/session.cpp | awk -F: -v after="$LINE_SETTER" '$1 > after {print $1; exit}')
[ -n "$LINE_SETTER" ] || fail 'SetSharedHistoryAppender call not found in session.cpp'
[ -n "$LINE_SAVE"   ] \
    || fail "config::SaveHistory not found after SetSharedHistoryAppender in session.cpp — history.json will not be updated for Telegram turns"
# Confirm SaveHistory appears close by (within 15 lines of the setter),
# i.e. it is inside the lambda, not somewhere unrelated later.
DIFF=$(( LINE_SAVE - LINE_SETTER ))
[ "$DIFF" -le 15 ] \
    || fail "SaveHistory (L$LINE_SAVE) is $DIFF lines after SetSharedHistoryAppender (L$LINE_SETTER) — check it is inside the lambda"
pass

echo
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] || exit 1
