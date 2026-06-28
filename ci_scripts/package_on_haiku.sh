#!/usr/bin/env bash
# Build the HPKG for a tagged release.
# Runs directly on the Haiku CI runner (no SSH needed).
#
# Required environment:
#   VERSION       — the tag ref (e.g. "v1.7.1"); leading "v" is stripped
#   BUILD_NUMBER  — CI run number, used as the HPKG build component
#
# On success, writes pkg_name to $GITHUB_OUTPUT (or prints it if unset).

set -euo pipefail

: "${VERSION:?VERSION required}"
: "${BUILD_NUMBER:?BUILD_NUMBER required}"

version="${VERSION#v}"
pkg_name="claude_cli-${version}-${BUILD_NUMBER}-x86_64.hpkg"

# The HPKG bundles the GUI app (apps/Claude), which `make package` builds as
# a prerequisite. The GUI links yaml-cpp (for the Genio-theme syntax
# highlighter), so its -devel package must be present — the CLI build step
# does not install it. Install it here if missing so the GUI compile finds
# <yaml-cpp/yaml.h>.
if ! pkg-config --exists yaml-cpp 2>/dev/null; then
    echo "  yaml-cpp headers missing, installing yaml_cpp0.8_devel..."
    pkgman install -y yaml_cpp0.8_devel
fi

make clean
make
make package PKG_VERSION="$version" PKG_BUILD="$BUILD_NUMBER"

ls -l "build/$pkg_name"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "pkg_name=$pkg_name" >> "$GITHUB_OUTPUT"
fi
echo "PKG_NAME=$pkg_name"
