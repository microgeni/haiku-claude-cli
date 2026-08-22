#!/usr/bin/env bash
# Config tool-permission policy tests.
#
# "allow_destructive_tools" is an opt-in that suppresses every tool
# permission prompt, so it must be honoured exactly as written. The key
# was previously spelled "fAllowDestructiveTools" — the C++ member name
# leaked into the JSON schema — which meant the natural snake_case
# spelling was parsed as an unknown key and silently ignored. These
# tests pin the canonical key, the two legacy spellings, and the
# negative cases so that failure mode cannot return unnoticed.

set -euo pipefail

BIN="${BIN:-./build/claude}"

fail() { echo "FAIL: $*" >&2; exit 1; }

tmphome=$(mktemp -d /tmp/claude-cfg-test.XXXXXX)
trap 'rm -rf "$tmphome"' EXIT
cfgdir="$tmphome/config/settings/claude-cli"
mkdir -p "$cfgdir"

# Capture output into a variable rather than piping into `grep -q`.
# grep -q exits on first match and closes the pipe, which kills the
# binary with SIGPIPE; under `set -o pipefail` that non-zero status
# propagates and the match is reported as a miss. Matching in-process
# with [[ ]] avoids the pipe and the false negative entirely.
cli_output() {
    printf '%s' "$1" > "$cfgdir/config.json"
    env -u ANTHROPIC_API_KEY HOME="$tmphome" "$BIN" --version </dev/null 2>&1 || true
}

# Engages ludicrous mode? Emits the banner on stderr at startup.
ludicrous_for() {
    local out; out=$(cli_output "$1")
    [[ "$out" == *LUDICROUS* ]] && echo yes || echo no
}

expect() {
    local label="$1" json="$2" want="$3"
    local got
    got=$(ludicrous_for "$json")
    [ "$got" = "$want" ] || fail "$label: expected $want, got $got (config: $json)"
    echo "  ok: $label -> $got"
}

echo "canonical key"
expect "allow_destructive_tools=true"   '{"allow_destructive_tools": true}'   yes
expect "allow_destructive_tools=false"  '{"allow_destructive_tools": false}'  no

echo "legacy spellings still accepted"
expect "fAllowDestructiveTools=true"    '{"fAllowDestructiveTools": true}'    yes
expect "fAllowDestructivetools=true"    '{"fAllowDestructivetools": true}'    yes

echo "negative cases"
expect "key absent"                     '{"model": "claude-sonnet-4-5"}'      no
expect "empty object"                   '{}'                                 no
expect "canonical wins over legacy"     '{"allow_destructive_tools": false, "fAllowDestructiveTools": true}' no

echo "malformed config does not engage ludicrous mode"
expect "invalid json"                   '{not valid json'                    no

echo "help text advertises the canonical key"
"$BIN" --help </dev/null 2>&1 | grep -q "allow_destructive_tools" \
    || fail "--help does not mention allow_destructive_tools"
echo "  ok: --help mentions allow_destructive_tools"

# Deprecation warnings. Accepting a legacy spelling silently is how a
# stale key sits in a config unnoticed, so the legacy paths must be
# loud and the canonical path must stay quiet.
warns_for() {
    local out; out=$(cli_output "$1")
    [[ "$out" == *deprecated* ]] && echo yes || echo no
}

expect_warn() {
    local label="$1" json="$2" want="$3"
    local got
    got=$(warns_for "$json")
    [ "$got" = "$want" ] || fail "$label: expected warn=$want, got $got (config: $json)"
    echo "  ok: $label -> warn=$got"
}

echo "deprecation warnings"
expect_warn "canonical key is quiet"      '{"allow_destructive_tools": true}'   no
expect_warn "fAllowDestructiveTools warns" '{"fAllowDestructiveTools": true}'   yes
expect_warn "fAllowDestructivetools warns" '{"fAllowDestructivetools": true}'   yes
expect_warn "canonical suppresses the nag" '{"allow_destructive_tools": true, "fAllowDestructivetools": true}' no
expect_warn "telegram legacy warns"        '{"telegram": {"fAllowDestructivetools": true}}' yes
expect_warn "telegram canonical is quiet"  '{"telegram": {"allow_destructive_tools": true}}' no
expect_warn "no keys at all is quiet"      '{"model": "claude-sonnet-4-5"}'     no

# The warning must name the replacement, not just complain.
out=$(cli_output '{"fAllowDestructivetools": true}')
[[ "$out" == *'rename it to "allow_destructive_tools"'* ]] \
    || fail "deprecation warning does not name the canonical key"
echo "  ok: warning names the canonical replacement"
