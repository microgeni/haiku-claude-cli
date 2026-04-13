#!/usr/bin/env bash
# Upload a built HPKG and a generated source tarball to the Gitea
# release for the current tag. Creates (or replaces) the release,
# attaches both assets, and prints the final release URL.
#
# Required environment:
#   VERSION         — tag ref (e.g. "v0.1.0")
#   RELEASE_TOKEN   — Gitea API token with release:write
#   PKG_NAME        — filename of the HPKG in ./build/
#
# Optional:
#   GITEA_API_URL   — defaults to https://gitea.microgeni.synology.me/api/v1
#   GITEA_REPO      — defaults to daniel/haiku-claude-cli

set -euo pipefail

: "${VERSION:?VERSION required}"
: "${RELEASE_TOKEN:?RELEASE_TOKEN required}"
: "${PKG_NAME:?PKG_NAME required}"

API="${GITEA_API_URL:-https://gitea.microgeni.synology.me/api/v1}"
REPO="${GITEA_REPO:-daniel/haiku-claude-cli}"
TAG="$VERSION"
version_num="${VERSION#v}"
archive="haiku-claude-cli-${version_num}.tar.gz"

[ -f "build/$PKG_NAME" ] || { echo "error: build/$PKG_NAME not found"; exit 1; }

git archive --format=tar.gz --prefix="haiku-claude-cli-${version_num}/" HEAD > "$archive"

hpkg_sha=$(shasum -a 256 "build/$PKG_NAME" | awk '{print $1}')
arch_sha=$(shasum -a 256 "$archive"         | awk '{print $1}')

changelog_section=""
if [ -f CHANGELOG.md ]; then
    changelog_section=$(awk -v ver="$version_num" '
        $0 ~ "^## \\["ver"\\]"     { in_section=1; print; next }
        in_section && $0 ~ "^## \\[" { exit }
        in_section                 { print }
    ' CHANGELOG.md)
fi
if [ -z "$changelog_section" ]; then
    changelog_section="## haiku-claude-cli ${version_num}

Minimal Claude CLI for Haiku OS."
fi

body=$(cat <<EOF
${changelog_section}

## Downloads
- Haiku package: \`${PKG_NAME}\`
- Source: \`${archive}\`

## Install

    pkgman install ./${PKG_NAME}

## Checksums (SHA256)

    ${hpkg_sha}  ${PKG_NAME}
    ${arch_sha}  ${archive}
EOF
)

# Delete any existing release for this tag so re-running the job is idempotent.
existing=$(curl -s -H "Authorization: token $RELEASE_TOKEN" \
    "$API/repos/$REPO/releases/tags/$TAG" || true)
existing_id=$(python3 -c 'import sys,json
try:
    d=json.loads(sys.stdin.read())
    print(d.get("id","") if isinstance(d,dict) else "")
except Exception:
    pass' <<<"$existing" 2>/dev/null || true)

if [ -n "$existing_id" ]; then
    echo "deleting previous release id=$existing_id"
    curl -s -X DELETE -H "Authorization: token $RELEASE_TOKEN" \
        "$API/repos/$REPO/releases/$existing_id" >/dev/null || true
    sleep 1
fi

payload=$(TAG="$TAG" VERSION_NUM="$version_num" BODY="$body" python3 -c '
import json, os
name = "haiku-claude-cli " + os.environ["VERSION_NUM"]
print(json.dumps({
    "tag_name":   os.environ["TAG"],
    "name":       name,
    "body":       os.environ["BODY"],
    "draft":      False,
    "prerelease": False,
}))')

resp=$(curl -s -X POST \
    -H "Authorization: token $RELEASE_TOKEN" \
    -H "Content-Type: application/json" \
    -d "$payload" \
    "$API/repos/$REPO/releases")

rel_id=$(python3 -c 'import sys,json
try:
    print(json.loads(sys.stdin.read()).get("id",""))
except Exception:
    pass' <<<"$resp" 2>/dev/null || true)

if [ -z "$rel_id" ]; then
    echo "error: could not create release"
    echo "$resp"
    exit 1
fi
echo "created release id=$rel_id"

for asset in "build/$PKG_NAME" "$archive"; do
    name=$(basename "$asset")
    echo "uploading $name..."
    curl -s -X POST \
        -H "Authorization: token $RELEASE_TOKEN" \
        -H "Content-Type: application/octet-stream" \
        --data-binary @"$asset" \
        "$API/repos/$REPO/releases/$rel_id/assets?name=$name" >/dev/null
done

echo "Release published: https://gitea.microgeni.synology.me/$REPO/releases/tag/$TAG"
