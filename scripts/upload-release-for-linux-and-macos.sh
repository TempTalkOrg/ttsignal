#!/usr/bin/env bash
# ============================================================================
# scripts/upload-release-for-linux-and-macos.sh
#
# Publish the Node.js native addons for Linux + macOS (and the JS loader)
# to the same GitHub Release that hosts the iOS xcframework.
#
# Uploaded assets (every release version):
#   - node_modules/ttsignal/Release/ttsignal.darwin.arm64.node
#   - node_modules/ttsignal/Release/ttsignal.darwin.x64.node
#   - node_modules/ttsignal/Release/ttsignal.linux.arm64.node
#   - node_modules/ttsignal/Release/ttsignal.linux.x64.node
#   - src/js/index.js
#
# Companion script for Windows: scripts/upload-release-for-win.bat (uploads
# ttsignal.win32.x64.node + index.js to the same tag with --clobber).
#
# Versioning policy: identical to ios/scripts/release-xcframework.sh.
# The tag is `1.0.YYYYMMDD` where YYYYMMDD == src/cpp/Utils.cpp's mtime,
# which is what the C++ SDK bakes in via __DATE__ at compile time. Using
# the same source for the tag keeps the iOS xcframework and the Node
# binaries on a single release entry per build day. The patch component is
# the date so the string remains valid 3-segment SemVer (SwiftPM and
# CocoaPods both reject `1.0.0.YYYYMMDD` — too many segments).
#
# Usage:
#   scripts/upload-release-for-linux-and-macos.sh
#   scripts/upload-release-for-linux-and-macos.sh --version 1.0.20260429
#   scripts/upload-release-for-linux-and-macos.sh --repo owner/other-release
#
# Env overrides (alternative to flags):
#   TTSIGNAL_VERSION  e.g. 1.0.20260429    (default: 1.0.<Utils.cpp mtime YYYYMMDD>)
#   TTSIGNAL_REPO     e.g. owner/ttsignal-xcframework  (default: 3th1UOYgUtJkurSZ/ttsignal-xcframework)
#   TTSIGNAL_TAG      git tag name          (default: same as $TTSIGNAL_VERSION)
#
# Requires: `gh` CLI (authenticated against the target repo) and `stat`.
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

show_help() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

# ------------------------------------------------------------ derive default version
# `Utils.cpp`'s mtime is the same source iOS uses, keeping all platforms on
# one release entry. `stat -f` is BSD/macOS; `stat -c` is GNU/Linux — try
# both so this script works on either build host.
UTILS_CPP="${REPO_ROOT}/src/cpp/Utils.cpp"
utils_mdate() {
    if [ ! -f "${UTILS_CPP}" ]; then
        return 1
    fi
    if stat -f '%Sm' -t '%Y%m%d' "${UTILS_CPP}" 2>/dev/null; then
        return 0
    fi
    if stat -c '%y' "${UTILS_CPP}" 2>/dev/null | awk '{gsub("-",""); print substr($1,1,8)}'; then
        return 0
    fi
    return 1
}
UTILS_MDATE="$(utils_mdate || true)"

# ------------------------------------------------------------ args + defaults
TTSIGNAL_VERSION="${TTSIGNAL_VERSION:-1.0.${UTILS_MDATE}}"
TTSIGNAL_REPO="${TTSIGNAL_REPO:-3th1UOYgUtJkurSZ/ttsignal-xcframework}"
TTSIGNAL_TAG="${TTSIGNAL_TAG:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --version) TTSIGNAL_VERSION="$2"; shift 2 ;;
        --repo)    TTSIGNAL_REPO="$2"; shift 2 ;;
        --tag)     TTSIGNAL_TAG="$2"; shift 2 ;;
        -h|--help) show_help; exit 0 ;;
        *) echo "unknown argument: $1" >&2; show_help; exit 1 ;;
    esac
done

if [ -z "${UTILS_MDATE}" ] && [ "${TTSIGNAL_VERSION}" = "1.0." ]; then
    echo "ERROR: cannot derive SDK version: ${UTILS_CPP} not found, and" >&2
    echo "       no --version / TTSIGNAL_VERSION was supplied." >&2
    exit 1
fi
[ -z "${TTSIGNAL_TAG}" ] && TTSIGNAL_TAG="${TTSIGNAL_VERSION}"

# ------------------------------------------------------------ asset paths
ADDON_DIR="${REPO_ROOT}/node_modules/ttsignal/Release"
INDEX_JS="${REPO_ROOT}/src/js/index.js"

ASSETS=(
    "${ADDON_DIR}/ttsignal.darwin.arm64.node"
    "${ADDON_DIR}/ttsignal.darwin.x64.node"
    "${ADDON_DIR}/ttsignal.linux.arm64.node"
    "${ADDON_DIR}/ttsignal.linux.x64.node"
    "${INDEX_JS}"
)

# ------------------------------------------------------------ sanity checks
command -v gh >/dev/null 2>&1 || {
    echo "ERROR: 'gh' CLI not found. Install with: brew install gh" >&2
    exit 1
}
if ! gh auth status -h github.com >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated. Run: gh auth login -h github.com" >&2
    exit 1
fi

missing=0
for f in "${ASSETS[@]}"; do
    if [ ! -f "${f}" ]; then
        echo "ERROR: missing asset: ${f}" >&2
        missing=1
    fi
done
if [ "${missing}" -ne 0 ]; then
    echo "Hint: build all four addons first (linux x64/arm64 + macos x64/arm64)" >&2
    echo "      e.g. scripts/build-for-linux-and-macos.sh" >&2
    exit 1
fi

# ------------------------------------------------------------ summary
echo "============================================================"
echo "[release] version : ${TTSIGNAL_VERSION}"
echo "[release] tag     : ${TTSIGNAL_TAG}"
echo "[release] repo    : ${TTSIGNAL_REPO}"
echo "[release] assets  :"
for f in "${ASSETS[@]}"; do
    size=$(du -h "${f}" | awk '{print $1}')
    printf "  %-7s  %s\n" "${size}" "${f#${REPO_ROOT}/}"
done
echo "============================================================"

# ------------------------------------------------------------ create / upload
# If the release already exists (e.g. iOS xcframework was uploaded first
# under the same tag, or this script ran for Windows already), append the
# linux/macos addons to it. Otherwise create the release with these
# assets attached.
NOTES_FILE="$(mktemp -t ttsignal-xcframework-notes.XXXXXX)"
trap 'rm -f "${NOTES_FILE}"' EXIT
cat > "${NOTES_FILE}" <<EOF
TTSignal release \`${TTSIGNAL_VERSION}\`.

Assets uploaded by \`scripts/upload-release-for-linux-and-macos.sh\`:

- \`ttsignal.darwin.arm64.node\` — macOS Apple Silicon Node addon
- \`ttsignal.darwin.x64.node\`   — macOS Intel Node addon
- \`ttsignal.linux.arm64.node\`  — Linux aarch64 Node addon
- \`ttsignal.linux.x64.node\`    — Linux x86_64 Node addon
- \`index.js\`                   — Node.js loader (selects the addon by \`process.platform\` + \`process.arch\`)

Companion uploads (separate scripts):
- \`scripts/upload-release-for-win.bat\` — Windows \`ttsignal.win32.x64.node\`
- \`ios/scripts/release-xcframework.sh\` — \`ttsignal-swift.zip\` (SPM binaryTarget)
EOF

if gh release view "${TTSIGNAL_TAG}" --repo "${TTSIGNAL_REPO}" >/dev/null 2>&1; then
    echo "[release] tag ${TTSIGNAL_TAG} already exists — uploading with --clobber"
    gh release upload "${TTSIGNAL_TAG}" "${ASSETS[@]}" \
        --repo "${TTSIGNAL_REPO}" --clobber
else
    echo "[release] creating release ${TTSIGNAL_TAG} on ${TTSIGNAL_REPO}"
    gh release create "${TTSIGNAL_TAG}" "${ASSETS[@]}" \
        --repo "${TTSIGNAL_REPO}" \
        --title "TTSignal ${TTSIGNAL_VERSION}" \
        --notes-file "${NOTES_FILE}"
fi

echo "============================================================"
echo "[release] OK — released to https://github.com/${TTSIGNAL_REPO}/releases/tag/${TTSIGNAL_TAG}"
echo "============================================================"
