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

# Engages ludicrous mode? Emits the banner on stderr at startup.
ludicrous_for() {
    printf '%s' "$1" > "$cfgdir/config.json"
    if env -u ANTHROPIC_API_KEY HOME="$tmphome" "$BIN" --version </dev/null 2>&1 \
            | grep -q "LUDICROUS"; then
        echo yes
    else
        echo no
    fi
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
