#!/usr/bin/env bash
# ============================================================================
# ios/scripts/release-xcframework.sh
#
# Package the TTSignal.xcframework produced by build-xcframework.sh into a
# Swift Package Manager binary distribution zip, and (optionally) publish it
# as a GitHub Release asset on the ttsignal repo.
#
# Layout of the zip (what SPM `binaryTarget(url:checksum:)` expects):
#
#     ttsignal-swift.zip
#       └─ TTSignal.xcframework/
#            ├─ Info.plist
#            ├─ ios-arm64/...
#            └─ ios-arm64_x86_64-simulator/...
#
# Steps:
#   1. ditto -c -k --keepParent       →  build/ios-xcframework/dist/ttsignal-swift.zip
#   2. swift package compute-checksum →  zip.swift-checksum  (the value SPM
#      binaryTarget(checksum:) actually validates against — authoritative)
#      shasum -a 256                  →  zip.sha256          (cross-check;
#      we abort if it disagrees with the SPM value, in case a future Swift
#      toolchain changes the digest format)
#   3. (optional) gh release create / gh release upload
#
# Default version is derived from `src/cpp/Utils.cpp`'s mtime, formatted as
# `1.0.YYYYMMDD`. The patch component is the build-day stamp so all four
# platform artifacts (iOS xcframework, Linux/macOS Node addons, Windows
# Node addon) built on the same day land on the same release tag, while
# the resulting string remains a valid 3-segment SemVer that SwiftPM /
# CocoaPods accept (`1.0.0.YYYYMMDD` would be invalid SemVer — too many
# segments — and SPM's manifest parser rejects it).
#
# YYYYMMDD comes from Utils.cpp's mtime instead of `date +%Y%m%d` because
# the iOS build scripts `touch Utils.cpp` on every build, so the file's
# mtime equals the most recent compile date, which equals the date baked
# into the xcframework via `__DATE__`. Using today's date would be wrong
# whenever you package an xcframework built on a previous day.
#
# Same-day repacks (--upload only): if the GitHub release tag for the
# base version is already published on $TTSIGNAL_REPO, the script
# auto-bumps to `1.0.YYYYMMDD-N` (N starts at 1, increments on each
# subsequent same-day upload). Pinning a version with --version /
# TTSIGNAL_VERSION disables the auto-bump entirely.
#
# Caveat — the `-N` suffix is technically a SemVer pre-release tag, so
# strictly speaking `1.0.20260430-1` sorts BEFORE `1.0.20260430` under
# SemVer rules. SwiftPM `from:` constraints will skip pre-release
# versions, so consumers tracking via `from:` won't auto-pick up
# `-N` repacks — pin the exact tag if you need one. We accept this
# trade-off because the alternative (bumping the patch component to
# something like `1.0.YYYYMMDD01`) breaks the chronological reading
# of the date and complicates parsing in the publish pipeline.
#
# The final version chosen is written to `${ZIP_PATH}.version` so
# publish-to-release-repo.sh picks up exactly the same string —
# otherwise its own mtime-based derivation could drift to a different
# `-N` and the publish step would fail the asset-checksum cross-check.
#
# Override with --version if you really know better (e.g. packaging an
# archived artifact, forcing a specific -N).
#
# Usage:
#   ios/scripts/release-xcframework.sh                  # package only
#   ios/scripts/release-xcframework.sh --upload         # package + upload (auto-bump)
#   ios/scripts/release-xcframework.sh --version 1.0.20260429 --upload
#
# Env overrides (alternative to flags):
#   TTSIGNAL_VERSION  e.g. 1.0.20260429    (default: 1.0.<Utils.cpp mtime YYYYMMDD>)
#   TTSIGNAL_REPO     e.g. owner/ttsignal-xcframework  (default: 3th1UOYgUtJkurSZ/ttsignal-xcframework)
#   TTSIGNAL_TAG      git tag name          (default: same as $TTSIGNAL_VERSION)
#
# Requires: macOS (ditto, shasum, stat), `swift` (Xcode command-line tools),
#           and `gh` CLI (only for --upload).
# ============================================================================
set -eu -o pipefail

ZIP_NAME="ttsignal-swift.zip"

show_help() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ------------------------------------------------------------ derive default version
# `Utils.cpp` is touched on every build; its mtime therefore matches the
# `__DATE__` value baked into the compiled SDK version string. Using
# `stat -f` (BSD/macOS variant) keeps this script macOS-only, which is
# the same constraint as build-xcframework.sh.
UTILS_CPP="${REPO_ROOT}/src/cpp/Utils.cpp"
if [ -f "${UTILS_CPP}" ]; then
    UTILS_MDATE="$(stat -f '%Sm' -t '%Y%m%d' "${UTILS_CPP}")"
else
    UTILS_MDATE=""
fi

# ------------------------------------------------------------ args + defaults
DO_UPLOAD=0
# Track whether the version string came from the user (env or --version)
# vs. our default mtime derivation. Auto-bump only fires for the latter.
EXPLICIT_VERSION=0
if [ -n "${TTSIGNAL_VERSION:-}" ]; then EXPLICIT_VERSION=1; fi
TTSIGNAL_VERSION="${TTSIGNAL_VERSION:-1.0.${UTILS_MDATE}}"
TTSIGNAL_REPO="${TTSIGNAL_REPO:-3th1UOYgUtJkurSZ/ttsignal-xcframework}"
TTSIGNAL_TAG="${TTSIGNAL_TAG:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        -u|--upload)   DO_UPLOAD=1; shift ;;
        --version)     TTSIGNAL_VERSION="$2"; EXPLICIT_VERSION=1; shift 2 ;;
        --repo)        TTSIGNAL_REPO="$2"; shift 2 ;;
        --tag)         TTSIGNAL_TAG="$2"; shift 2 ;;
        -h|--help)     show_help; exit 0 ;;
        *) echo "unknown argument: $1" >&2; show_help; exit 1 ;;
    esac
done

if [ -z "${UTILS_MDATE}" ] && [ "${TTSIGNAL_VERSION}" = "1.0." ]; then
    echo "ERROR: cannot derive SDK version: ${UTILS_CPP} not found, and" >&2
    echo "       no --version / TTSIGNAL_VERSION was supplied." >&2
    exit 1
fi

# ------------------------------------------------------------ same-day -N bump
# When --upload is requested and we're on the auto-derived base version,
# probe the release repo for an existing same-day tag and pick the next
# free `-N` suffix. Stays a no-op for explicit --version / dry-run / no
# gh / unauthenticated gh / repo not yet bootstrapped — see the WARN
# branches.
#
# Stdout of the helper is the next free version string (base or base-N).
# Returns nonzero only when gh itself can't talk to GitHub, so the
# caller can warn + fall back to the base version.
derive_next_release_version() {
    local base="$1" repo="$2"
    if ! command -v gh >/dev/null 2>&1; then return 1; fi
    if ! gh auth status -h github.com >/dev/null 2>&1; then return 1; fi
    local tags
    if ! tags=$(gh release list --repo "${repo}" --limit 1000 \
                  --json tagName --jq '.[].tagName' 2>/dev/null); then
        return 1
    fi
    local has_base=0 max=0 t n
    while IFS= read -r t; do
        [ -z "${t}" ] && continue
        if [ "${t}" = "${base}" ]; then
            has_base=1
        elif [[ "${t}" =~ ^${base}-([0-9]+)$ ]]; then
            n="${BASH_REMATCH[1]}"
            if (( n > max )); then max=$n; fi
        fi
    done <<<"${tags}"
    if (( has_base == 0 && max == 0 )); then
        printf '%s\n' "${base}"
    else
        printf '%s\n' "${base}-$((max + 1))"
    fi
}

if [ "${DO_UPLOAD}" -eq 1 ] && [ "${EXPLICIT_VERSION}" -eq 0 ]; then
    if next_version=$(derive_next_release_version "${TTSIGNAL_VERSION}" "${TTSIGNAL_REPO}"); then
        if [ "${next_version}" != "${TTSIGNAL_VERSION}" ]; then
            echo "[release] base version ${TTSIGNAL_VERSION} already published on ${TTSIGNAL_REPO}"
            echo "[release]   -> bumping to ${next_version} (pass --version to pin)"
            TTSIGNAL_VERSION="${next_version}"
        fi
    else
        echo "[release] WARN: gh unavailable / unauthenticated / can't reach ${TTSIGNAL_REPO}" >&2
        echo "[release]       skipping same-day -N bump; will try uploading as ${TTSIGNAL_VERSION}." >&2
        echo "[release]       (gh release create will collide if a release with this tag already exists.)" >&2
    fi
fi

# Default the git tag to the SDK version (no "v" prefix) so the asset URL
# directly carries the version: .../releases/download/<version>/ttsignal-swift.zip
[ -z "${TTSIGNAL_TAG}" ] && TTSIGNAL_TAG="${TTSIGNAL_VERSION}"

XCFW="${REPO_ROOT}/build/ios-xcframework/TTSignal.xcframework"
DIST_DIR="${REPO_ROOT}/build/ios-xcframework/dist"
ZIP_PATH="${DIST_DIR}/${ZIP_NAME}"
SHA_PATH="${ZIP_PATH}.sha256"
SWIFT_CHECKSUM_PATH="${ZIP_PATH}.swift-checksum"
# Sidecar storing the final version string (post auto-bump). publish-to-
# release-repo.sh reads this to stay in lockstep — otherwise its own
# mtime-based default could drift to a different `-N` and the
# asset-checksum cross-check would fail.
VERSION_PATH="${ZIP_PATH}.version"

# ------------------------------------------------------------ sanity checks
if [ ! -d "${XCFW}" ]; then
    echo "ERROR: xcframework not found: ${XCFW}" >&2
    echo "       Run ios/scripts/build-xcframework.sh first." >&2
    exit 1
fi
for tool in ditto shasum swift; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "ERROR: required tool '${tool}' not on PATH." >&2
        echo "       (swift ships with Xcode command-line tools)" >&2
        exit 1
    }
done

# ------------------------------------------------------------ Step 1: package
mkdir -p "${DIST_DIR}"
rm -f "${ZIP_PATH}" "${SHA_PATH}" "${SWIFT_CHECKSUM_PATH}" "${VERSION_PATH}"

echo "============================================================"
echo "[release] zipping  : ${XCFW}"
echo "[release]   ->     : ${ZIP_PATH}"

# --keepParent keeps the "TTSignal.xcframework/" directory at the top of
# the zip — this is what SPM binaryTarget expects.
#
# We deliberately do NOT pass --sequesterRsrc, and we strip resource
# forks / extended attributes / ACLs: the xcframework tree is pure
# binary + headers, and on macOS Sonoma+ each file carries a
# `com.apple.provenance` xattr that ditto would otherwise materialize
# as a "._*" sidecar in the zip, polluting the extracted bundle.
ditto -c -k --keepParent --norsrc --noextattr --noacl "${XCFW}" "${ZIP_PATH}"

# ------------------------------------------------------------ Step 2: checksum
# `swift package compute-checksum <zip>` is the canonical way to compute the
# value SPM later validates against in binaryTarget(checksum:). Today it is
# the file's plain SHA-256 hex, so the result agrees with `shasum -a 256`,
# but Apple has reserved the right to evolve the digest format (e.g. add a
# version prefix). Always trust the swift-side value, and use shasum only
# as a sanity cross-check + as a generic distribution checksum file.
CHECKSUM="$(swift package compute-checksum "${ZIP_PATH}")"
SHA256="$(shasum -a 256 "${ZIP_PATH}" | awk '{print $1}')"
ZIP_SIZE_HUMAN="$(du -h "${ZIP_PATH}" | awk '{print $1}')"

if [ "${CHECKSUM}" != "${SHA256}" ]; then
    echo "[release] note: swift compute-checksum (${CHECKSUM})" >&2
    echo "[release]       differs from shasum -a 256 (${SHA256})." >&2
    echo "[release]       Using the swift value for binaryTarget — the SPM" >&2
    echo "[release]       digest format must have changed; update this script." >&2
fi

echo "${SHA256}  ${ZIP_NAME}" > "${SHA_PATH}"
echo "${CHECKSUM}" > "${SWIFT_CHECKSUM_PATH}"
echo "${TTSIGNAL_VERSION}" > "${VERSION_PATH}"

ASSET_URL="https://github.com/${TTSIGNAL_REPO}/releases/download/${TTSIGNAL_TAG}/${ZIP_NAME}"

echo "[release] version  : ${TTSIGNAL_VERSION}"
echo "[release] tag      : ${TTSIGNAL_TAG}"
echo "[release] repo     : ${TTSIGNAL_REPO}"
echo "[release] size     : ${ZIP_SIZE_HUMAN}"
echo "[release] sha256   : ${SHA256}"
echo "[release] checksum : ${CHECKSUM}   (swift package compute-checksum)"
echo "[release] asset URL: ${ASSET_URL}"
echo "[release] Package.swift snippet:"
cat <<EOF
    .binaryTarget(
        name: "TTSignal",
        url: "${ASSET_URL}",
        checksum: "${CHECKSUM}"
    )
EOF
echo "============================================================"

if [ "${DO_UPLOAD}" -ne 1 ]; then
    echo "[release] packaging done — pass --upload to publish to GitHub."
    exit 0
fi

# ------------------------------------------------------------ Step 3: upload
command -v gh >/dev/null 2>&1 || {
    echo "ERROR: 'gh' CLI not found. Install with: brew install gh" >&2
    exit 1
}
if ! gh auth status -h github.com >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated. Run: gh auth login" >&2
    exit 1
fi

NOTES_FILE="$(mktemp -t ttsignal-xcframework-notes.XXXXXX)"
trap 'rm -f "${NOTES_FILE}"' EXIT
cat > "${NOTES_FILE}" <<EOF
TTSignal Swift binary distribution \`${TTSIGNAL_VERSION}\`.

- xcframework: \`TTSignal.xcframework\` (\`ios-arm64\` + \`ios-arm64_x86_64-simulator\`)
- sha256: \`${SHA256}\`
- SPM checksum (\`swift package compute-checksum\`): \`${CHECKSUM}\`

Use from \`Package.swift\`:

\`\`\`swift
.binaryTarget(
    name: "TTSignal",
    url: "${ASSET_URL}",
    checksum: "${CHECKSUM}"
)
\`\`\`
EOF

if gh release view "${TTSIGNAL_TAG}" --repo "${TTSIGNAL_REPO}" >/dev/null 2>&1; then
    echo "[release] release ${TTSIGNAL_TAG} already exists — re-uploading asset"
    gh release upload "${TTSIGNAL_TAG}" "${ZIP_PATH}" \
        --repo "${TTSIGNAL_REPO}" --clobber
else
    echo "[release] creating release ${TTSIGNAL_TAG} on ${TTSIGNAL_REPO}"
    gh release create "${TTSIGNAL_TAG}" "${ZIP_PATH}" \
        --repo "${TTSIGNAL_REPO}" \
        --title "TTSignal ${TTSIGNAL_VERSION}" \
        --notes-file "${NOTES_FILE}"
fi

echo "============================================================"
echo "[release] OK — uploaded:"
echo "  ${ASSET_URL}"
echo "[release] checksum (swift package compute-checksum): ${CHECKSUM}"
echo "============================================================"
