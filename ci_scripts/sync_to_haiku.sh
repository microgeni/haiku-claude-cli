#!/usr/bin/env bash
# Sync the repo to Taurus for a CI build/test run.
# Runs on the CI runner; SSHes to Haiku.
#
# Required environment:
#   HAIKU_HOST, HAIKU_USER, HAIKU_PROJECT_PATH, GITEA_REPO_URL, REF

set -euo pipefail

: "${HAIKU_HOST:?HAIKU_HOST required}"
: "${HAIKU_USER:?HAIKU_USER required}"
: "${HAIKU_PROJECT_PATH:?HAIKU_PROJECT_PATH required}"
: "${GITEA_REPO_URL:?GITEA_REPO_URL required}"
: "${REF:?REF required}"

ssh -o StrictHostKeyChecking=no "$HAIKU_USER@$HAIKU_HOST" \
    bash -s "$HAIKU_PROJECT_PATH" "$GITEA_REPO_URL" "$REF" <<'REMOTE'
set -euo pipefail
project_path="$1"
repo_url="$2"
ref="$3"

if [ ! -d "$project_path/.git" ]; then
    rm -rf "$project_path"
    mkdir -p "$(dirname "$project_path")"
    git clone "$repo_url" "$project_path"
fi

cd "$project_path"
git fetch origin --tags --force

if git tag -l "$ref" | grep -q "^${ref}$"; then
    git checkout -f "$ref"
else
    git checkout -f "$ref" 2>/dev/null || git checkout -b "$ref" "origin/$ref"
    git reset --hard "origin/$ref"
fi

git clean -fd
echo "Synced to $(git rev-parse --short HEAD) on $ref"
REMOTE
