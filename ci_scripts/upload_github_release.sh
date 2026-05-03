#!/usr/bin/env bash
# Upload the HPKG to a GitHub release for the current tag.
# Creates (or replaces) the release and attaches the single package asset.
#
# Required environment:
#   VERSION          — tag ref (e.g. "v1.7.2")
#   GH_RELEASE_TOKEN — GitHub PAT with Contents: write scope
#   PKG_NAME         — filename of the HPKG in ./build/
#
# Optional:
#   GH_REPO          — defaults to microgeni/haiku-claude-cli
#   GH_API_URL       — defaults to https://api.github.com
#   GH_UPLOAD_URL    — defaults to https://uploads.github.com

set -euo pipefail

: "${VERSION:?VERSION required}"
: "${GH_RELEASE_TOKEN:?GH_RELEASE_TOKEN required}"
: "${PKG_NAME:?PKG_NAME required}"

API="${GH_API_URL:-https://api.github.com}"
UPLOAD="${GH_UPLOAD_URL:-https://uploads.github.com}"
REPO="${GH_REPO:-microgeni/haiku-claude-cli}"
TAG="$VERSION"
version_num="${VERSION#v}"

AUTH_HEADER="Authorization: Bearer $GH_RELEASE_TOKEN"
ACCEPT_HEADER="Accept: application/vnd.github+json"
API_VER_HEADER="X-GitHub-Api-Version: 2022-11-28"

# Sanity-check: verify we can reach the GitHub API.
echo "Testing connectivity to $API..."
curl -sS --fail-with-body -H "$ACCEPT_HEADER" "$API/zen" 2>&1 || {
    echo "error: cannot reach $API — check network/TLS on this runner"
    exit 1
}
echo ""

# Sanity-check: built HPKG must exist.
[ -f "build/$PKG_NAME" ] || { echo "error: build/$PKG_NAME not found"; exit 1; }

# Pull the relevant section from CHANGELOG.md (if present).
changelog_section=""
if [ -f CHANGELOG.md ]; then
    changelog_section=$(awk -v ver="$version_num" '
        $0 ~ "^## \\["ver"\\]"      { in_section=1; print; next }
        in_section && $0 ~ "^## \\[" { exit }
        in_section                  { print }
    ' CHANGELOG.md)
fi
if [ -z "$changelog_section" ]; then
    changelog_section="## haiku-claude-cli ${version_num}

Native Claude CLI for Haiku OS."
fi

body=$(cat <<EOF
${changelog_section}

## Install (Haiku)

    pkgman install ./${PKG_NAME}
EOF
)

# ── Delete any existing release for this tag (makes re-runs idempotent) ──
echo "Checking for existing GitHub release at tag $TAG..."
existing=$(curl -s \
    -H "$AUTH_HEADER" \
    -H "$ACCEPT_HEADER" \
    -H "$API_VER_HEADER" \
    "$API/repos/$REPO/releases/tags/$TAG" 2>/dev/null || true)

existing_id=$(python3 -c '
import sys, json
try:
    d = json.loads(sys.stdin.read())
    print(d.get("id", "") if isinstance(d, dict) else "")
except Exception:
    pass
' <<<"$existing" 2>/dev/null || true)

if [ -n "$existing_id" ]; then
    echo "Deleting previous release id=$existing_id..."
    curl -s -X DELETE \
        -H "$AUTH_HEADER" \
        -H "$ACCEPT_HEADER" \
        -H "$API_VER_HEADER" \
        "$API/repos/$REPO/releases/$existing_id" >/dev/null || true
    sleep 1
fi

# ── Create the release ──
echo "Creating GitHub release $TAG..."
payload=$(TAG="$TAG" VERSION_NUM="$version_num" BODY="$body" python3 -c '
import json, os
name = "haiku-claude-cli " + os.environ["VERSION_NUM"]
print(json.dumps({
    "tag_name":               os.environ["TAG"],
    "name":                   name,
    "body":                   os.environ["BODY"],
    "draft":                  False,
    "prerelease":             False,
    "make_latest":            "true",
    "generate_release_notes": False,
}))')

resp=$(curl -sS --fail-with-body -X POST \
    -H "$AUTH_HEADER" \
    -H "$ACCEPT_HEADER" \
    -H "$API_VER_HEADER" \
    -H "Content-Type: application/json" \
    -d "$payload" \
    "$API/repos/$REPO/releases" 2>&1 || true)

rel_id=$(python3 -c '
import sys, json
try:
    print(json.loads(sys.stdin.read()).get("id", ""))
except Exception:
    pass
' <<<"$resp" 2>/dev/null || true)

if [ -z "$rel_id" ]; then
    echo "error: could not create GitHub release"
    echo "$resp"
    exit 1
fi
echo "Created release id=$rel_id"

# ── Upload the HPKG ──
echo "Uploading $PKG_NAME..."
curl -sS --fail-with-body -X POST \
    -H "$AUTH_HEADER" \
    -H "$ACCEPT_HEADER" \
    -H "$API_VER_HEADER" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"build/$PKG_NAME" \
    "${UPLOAD}/repos/$REPO/releases/$rel_id/assets?name=${PKG_NAME}" >/dev/null

echo "GitHub Release published: https://github.com/$REPO/releases/tag/$TAG"
