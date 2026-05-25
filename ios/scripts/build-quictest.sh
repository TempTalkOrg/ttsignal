#!/usr/bin/env bash
# ============================================================================
# ios/scripts/build-quictest.sh
#
# Produces a single, fully debuggable static archive for the QUICTest demo
# app. Independent from the public xcframework pipeline (build-core.sh +
# build-xcframework.sh) — this is the *internal* debugging build, NOT
# something to ship to consumers.
#
# Why a separate script:
#   - QUICTest only ever runs on the iOS Simulator (default: arm64), so
#     we don't need the device + x86_64 slices the xcframework carries.
#   - We want -O0 -g (full DWARF, no inlining, all locals visible in
#     lldb) without overwriting the release xcframework consumers depend
#     on. Release `build/ios-xcframework/TTSignal.xcframework` is left
#     completely untouched by this script.
#   - QUICTest links the resulting `.a` directly via LIBRARY_SEARCH_PATHS
#     + OTHER_LDFLAGS, bypassing xcframework altogether — Xcode treats
#     it as part of the project so breakpoints in C/C++/Objective-C
#     work the same as breakpoints in the Swift app code.
#
# Output:
#   build/quictest-debug/<sdk>/lib/libTTSignalDebug.a
#   build/quictest-debug/<sdk>/include/{ios_bridge.h,
#                                      AppleNetworkMonitor.h,
#                                      INetworkPathMonitor.h,
#                                      ios_logger.h,
#                                      module.modulemap}
#
# `<sdk>` is one of `iphonesimulator-arm64` (default), `iphonesimulator-x86_64`
# or `iphoneos-arm64`. Override via:
#
#   IOS_SLICE=device-arm64 ios/scripts/build-quictest.sh
#
# Run AFTER `ios/scripts/build-deps.sh` has produced the dependency
# archives in `build/ios-deps/<sdk>-<arch>/lib`. The same deps are
# reused unchanged — only ttsignal_ios itself is rebuilt with -g -O0.
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/src/cpp/cmake/ios.toolchain.cmake"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

# Slice selector. The QUICTest .xcodeproj search paths point at
# `build/quictest-debug/<sdk_label>` — keep the default in sync with
# the value baked into project.pbxproj (currently iphonesimulator-arm64).
IOS_SLICE="${IOS_SLICE:-sim-arm64}"
case "${IOS_SLICE}" in
    sim-arm64)
        IOS_PLATFORM=SIMULATOR_ARM64
        SDK=iphonesimulator
        ARCH=arm64
        ;;
    sim-x64)
        IOS_PLATFORM=SIMULATOR_X64
        SDK=iphonesimulator
        ARCH=x86_64
        ;;
    device-arm64)
        IOS_PLATFORM=OS64
        SDK=iphoneos
        ARCH=arm64
        ;;
    *)
        echo "ERROR: IOS_SLICE must be sim-arm64 / sim-x64 / device-arm64, got '${IOS_SLICE}'" >&2
        exit 1
        ;;
esac
SDK_LABEL="${SDK}-${ARCH}"

DEPS_ROOT="${REPO_ROOT}/build/ios-deps/${SDK_LABEL}"
if [ ! -d "${DEPS_ROOT}/lib" ]; then
    echo "ERROR: deps for ${SDK_LABEL} not found at ${DEPS_ROOT}." >&2
    echo "       Run ios/scripts/build-deps.sh first." >&2
    exit 1
fi

CMAKE_DIR="${REPO_ROOT}/build/quictest-debug/${SDK_LABEL}/cmake"
OUT_DIR="${REPO_ROOT}/build/quictest-debug/${SDK_LABEL}"
mkdir -p "${CMAKE_DIR}" "${OUT_DIR}/lib" "${OUT_DIR}/include"

echo "============================================================"
echo "[quictest] building ttsignal_ios for ${SDK_LABEL} (Debug, -O0 -g)"
echo "============================================================"

cmake -S "${REPO_ROOT}/src" -B "${CMAKE_DIR}" \
    -DBUILD_IOS_FRAMEWORK=ON \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DIOS_PLATFORM="${IOS_PLATFORM}" \
    -DIOS_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIOS_DEPS_DIR="${DEPS_ROOT}/lib"

cmake --build "${CMAKE_DIR}" --target ttsignal_ios -j "${JOBS}"

# ----------------------------------------------------------------------------
# Merge ttsignal_ios + deps into a single archive. Keeping it as one .a
# means QUICTest only needs `-lTTSignalDebug` in OTHER_LDFLAGS.
# ----------------------------------------------------------------------------
MERGED_A="${OUT_DIR}/lib/libTTSignalDebug.a"
echo "[quictest] merging into ${MERGED_A}"
libtool -static -o "${MERGED_A}" \
    "${CMAKE_DIR}/lib/libttsignal_ios.a" \
    "${DEPS_ROOT}/lib/libjquic.a" \
    "${DEPS_ROOT}/lib/libssl.a" \
    "${DEPS_ROOT}/lib/libcrypto.a" \
    "${DEPS_ROOT}/lib/libenv.a"

# ----------------------------------------------------------------------------
# Stage public headers + module.modulemap so QUICTest's HEADER_SEARCH_PATHS
# / SWIFT_INCLUDE_PATHS pointing at <out>/include find both `import
# TTSignalC` and the actual .h files in one place.
# ----------------------------------------------------------------------------
cp -f "${CMAKE_DIR}/stage/Headers/"*.h "${OUT_DIR}/include/"
cp -f "${CMAKE_DIR}/stage/Modules/module.modulemap" "${OUT_DIR}/include/module.modulemap"

echo "============================================================"
echo "[quictest] OK -> ${OUT_DIR}"
echo "[quictest]   library : ${MERGED_A}"
echo "[quictest]   headers : ${OUT_DIR}/include/"
echo "============================================================"
ls -lh "${MERGED_A}"
ls -1 "${OUT_DIR}/include/"
