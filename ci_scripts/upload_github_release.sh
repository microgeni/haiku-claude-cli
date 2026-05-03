#!/usr/bin/env bash
# Upload a built HPKG and a generated source tarball to a GitHub
# release for the current tag. Creates (or replaces) the release,
# attaches both assets plus SHA256SUMS, and prints the final URL.
#
# Required environment:
#   VERSION          — tag ref (e.g. "v1.7.1")
#   GITHUB_TOKEN     — GitHub personal-access-token (or Actions token)
#                      with Contents: write scope on the target repo
#   PKG_NAME         — filename of the HPKG in ./build/
#
# Optional:
#   GITHUB_REPO      — defaults to microgeni/haiku-claude-cli
#   GITHUB_API_URL   — defaults to https://api.github.com
#   GITHUB_UPLOAD_URL — defaults to https://uploads.github.com

set -euo pipefail

: "${VERSION:?VERSION required}"
: "${GH_RELEASE_TOKEN:?GH_RELEASE_TOKEN required}"
: "${PKG_NAME:?PKG_NAME required}"

API="${GITHUB_API_URL:-https://api.github.com}"
UPLOAD="${GITHUB_UPLOAD_URL:-https://uploads.github.com}"
REPO="${GITHUB_REPO:-microgeni/haiku-claude-cli}"
TAG="$VERSION"
version_num="${VERSION#v}"
archive="haiku-claude-cli-${version_num}.tar.gz"

AUTH_HEADER="Authorization: Bearer $GH_RELEASE_TOKEN"
ACCEPT_HEADER="Accept: application/vnd.github+json"
API_VER_HEADER="X-GitHub-Api-Version: 2022-11-28"

# Sanity-check: built HPKG must exist.
[ -f "build/$PKG_NAME" ] || { echo "error: build/$PKG_NAME not found"; exit 1; }

# Build source tarball from the current HEAD.
echo "Creating source archive ${archive}..."
git archive --format=tar.gz --prefix="haiku-claude-cli-${version_num}/" HEAD > "$archive"

# Compute SHA-256 checksums.
hpkg_sha=$(shasum -a 256 "build/$PKG_NAME" | awk '{print $1}')
arch_sha=$(shasum -a 256 "$archive"         | awk '{print $1}')

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

## Downloads
- Haiku package: \`${PKG_NAME}\`
- Source: \`${archive}\`

## Install (Haiku)

    pkgman install ./${PKG_NAME}

## Checksums (SHA256)

    ${hpkg_sha}  ${PKG_NAME}
    ${arch_sha}  ${archive}
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

resp=$(curl -s -X POST \
    -H "$AUTH_HEADER" \
    -H "$ACCEPT_HEADER" \
    -H "$API_VER_HEADER" \
    -H "Content-Type: application/json" \
    -d "$payload" \
    "$API/repos/$REPO/releases")

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

# ── Upload assets via the upload endpoint ──
upload_asset() {
    local filepath="$1"
    local name
    name=$(basename "$filepath")
    local mime="application/octet-stream"
    echo "Uploading ${name}..."
    curl -s -X POST \
        -H "$AUTH_HEADER" \
        -H "$ACCEPT_HEADER" \
        -H "$API_VER_HEADER" \
        -H "Content-Type: ${mime}" \
        --data-binary @"$filepath" \
        "${UPLOAD}/repos/$REPO/releases/$rel_id/assets?name=${name}" >/dev/null
}

upload_asset "build/$PKG_NAME"
upload_asset "$archive"

# ── Upload SHA256SUMS ──
echo "Uploading SHA256SUMS..."
echo "${hpkg_sha}  ${PKG_NAME}" >  SHA256SUMS
echo "${arch_sha}  ${archive}"  >> SHA256SUMS
curl -s -X POST \
    -H "$AUTH_HEADER" \
    -H "$ACCEPT_HEADER" \
    -H "$API_VER_HEADER" \
    -H "Content-Type: text/plain" \
    --data-binary @SHA256SUMS \
    "${UPLOAD}/repos/$REPO/releases/$rel_id/assets?name=SHA256SUMS" >/dev/null

echo "GitHub Release published: https://github.com/$REPO/releases/tag/$TAG"
