#!/usr/bin/env bash
# Static analysis and security audit. Runs on Haiku, invoked from the
# project root. Installs cppcheck if missing; skips flawfinder if absent.

set -euo pipefail

echo "=== ensure cppcheck ==="
if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck not found, installing..."
    pkgman install -y cppcheck
fi

echo "=== cppcheck version ==="
cppcheck --version

echo "=== make check (lint + security) ==="
if command -v flawfinder >/dev/null 2>&1; then
    make check
else
    echo "flawfinder not found — running cppcheck only"
    make lint
fi
