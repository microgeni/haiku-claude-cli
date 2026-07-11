#!/usr/bin/env bash
# Static analysis and security audit. Runs on Haiku, invoked from the
# project root. Ensures a *working* cppcheck (the packaged binary has
# shipped broken before — present but missing its own shared libraries,
# so `cppcheck --version` exits non-zero and produces no output). We
# verify cppcheck actually runs, attempt a clean reinstall if it does
# not, and only fail if analysis genuinely cannot run. flawfinder is
# optional and skipped when absent.

set -euo pipefail

# Return 0 only if cppcheck exists AND actually executes (a corrupt
# package fails here with a runtime-loader error and a non-zero exit).
cppcheck_works() {
    command -v cppcheck >/dev/null 2>&1 || return 1
    cppcheck --version >/dev/null 2>&1 || return 1
    return 0
}

echo "=== ensure a working cppcheck ==="
if ! cppcheck_works; then
    echo "cppcheck missing or broken — installing..."
    pkgman install -y cppcheck || true
fi

# If it is present but still won't run, the on-disk package is corrupt
# (missing libcli.so / libcppcheck-core.so). Refresh the repo cache and
# reinstall from scratch to pull a clean copy and repair the activation.
if ! cppcheck_works && command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck present but not runnable — repairing package..."
    pkgman refresh               || true
    pkgman uninstall -y cppcheck || true
    pkgman install   -y cppcheck || true
fi

if ! cppcheck_works; then
    echo "ERROR: cppcheck could not be made to run after reinstall." >&2
    echo "       The static-analysis tool is broken in this environment;" >&2
    echo "       this is an infrastructure failure, not a code defect." >&2
    exit 1
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
