#!/usr/bin/env bash
# Build the HPKG on Haiku for a tagged release.
# Runs on the CI runner; SSHes to Haiku, then scp's the resulting
# .hpkg back to the runner's `build/` directory.
#
# Required environment:
#   HAIKU_HOST, HAIKU_USER, HAIKU_PROJECT_PATH
#   VERSION        — the tag ref (e.g. "v0.1.0"); leading "v" is stripped
#   BUILD_NUMBER   — CI run number, used as the HPKG build component
#
# On success, sets $PKG_NAME in $GITHUB_OUTPUT (or prints it if unset).

set -euo pipefail

: "${HAIKU_HOST:?HAIKU_HOST required}"
: "${HAIKU_USER:?HAIKU_USER required}"
: "${HAIKU_PROJECT_PATH:?HAIKU_PROJECT_PATH required}"
: "${VERSION:?VERSION required}"
: "${BUILD_NUMBER:?BUILD_NUMBER required}"

version="${VERSION#v}"
pkg_name="claude_cli-${version}-${BUILD_NUMBER}-x86_64.hpkg"

ssh -o StrictHostKeyChecking=no "$HAIKU_USER@$HAIKU_HOST" \
    bash -s "$HAIKU_PROJECT_PATH" "$version" "$BUILD_NUMBER" <<'REMOTE'
set -euo pipefail
project_path="$1"
version="$2"
build_number="$3"
cd "$project_path"
make clean
make
make package PKG_VERSION="$version" PKG_BUILD="$build_number"
REMOTE

mkdir -p build
scp -o StrictHostKeyChecking=no \
    "$HAIKU_USER@$HAIKU_HOST:$HAIKU_PROJECT_PATH/build/$pkg_name" \
    "build/$pkg_name"

ls -l "build/$pkg_name"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "pkg_name=$pkg_name" >> "$GITHUB_OUTPUT"
fi
echo "PKG_NAME=$pkg_name"
