#!/usr/bin/env bash
# ============================================================================
# ios/scripts/build-all.sh
#
# End-to-end iOS build pipeline. Runs the three stage scripts in order:
#
#   1. build-deps.sh        — boringssl + jquic + env (per slice)
#   2. build-core.sh        — ttsignal_ios + Apple bridge (per slice)
#   3. build-xcframework.sh — libtool merge + lipo + xcodebuild create-xcframework
#
# This is the safe one-button entry point: CMake skips up-to-date work, so
# a no-op re-run is only a few seconds of cmake-configure overhead. Use the
# individual stage scripts when you want fine-grained control or you're
# isolating a build issue.
#
# Output: build/ios-xcframework/TTSignal.xcframework
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

"${SCRIPT_DIR}/build-deps.sh"
"${SCRIPT_DIR}/build-core.sh"
"${SCRIPT_DIR}/build-xcframework.sh"
"${SCRIPT_DIR}/release-xcframework.sh" --upload

echo "============================================================"
echo "[ios] xcframework pipeline complete."
echo "[ios]   -> ${REPO_ROOT}/build/ios-xcframework/TTSignal.xcframework"
echo "============================================================"
