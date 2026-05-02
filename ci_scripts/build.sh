#!/usr/bin/env bash
# Build the CLI. Runs on Haiku, invoked from the project root.

set -euo pipefail

echo "=== gcc ==="
gcc --version | head -1

echo "=== ensure libedit_devel ==="
if ! pkg-config --exists libedit 2>/dev/null; then
    echo "libedit devel headers missing, installing libedit_devel..."
    pkgman install -y libedit_devel
fi

echo "=== make clean ==="
make clean || true

echo "=== make ==="
make -j$(nproc)

echo "=== artifact ==="
ls -la build/claude
file build/claude
