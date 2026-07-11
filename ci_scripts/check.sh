#!/usr/bin/env bash
# Static analysis and security audit. Runs on Haiku, invoked from the
# project root.
#
# cppcheck is treated as a BEST-EFFORT gate. The Haiku cppcheck-2.21.1-1
# package is currently broken at the source: its bin/cppcheck is linked
# against libcli.so / libcppcheck-core.so / libsimplecpp.so, but those
# libraries are shipped by no package and exist nowhere on the system, so
# `cppcheck --version` fails with a runtime-loader error. That is an
# upstream HaikuPorts packaging bug, not a defect in this codebase.
#
# Rather than hard-fail CI on broken tooling, we verify cppcheck actually
# runs; if it cannot (even after a reinstall attempt) we log a loud
# warning and SKIP lint. Build and test remain the hard gates. flawfinder
# is optional and skipped when absent.

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
    echo "cppcheck missing or broken — attempting install..."
    pkgman install -y cppcheck || true
fi

# If still broken, try one clean reinstall in case it is a corrupt
# activation rather than the known upstream packaging bug.
if ! cppcheck_works && command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck present but not runnable — attempting repair..."
    pkgman refresh               || true
    pkgman uninstall -y cppcheck || true
    pkgman install   -y cppcheck || true
fi

if ! cppcheck_works; then
    echo "::warning::cppcheck is unusable in this environment — SKIPPING lint."
    echo "WARNING: cppcheck could not be made to run (missing libcli.so /"
    echo "         libcppcheck-core.so — an upstream HaikuPorts packaging bug,"
    echo "         not a code defect). Static analysis is skipped; build and"
    echo "         test remain the enforced gates. Re-enable lint once a fixed"
    echo "         cppcheck package is available."
    # Run flawfinder alone if it happens to be present, then exit clean.
    if command -v flawfinder >/dev/null 2>&1; then
        echo "=== flawfinder available — running security audit only ==="
        make security
    fi
    exit 0
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
