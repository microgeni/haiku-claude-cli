#!/usr/bin/env bash
# Static analysis and security audit. Runs on Haiku, invoked from the
# project root. Requires cppcheck and flawfinder to be installed.
# Exits non-zero if either tool reports a finding or is not present.

set -euo pipefail

echo "=== cppcheck version ==="
cppcheck --version

echo "=== flawfinder version ==="
flawfinder --version

echo "=== make check (lint + security) ==="
make check
