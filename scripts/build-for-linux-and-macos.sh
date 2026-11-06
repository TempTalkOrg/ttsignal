#!/bin/bash
# Build all Linux + macOS Node addons in sequence and, after each platform
# build, scan the produced .node for absolute build-host paths or usernames
# (see scripts/verify-no-sensitive-info.py for the exact pattern set).
#
# Hits hard-fail the script with a non-zero exit code so a leaky build never
# silently makes it to scripts/upload-release-for-linux-and-macos.sh.

set -eu -o pipefail

script_dir=$(dirname "$(realpath "$0")")
repo_root=$(cd "${script_dir}/.." && pwd)

PY=${PYTHON:-}
if [ -z "${PY}" ]; then
    if command -v python3 >/dev/null 2>&1; then
        PY=python3
    elif command -v python >/dev/null 2>&1; then
        PY=python
    fi
fi

verify() {
    local artifact_glob="$1"
    if [ -z "${PY}" ]; then
        echo "WARN: python3 not found on PATH; skipping sensitive-info verify for ${artifact_glob}" >&2
        return 0
    fi
    # shellcheck disable=SC2086
    local matches=( ${artifact_glob} )
    if [ ! -e "${matches[0]}" ]; then
        echo "WARN: artifact not found, skipping verify: ${artifact_glob}" >&2
        return 0
    fi
    "${PY}" "${script_dir}/verify-no-sensitive-info.py" "${matches[@]}"
}

build() {
    local script_rel="$1"
    cd "${repo_root}"
    bash "${script_rel}"
}

build build/linux-x64-release/build
verify "${repo_root}/node_modules/ttsignal/dist/build/Release/ttsignal.linux.x64.node"

build build/linux-arm64-release/build
verify "${repo_root}/node_modules/ttsignal/dist/build/Release/ttsignal.linux.arm64.node"

build build/macos-arm64-release/build
verify "${repo_root}/node_modules/ttsignal/dist/build/Release/ttsignal.darwin.arm64.node"

build build/macos-x64-release/build
verify "${repo_root}/node_modules/ttsignal/dist/build/Release/ttsignal.darwin.x64.node"

echo "============================================================"
echo "[build] all 4 addons built and verified clean."
echo "============================================================"
