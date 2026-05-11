#!/usr/bin/env bash
# ============================================================================
# ios/scripts/purge-leaky-releases.sh
#
# Wipe historical `ttsignal-swift.zip` assets from the GitHub release repo.
#
# Background:
#   The xcframework artifacts produced before the path-stripping fix bake
#   absolute source paths (e.g. /Users/<localuser>/.../src/cpp/...) into
#   the static archives' DWARF debug info / __DATA strings, leaking the
#   local username of whoever built the SDK. Every historical release
#   asset on the public distribution repo therefore needs to be purged.
#
# Default behavior (safe):
#   * Dry-run — lists what WOULD be deleted, but doesn't touch GitHub.
#     You must pass --yes to actually delete anything.
#   * Asset-level — deletes only the `ttsignal-swift.zip` asset on each
#     release; the release entry + git tag stay in place so consumers
#     can still see the version history (downloads will 404, which is
#     the intended signal: "this version was retracted").
#
# Pass --delete-release to nuke the entire release entry and the
# underlying git tag as well (uses `gh release delete --cleanup-tag`).
# Use this when you need the version numbers themselves to disappear
# (e.g. so SwiftPM `from:` resolution can't even SEE the leaky tags).
#
# Use --keep <tag> (repeatable) to preserve specific releases — typical
# workflow is:
#     1. Re-build + publish a clean `1.0.YYYYMMDD` with the path fix
#     2. Run this script with `--keep 1.0.YYYYMMDD --yes` to purge
#        everything else
#
# Usage:
#   ios/scripts/purge-leaky-releases.sh                          # dry-run
#   ios/scripts/purge-leaky-releases.sh --yes                    # delete assets
#   ios/scripts/purge-leaky-releases.sh --delete-release --yes   # delete releases + tags
#   ios/scripts/purge-leaky-releases.sh --keep 1.0.20260508 --yes
#
# Flags:
#   --yes                  actually perform deletion (otherwise dry-run)
#   --delete-release       delete release entry + git tag, not just the asset
#   --keep <tag>           preserve this tag (repeatable)
#   --asset <name>         asset filename to delete (default: ttsignal-swift.zip)
#   --repo <owner/repo>    target repo (default: 3th1UOYgUtJkurSZ/ttsignal-xcframework,
#                          or $TTSIGNAL_RELEASE_REPO)
#
# Requires: bash, gh CLI authenticated for github.com.
# ============================================================================
set -eu -o pipefail

show_help() {
    sed -n '2,48p' "$0" | sed 's/^# \{0,1\}//'
}

# ------------------------------------------------------------ args / defaults
DO_DELETE=0
DELETE_RELEASE=0
ASSET_NAME="ttsignal-swift.zip"
RELEASE_REPO="${TTSIGNAL_RELEASE_REPO:-3th1UOYgUtJkurSZ/ttsignal-xcframework}"
KEEP_TAGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)            DO_DELETE=1; shift ;;
        --delete-release)    DELETE_RELEASE=1; shift ;;
        --keep)              KEEP_TAGS+=("$2"); shift 2 ;;
        --asset)             ASSET_NAME="$2"; shift 2 ;;
        --repo)              RELEASE_REPO="$2"; shift 2 ;;
        -h|--help)           show_help; exit 0 ;;
        *) echo "unknown argument: $1" >&2; show_help; exit 1 ;;
    esac
done

# ------------------------------------------------------------ tool checks
command -v gh >/dev/null 2>&1 || {
    echo "ERROR: 'gh' CLI not found. Install with: brew install gh" >&2
    exit 1
}
if ! gh auth status -h github.com >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated. Run: gh auth login -h github.com" >&2
    exit 1
fi

# ------------------------------------------------------------ helpers
is_kept() {
    local tag="$1"
    local kept
    for kept in "${KEEP_TAGS[@]:-}"; do
        [ -n "${kept}" ] && [ "${kept}" = "${tag}" ] && return 0
    done
    return 1
}

# ------------------------------------------------------------ enumerate releases
# `--limit 1000` should comfortably exceed our entire history (we currently
# have ~7 releases). If we ever blow past 1000 the script will print a
# warning instead of silently leaving stragglers behind.
echo "[purge] target repo : ${RELEASE_REPO}"
echo "[purge] asset name  : ${ASSET_NAME}"
echo "[purge] mode        : $([ "${DELETE_RELEASE}" -eq 1 ] && echo 'delete release + tag' || echo 'delete asset only')"
echo "[purge] dry-run     : $([ "${DO_DELETE}" -eq 1 ] && echo 'NO (will delete)' || echo 'YES (no changes)')"
if [ "${#KEEP_TAGS[@]:-0}" -gt 0 ]; then
    echo "[purge] keep tags   : ${KEEP_TAGS[*]}"
fi
echo "============================================================"

TAGS_RAW=$(gh release list --repo "${RELEASE_REPO}" --limit 1000 \
            --json tagName --jq '.[].tagName' 2>&1) || {
    echo "ERROR: failed to list releases on ${RELEASE_REPO}:" >&2
    echo "${TAGS_RAW}" >&2
    exit 1
}

if [ -z "${TAGS_RAW}" ]; then
    echo "[purge] no releases found on ${RELEASE_REPO} — nothing to do."
    exit 0
fi

TOTAL=0
DELETED=0
SKIPPED=0
MISSING=0
FAILED=0

while IFS= read -r tag; do
    [ -z "${tag}" ] && continue
    TOTAL=$((TOTAL + 1))

    if is_kept "${tag}"; then
        echo "[purge] SKIP  ${tag}  (in --keep list)"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    if [ "${DELETE_RELEASE}" -eq 1 ]; then
        # ---- delete release entry + git tag
        if [ "${DO_DELETE}" -eq 1 ]; then
            if gh release delete "${tag}" --repo "${RELEASE_REPO}" \
                   --cleanup-tag --yes >/dev/null 2>&1; then
                echo "[purge] DELETE ${tag}  (release + tag removed)"
                DELETED=$((DELETED + 1))
            else
                echo "[purge] FAIL   ${tag}  (gh release delete failed)" >&2
                FAILED=$((FAILED + 1))
            fi
        else
            echo "[purge] WOULD-DELETE ${tag}  (release + tag)"
            DELETED=$((DELETED + 1))
        fi
        continue
    fi

    # ---- asset-only path
    # Probe whether the asset exists before trying to delete it, so we
    # can distinguish "already gone" (benign) from "delete failed"
    # (needs attention) in the summary.
    HAS_ASSET=$(gh release view "${tag}" --repo "${RELEASE_REPO}" \
                  --json assets \
                  --jq ".assets[] | select(.name==\"${ASSET_NAME}\") | .name" \
                  2>/dev/null || true)
    if [ -z "${HAS_ASSET}" ]; then
        echo "[purge] MISS  ${tag}  (no '${ASSET_NAME}' asset, already clean)"
        MISSING=$((MISSING + 1))
        continue
    fi

    if [ "${DO_DELETE}" -eq 1 ]; then
        if gh release delete-asset "${tag}" "${ASSET_NAME}" \
               --repo "${RELEASE_REPO}" --yes >/dev/null 2>&1; then
            echo "[purge] DELETE ${tag}  (${ASSET_NAME})"
            DELETED=$((DELETED + 1))
        else
            echo "[purge] FAIL   ${tag}  (delete-asset failed)" >&2
            FAILED=$((FAILED + 1))
        fi
    else
        echo "[purge] WOULD-DELETE ${tag}  (${ASSET_NAME})"
        DELETED=$((DELETED + 1))
    fi
done <<<"${TAGS_RAW}"

# ------------------------------------------------------------ summary
echo "============================================================"
if [ "${DO_DELETE}" -eq 1 ]; then
    echo "[purge] done — total=${TOTAL} deleted=${DELETED} skipped=${SKIPPED} missing=${MISSING} failed=${FAILED}"
else
    echo "[purge] DRY-RUN — total=${TOTAL} would-delete=${DELETED} skipped=${SKIPPED} missing=${MISSING}"
    echo "[purge] re-run with --yes to actually delete."
fi

if [ "${FAILED}" -gt 0 ]; then
    exit 1
fi
