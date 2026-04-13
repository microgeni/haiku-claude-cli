#!/usr/bin/env bash
# Functional tests for the CLI binary.
# Runs on Haiku, invoked from the project root after build.

set -euo pipefail

BIN="./build/claude"

step() { echo; echo "=== $* ==="; }
fail() { echo "FAIL: $*" >&2; exit 1; }

step "binary sanity"
[ -x "$BIN" ]                          || fail "$BIN missing or not executable"
file "$BIN" | grep -q "ELF 64-bit"     || fail "not a 64-bit ELF"
file "$BIN" | grep -q "x86-64"         || fail "not x86-64"
echo "PASS"

step "--help exits 0 and prints usage"
out=$("$BIN" --help 2>&1) || fail "--help exited non-zero"
echo "$out" | grep -q "^Usage:"        || fail "missing Usage line"
echo "$out" | grep -q "login"          || fail "missing login command"
echo "$out" | grep -q "logout"         || fail "missing logout command"
echo "PASS"

step "no-args exits non-zero"
if "$BIN" >/dev/null 2>&1; then
    fail "expected non-zero exit with no args"
fi
echo "PASS"

step "unauthenticated request reports missing auth"
tmphome=$(mktemp -d /tmp/claude-cli-test.XXXXXX)
trap 'rm -rf "$tmphome"' EXIT
set +e
out=$(env -u ANTHROPIC_API_KEY HOME="$tmphome" "$BIN" "hello" 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ]                        || fail "expected non-zero exit when no auth configured"
echo "$out" | grep -qi "no authentication" || fail "error did not mention missing auth (got: $out)"
echo "PASS"

echo
echo "=== all tests passed ==="
