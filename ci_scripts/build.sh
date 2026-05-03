#!/usr/bin/env bash
# Build the CLI. Runs on Haiku, invoked from the project root.

set -euo pipefail

echo "=== gcc ==="
gcc --version | head -1

echo "=== ensure build dependencies ==="
install_if_missing() {
    local pkg="$1"
    local pc="$2"
    if ! pkg-config --exists "$pc" 2>/dev/null; then
        echo "  $pc headers missing, installing $pkg..."
        pkgman install -y "$pkg"
    else
        echo "  $pc already present."
    fi
}
install_if_missing libedit_devel       libedit
install_if_missing curl_devel          libcurl
install_if_missing openssl3_devel      openssl
install_if_missing nlohmann_json       nlohmann_json

echo "=== make clean ==="
make clean || true

echo "=== make ==="
make -j$(nproc)

echo "=== artifact ==="
ls -la build/claude
file build/claude 2>/dev/null || true
