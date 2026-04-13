#!/usr/bin/env bash
# Build the CLI. Runs on Haiku, invoked from the project root.

set -euo pipefail

echo "=== gcc ==="
gcc --version | head -1

echo "=== make clean ==="
make clean || true

echo "=== make ==="
make

echo "=== artifact ==="
ls -la build/claude
file build/claude
