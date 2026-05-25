#!/usr/bin/env bash
# ============================================================================
# ios/scripts/publish-to-release-repo.sh
#
# Mirror the upstream Swift wrapper + render the SPM/CocoaPods manifests
# into the ttsignal-xcframework distribution repo, commit + tag, and (with
# --push) publish to GitHub.
#
# Pre-requisite: ios/scripts/release-xcframework.sh --upload has already
# been run for THIS version, so build/ios-xcframework/dist/ttsignal-swift.zip
# (and its .swift-checksum / .sha256 sidecars) exist locally and the
# matching asset is live at the release URL. We re-verify both so a stale
# zip can never get coupled to a fresh tag.
#
# Steps:
#   1. Derive version: read the .version sidecar that release-xcframework.sh
#      wrote (this is the post-auto-bump value, e.g. `1.0.YYYYMMDD-2`),
#      falling back to the Utils.cpp-mtime-derived `1.0.YYYYMMDD` if the
#      sidecar is absent (e.g. the artifact predates the auto-bump
#      pipeline). --version / TTSIGNAL_VERSION still wins over both.
#      Then load checksum + sha256 from sidecars; sanity check against
#      the GitHub release asset.
#   2. Clone (or fast-forward) ttsignal-xcframework into build/release-repo-checkout/.
#   3. rsync src/swift/ -> Sources/TTSignal/ (purges files removed upstream).
#   4. Render Package.swift, TTSignal.podspec, README.md from
#      ios/templates/ with the version / url / checksum / sha256
#      placeholders substituted in.
#   5. Copy over LICENSE + .gitignore on first bootstrap.
#   6. git add -A; commit "release <version>"; git tag <version>; (optional) push.
#
# Usage:
#   ios/scripts/publish-to-release-repo.sh                     # dry-run (commit + tag only, no push)
#   ios/scripts/publish-to-release-repo.sh --push              # publish to remote
#   ios/scripts/publish-to-release-repo.sh --force-tag --push  # overwrite existing tag (rare)
#
# Env / flag overrides:
#   --version <ver>           override SDK version (default: from Utils.cpp mtime)
#   --release-repo <slug>     where to publish (default: 3th1UOYgUtJkurSZ/ttsignal-xcframework)
#   --binary-repo  <slug>     where ttsignal-swift.zip is hosted as a release asset
#                             (default: same as --release-repo)
#
# Requires: macOS or Linux with bash, git, gh, rsync, sed, shasum.
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEMPLATE_DIR="${REPO_ROOT}/ios/templates"
SWIFT_SRC_DIR="${REPO_ROOT}/src/swift"
DIST_DIR="${REPO_ROOT}/build/ios-xcframework/dist"
ZIP_PATH="${DIST_DIR}/ttsignal-swift.zip"
SWIFT_CHECKSUM_PATH="${ZIP_PATH}.swift-checksum"
SHA256_PATH="${ZIP_PATH}.sha256"
VERSION_PATH="${ZIP_PATH}.version"
CHECKOUT_DIR="${REPO_ROOT}/build/release-repo-checkout"

show_help() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
}

# ------------------------------------------------------------ args / defaults
DO_PUSH=0
FORCE_TAG=0
TTSIGNAL_VERSION="${TTSIGNAL_VERSION:-}"
RELEASE_REPO="${TTSIGNAL_RELEASE_REPO:-3th1UOYgUtJkurSZ/ttsignal-xcframework}"
BINARY_REPO=""

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--push)         DO_PUSH=1; shift ;;
        --force-tag)       FORCE_TAG=1; shift ;;
        --version)         TTSIGNAL_VERSION="$2"; shift 2 ;;
        --release-repo)    RELEASE_REPO="$2"; shift 2 ;;
        --binary-repo)     BINARY_REPO="$2"; shift 2 ;;
        -h|--help)         show_help; exit 0 ;;
        *) echo "unknown argument: $1" >&2; show_help; exit 1 ;;
    esac
done

[ -z "${BINARY_REPO}" ] && BINARY_REPO="${RELEASE_REPO}"

if [ -z "${TTSIGNAL_VERSION}" ]; then
    # Prefer the sidecar release-xcframework.sh wrote — that string
    # already accounts for the same-day -N auto-bump. Fall back to the
    # mtime-derived base version only when the sidecar is missing
    # (e.g. an artifact built before the bump pipeline existed).
    if [ -f "${VERSION_PATH}" ]; then
        TTSIGNAL_VERSION=$(tr -d '[:space:]' < "${VERSION_PATH}")
    fi
fi
if [ -z "${TTSIGNAL_VERSION}" ]; then
    UTILS_CPP="${REPO_ROOT}/src/cpp/Utils.cpp"
    if [ ! -f "${UTILS_CPP}" ]; then
        echo "ERROR: cannot derive SDK version: ${UTILS_CPP} not found and" >&2
        echo "       ${VERSION_PATH} sidecar absent. Run release-xcframework.sh first." >&2
        exit 1
    fi
    if d=$(stat -f '%Sm' -t '%Y%m%d' "${UTILS_CPP}" 2>/dev/null); then
        TTSIGNAL_VERSION="1.0.${d}"
    else
        d=$(stat -c '%y' "${UTILS_CPP}" | awk '{gsub("-",""); print substr($1,1,8)}')
        TTSIGNAL_VERSION="1.0.${d}"
    fi
    echo "[publish] WARN: ${VERSION_PATH} not found; falling back to" >&2
    echo "[publish]       mtime-derived ${TTSIGNAL_VERSION}. If release-xcframework.sh" >&2
    echo "[publish]       did a same-day -N bump, this will mismatch — re-run it." >&2
fi
TAG="${TTSIGNAL_VERSION}"
ZIP_URL="https://github.com/${BINARY_REPO}/releases/download/${TAG}/ttsignal-swift.zip"

# ------------------------------------------------------------ tool checks
for tool in git gh rsync sed shasum; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "ERROR: required tool '${tool}' not on PATH." >&2
        exit 1
    }
done
if ! gh auth status -h github.com >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated. Run: gh auth login -h github.com" >&2
    exit 1
fi

# ------------------------------------------------------------ verify local artifact
if [ ! -f "${ZIP_PATH}" ] || [ ! -f "${SWIFT_CHECKSUM_PATH}" ] || [ ! -f "${SHA256_PATH}" ]; then
    echo "ERROR: local xcframework artifact missing under ${DIST_DIR}." >&2
    echo "       Run: ios/scripts/release-xcframework.sh --upload" >&2
    exit 1
fi
SPM_CHECKSUM=$(cat "${SWIFT_CHECKSUM_PATH}" | tr -d '[:space:]')
SHA256=$(awk '{print $1}' "${SHA256_PATH}")

LOCAL_SHA256=$(shasum -a 256 "${ZIP_PATH}" | awk '{print $1}')
if [ "${LOCAL_SHA256}" != "${SHA256}" ]; then
    echo "ERROR: local zip's SHA-256 (${LOCAL_SHA256}) does not match" >&2
    echo "       the cached sidecar (${SHA256}). Re-run release-xcframework.sh." >&2
    exit 1
fi

# ------------------------------------------------------------ verify remote asset matches
echo "[publish] verifying GitHub release asset checksum..."
if ! gh release view "${TAG}" --repo "${BINARY_REPO}" >/dev/null 2>&1; then
    echo "ERROR: release ${TAG} not found on ${BINARY_REPO}." >&2
    echo "       Run: ios/scripts/release-xcframework.sh --upload" >&2
    exit 1
fi
REMOTE_SHA=$(gh release view "${TAG}" --repo "${BINARY_REPO}" \
    --json assets --jq '.assets[] | select(.name=="ttsignal-swift.zip") | .digest' \
    | sed 's/^sha256://')
if [ -z "${REMOTE_SHA}" ]; then
    echo "ERROR: ttsignal-swift.zip not found on release ${TAG}@${BINARY_REPO}." >&2
    exit 1
fi
if [ "${REMOTE_SHA}" != "${SHA256}" ]; then
    echo "ERROR: remote release asset SHA-256 (${REMOTE_SHA})" >&2
    echo "       does not match local zip (${SHA256})." >&2
    echo "       Someone re-uploaded a different zip under the same tag." >&2
    exit 1
fi

# ------------------------------------------------------------ summary
echo "============================================================"
echo "[publish] version       : ${TTSIGNAL_VERSION}"
echo "[publish] release repo  : ${RELEASE_REPO}"
echo "[publish] binary repo   : ${BINARY_REPO}"
echo "[publish] zip url       : ${ZIP_URL}"
echo "[publish] SPM checksum  : ${SPM_CHECKSUM}"
echo "[publish] sha256        : ${SHA256}"
echo "[publish] push          : $([ "${DO_PUSH}" -eq 1 ] && echo yes || echo no)"
echo "============================================================"

# ------------------------------------------------------------ clone or update checkout
mkdir -p "$(dirname "${CHECKOUT_DIR}")"
if [ -d "${CHECKOUT_DIR}/.git" ]; then
    EXISTING_REMOTE=$(git -C "${CHECKOUT_DIR}" remote get-url origin 2>/dev/null || true)
    EXPECTED_REMOTE_HTTPS="https://github.com/${RELEASE_REPO}.git"
    EXPECTED_REMOTE_SSH="git@github.com:${RELEASE_REPO}.git"
    if [ "${EXISTING_REMOTE}" != "${EXPECTED_REMOTE_HTTPS}" ] && \
       [ "${EXISTING_REMOTE}" != "${EXPECTED_REMOTE_SSH}" ]; then
        echo "[publish] checkout points at ${EXISTING_REMOTE}, re-cloning"
        rm -rf "${CHECKOUT_DIR}"
    fi
fi
if [ ! -d "${CHECKOUT_DIR}/.git" ]; then
    echo "[publish] cloning ${RELEASE_REPO} -> ${CHECKOUT_DIR}"
    gh repo clone "${RELEASE_REPO}" "${CHECKOUT_DIR}" -- --quiet
else
    echo "[publish] updating existing checkout"
    # Use `+refs/...` refspecs so fetch force-updates branch refs and local
    # tags (including ones we created in a previous dry-run); without the
    # leading '+', a stale local tag pointing at a different commit than the
    # remote causes `git fetch --tags` to reject with "[rejected]" and exits
    # nonzero, killing the script under `set -e`. `--prune-tags` then drops
    # local tags that no longer exist on the remote, keeping the workspace
    # in lockstep with the published release set.
    git -C "${CHECKOUT_DIR}" fetch --prune --prune-tags --quiet origin \
        '+refs/heads/*:refs/remotes/origin/*' \
        '+refs/tags/*:refs/tags/*'
    DEFAULT_BRANCH=$(git -C "${CHECKOUT_DIR}" symbolic-ref --short HEAD 2>/dev/null \
        || git -C "${CHECKOUT_DIR}" rev-parse --abbrev-ref origin/HEAD | sed 's@^origin/@@')
    git -C "${CHECKOUT_DIR}" checkout --quiet "${DEFAULT_BRANCH}"
    git -C "${CHECKOUT_DIR}" reset --hard "origin/${DEFAULT_BRANCH}" --quiet
fi

# ------------------------------------------------------------ tag pre-check
# A common case: ios/scripts/release-xcframework.sh just ran `gh release
# create ${TAG}`, which auto-creates the tag pointing at origin/main's
# current HEAD (the previous publish commit, or the bootstrap README
# commit if this is the first publish). Our job is to stamp a NEW commit
# carrying Package.swift + Sources/ for this version and re-anchor the
# tag onto it. That is not a destructive action — the GitHub release
# entry is keyed on tag NAME, not commit, and we already verified above
# that the asset SHA matches our local zip — so silently auto-allow the
# tag move whenever the SHAs agree. Only require explicit --force-tag
# when retargeting a tag whose release asset doesn't match local (which
# would indicate a real content conflict, not a workflow chicken-and-egg).
if git -C "${CHECKOUT_DIR}" rev-parse "refs/tags/${TAG}" >/dev/null 2>&1; then
    if [ "${FORCE_TAG}" -eq 1 ]; then
        echo "[publish] WARN: --force-tag set, will overwrite existing tag ${TAG}"
    else
        echo "[publish] tag ${TAG} already exists; release asset SHA matches local"
        echo "[publish]   -> auto-allowing tag move (use --force-tag to silence this note)"
        FORCE_TAG=1
    fi
fi

# ------------------------------------------------------------ sync swift sources
SOURCES_DIR="${CHECKOUT_DIR}/Sources/TTSignal"
mkdir -p "${SOURCES_DIR}"
echo "[publish] syncing src/swift/ -> Sources/TTSignal/"
rsync -a --delete --include='*.swift' --exclude='*' \
    "${SWIFT_SRC_DIR}/" "${SOURCES_DIR}/"

# ------------------------------------------------------------ render templates
render() {
    local in="$1" out="$2"
    sed \
        -e "s|__TTSIGNAL_VERSION__|${TTSIGNAL_VERSION}|g" \
        -e "s|__TTSIGNAL_ZIP_URL__|${ZIP_URL}|g" \
        -e "s|__TTSIGNAL_CHECKSUM__|${SPM_CHECKSUM}|g" \
        -e "s|__TTSIGNAL_SHA256__|${SHA256}|g" \
        "${in}" > "${out}"
}
echo "[publish] rendering Package.swift / TTSignal.podspec / README.md"
render "${TEMPLATE_DIR}/Package.swift.in"     "${CHECKOUT_DIR}/Package.swift"
render "${TEMPLATE_DIR}/TTSignal.podspec.in"  "${CHECKOUT_DIR}/TTSignal.podspec"
render "${TEMPLATE_DIR}/README.md.in"         "${CHECKOUT_DIR}/README.md"

# ------------------------------------------------------------ first-bootstrap files
[ -f "${CHECKOUT_DIR}/LICENSE" ]    || cp "${TEMPLATE_DIR}/LICENSE"    "${CHECKOUT_DIR}/LICENSE"
[ -f "${CHECKOUT_DIR}/.gitignore" ] || cp "${TEMPLATE_DIR}/gitignore"  "${CHECKOUT_DIR}/.gitignore"

# ------------------------------------------------------------ commit + tag
cd "${CHECKOUT_DIR}"
git add -A
if git diff --cached --quiet; then
    echo "[publish] nothing to commit — release content unchanged."
else
    git commit -m "release ${TTSIGNAL_VERSION}

Auto-generated by ios/scripts/publish-to-release-repo.sh.

- xcframework: ${ZIP_URL}
- SPM checksum: ${SPM_CHECKSUM}
- SHA-256:      ${SHA256}
" >/dev/null
    echo "[publish] commit created: $(git log -1 --oneline)"
fi

# (re)create the tag
if [ "${FORCE_TAG}" -eq 1 ]; then
    git tag -f "${TAG}" >/dev/null
else
    git tag "${TAG}" >/dev/null 2>&1 || true
fi
echo "[publish] tag: ${TAG} -> $(git rev-parse --short "${TAG}")"

# ------------------------------------------------------------ push
if [ "${DO_PUSH}" -ne 1 ]; then
    echo "============================================================"
    echo "[publish] dry run complete (no push). Inspect ${CHECKOUT_DIR}"
    echo "[publish] then re-run with --push to publish to GitHub."
    echo "============================================================"
    exit 0
fi

DEFAULT_BRANCH=$(git symbolic-ref --short HEAD)
echo "[publish] pushing ${DEFAULT_BRANCH} + tag to origin"
git push origin "${DEFAULT_BRANCH}" --quiet
if [ "${FORCE_TAG}" -eq 1 ]; then
    git push origin "refs/tags/${TAG}" --force --quiet
else
    git push origin "refs/tags/${TAG}" --quiet
fi

echo "============================================================"
echo "[publish] OK — ${RELEASE_REPO} updated"
echo "[publish]   tag : https://github.com/${RELEASE_REPO}/releases/tag/${TAG}"
echo "[publish]   tree: https://github.com/${RELEASE_REPO}/tree/${TAG}"
echo "============================================================"
echo "SwiftPM:  .package(url: \"https://github.com/${RELEASE_REPO}.git\", from: \"${TTSIGNAL_VERSION}\")"
echo "CocoaPods: pod 'TTSignal', :git => 'https://github.com/${RELEASE_REPO}.git', :tag => '${TTSIGNAL_VERSION}'"
echo "============================================================"
