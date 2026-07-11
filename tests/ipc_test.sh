#!/bin/sh
# tests/ipc_test.sh — Test the Genio→Claude IPC feature (MSG_ASK_PROMPT)
#
# This script verifies the IPC path end-to-end in three stages:
#
#   Stage 1 — Build the ipc_test_sender helper binary
#   Stage 2 — Confirm the Claude GUI app is running (or start it)
#   Stage 3 — Fire several MSG_ASK_PROMPT messages with different payloads
#
# Run from the project root:
#   sh tests/ipc_test.sh
#
# Requirements:
#   • Haiku OS (uses BMessenger / BRoster)
#   • The Claude GUI (build/Claude) must be installed or reachable
#   • make + g++ available in PATH

set -e
cd "$(dirname "$0")/.."   # always run from project root

SENDER=build/ipc_test_sender
CLAUDE_GUI=build/Claude

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; RESET='\033[0m'
pass() { printf "${GREEN}[PASS]${RESET} %s\n" "$1"; }
fail() { printf "${RED}[FAIL]${RESET} %s\n" "$1"; exit 1; }
info() { printf "${YELLOW}[INFO]${RESET} %s\n" "$1"; }
banner() { printf "\n${BOLD}%s${RESET}\n" "$1"; }

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1 — Build the sender
# ─────────────────────────────────────────────────────────────────────────────
banner "Stage 1 — Build ipc_test_sender"

if [ ! -f "$SENDER" ]; then
    info "Binary not found — running: make ipc-test"
    make ipc-test
fi

if [ ! -x "$SENDER" ]; then
    fail "$SENDER not executable after build"
fi
pass "ipc_test_sender is built: $SENDER"

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2 — Ensure Claude GUI is running
# ─────────────────────────────────────────────────────────────────────────────
banner "Stage 2 — Check Claude GUI"

CLAUDE_SIG="application/x-vnd.Microgeni-claude-gui"

# query() checks if a given app signature is in the roster.
app_running() {
    # 'ps' on Haiku lists all teams; we look for the binary name.
    ps | grep -q "Claude$" 2>/dev/null || \
    # Also check via roster query (most reliable but needs a helper we don't
    # have in plain sh — fall back to ps heuristic above).
    false
}

if ! app_running; then
    info "Claude GUI does not appear to be running."
    info "Attempting to launch: $CLAUDE_GUI"
    if [ -x "$CLAUDE_GUI" ]; then
        open "$CLAUDE_GUI" &
        info "Waiting 3 seconds for the app to start…"
        sleep 3
    elif [ -x "/boot/system/non-packaged/apps/Claude" ]; then
        open "/boot/system/non-packaged/apps/Claude" &
        sleep 3
    else
        fail "Could not find Claude GUI binary. Build it with: make gui"
    fi
fi
info "Claude GUI should be running. The sender will fail gracefully if not."

# ─────────────────────────────────────────────────────────────────────────────
# Stage 3 — Send test messages
# ─────────────────────────────────────────────────────────────────────────────
banner "Stage 3 — Send IPC messages"

TESTS_PASSED=0
TESTS_FAILED=0

run_test() {
    LABEL="$1"
    shift
    printf "\n  Test: %s\n" "$LABEL"
    if "$SENDER" "$@"; then
        pass "$LABEL"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        fail "$LABEL — sender returned non-zero"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test A: simplest possible prompt (no optional fields)
run_test "Simple prompt only" \
    --prompt "IPC test A: What is the capital of France?"

echo
info "Waiting 1 second between messages…"
sleep 1

# Test B: prompt + context
run_test "Prompt with context" \
    --context "The user is testing the IPC feature from the command line." \
    --prompt "IPC test B: Please confirm you received this message via IPC."

sleep 1

# Test C: prompt + working directory override
WORKDIR="$(pwd)"
run_test "Prompt with working_dir" \
    --working-dir "$WORKDIR" \
    --prompt "IPC test C: List the files in the current working directory using the list_files tool."

sleep 1

# Test D: all three fields together
run_test "All three fields" \
    --context "Project: haiku-claude-cli" \
    --working-dir "$WORKDIR" \
    --prompt "IPC test D: What files are in this project root?"

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
banner "Results"
printf "  Passed : ${GREEN}%d${RESET}\n" "$TESTS_PASSED"
printf "  Failed : ${RED}%d${RESET}\n"   "$TESTS_FAILED"

if [ "$TESTS_FAILED" -eq 0 ]; then
    printf "\n${GREEN}${BOLD}All IPC send tests passed.${RESET}\n"
    printf "Check the Claude window — it should have come to the front\n"
    printf "with each prompt pre-filled in the input box in turn.\n"
    printf "Press Enter (or the Send button) in Claude to actually run any of them.\n"
    exit 0
else
    printf "\n${RED}${BOLD}Some tests failed.${RESET}\n"
    exit 1
fi
