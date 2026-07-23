#!/usr/bin/env bash
# version_check.sh — guard against release version drift.
#
# The VERSION file is the single source of truth: the Makefile reads it into
# PKG_VERSION and the binary's config::kVersion is compiled from it via
# -DCCH_VERSION, so those three can never disagree. What CAN drift is the
# CHANGELOG: the binary can advertise a version that has no changelog section.
#
# This check fails if the version in VERSION has no matching
# "## [x.y.z]" heading in CHANGELOG.md. The "[Unreleased]" section is
# ignored. Run from the project root; wired into `make version-check` and
# `make check`.

set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
version_file="$root_dir/VERSION"
changelog="$root_dir/CHANGELOG.md"

fail() { echo "version-check: FAIL: $*" >&2; exit 1; }

[ -f "$version_file" ] || fail "VERSION file not found at $version_file"
[ -f "$changelog" ]    || fail "CHANGELOG.md not found at $changelog"

version="$(tr -d '[:space:]' < "$version_file")"
[ -n "$version" ] || fail "VERSION file is empty"

# Basic semver sanity: MAJOR.MINOR.PATCH with optional -suffix.
if ! printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$'; then
    fail "VERSION '$version' is not a valid semver string"
fi

# A pre-release/dev suffix (e.g. 1.11.1-dev) is allowed to lack a changelog
# section — it isn't a shipped release yet.
case "$version" in
    *-*)
        echo "version-check: OK — '$version' is a pre-release; changelog section not required."
        exit 0
        ;;
esac

# Look for "## [x.y.z]" — the Keep-a-Changelog release heading. Escape the
# dots so grep treats them literally.
escaped="$(printf '%s' "$version" | sed 's/\./\\./g')"
if grep -Eq "^## \[$escaped\]" "$changelog"; then
    echo "version-check: OK — VERSION '$version' has a matching CHANGELOG.md section."
    exit 0
fi

fail "VERSION is '$version' but CHANGELOG.md has no '## [$version]' section.
      Add a '## [$version] - YYYY-MM-DD' entry (see the release checklist in
      CLAUDE.md), or bump VERSION back to a '-dev' pre-release while iterating."
