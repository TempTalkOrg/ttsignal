#!/usr/bin/env bash
# ============================================================================
# ios/scripts/build-core.sh
#
# Compile ttsignal core (src/cpp/*.cpp) + Apple bridge (src/cpp/apple/*.mm)
# into a static library `libttsignal_ios.a` for each iOS slice. Run AFTER
# build-deps.sh has produced the static deps under build/ios-deps/.
#
# Output layout:
#   build/ios-<slice>-<release|debug>/lib/libttsignal_ios.a
#   build/ios-<slice>-<release|debug>/stage/Headers/{...}
#   build/ios-<slice>-<release|debug>/stage/Modules/module.modulemap
#
# Defaults to Release (`-O3 -DNDEBUG`, no DWARF). Pass `BUILD_TYPE=Debug` to
# produce a `-O0 -g` build that includes DWARF inside each .o (and therefore
# inside the merged static archive built by build-xcframework.sh). The
# Debug archive is what you want when stepping through C/C++/Objective-C
# in Xcode against QUICTest — see ios/QUICTest for the workflow.
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/src/cpp/cmake/ios.toolchain.cmake"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

# Build type — "Release" or "Debug". Debug gives `-O0 -g` so QUICTest can
# set breakpoints and inspect locals inside ttsignal_ios.a; Release keeps
# the original `-O3 -DNDEBUG` flags shipped to consumers.
BUILD_TYPE="${BUILD_TYPE:-Release}"
case "${BUILD_TYPE}" in
    Release|Debug) ;;
    *)
        echo "ERROR: BUILD_TYPE must be Release or Debug, got '${BUILD_TYPE}'" >&2
        exit 1
        ;;
esac
BUILD_SUFFIX="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"

SLICES=(
    # slice_id            | sdk             | arch    | IOS_PLATFORM
    # The full build dir label is "ios-<slice_id>-<release|debug>".
    "device-arm64:iphoneos:arm64:OS64"
    "sim-arm64:iphonesimulator:arm64:SIMULATOR_ARM64"
    "sim-x64:iphonesimulator:x86_64:SIMULATOR_X64"
)

build_one_slice() {
    local slice_id="$1" sdk="$2" arch="$3" platform="$4"
    local sdk_label="${sdk}-${arch}"
    local deps_root="${REPO_ROOT}/build/ios-deps/${sdk_label}"

    if [ ! -d "${deps_root}/lib" ]; then
        echo "ERROR: deps for ${sdk_label} not found at ${deps_root}." >&2
        echo "       Run ios/scripts/build-deps.sh first." >&2
        exit 1
    fi

    local label="ios-${slice_id}-${BUILD_SUFFIX}"
    local build_dir="${REPO_ROOT}/build/${label}"
    mkdir -p "${build_dir}"

    echo "============================================================"
    echo "[ios-core] building ${label} (CMAKE_BUILD_TYPE=${BUILD_TYPE})"
    echo "============================================================"

    # Configure once, then iterative incremental build.
    cmake -S "${REPO_ROOT}/src" -B "${build_dir}" \
        -DBUILD_IOS_FRAMEWORK=ON \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DIOS_PLATFORM="${platform}" \
        -DIOS_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DIOS_DEPS_DIR="${deps_root}/lib"

    cmake --build "${build_dir}" --target ttsignal_ios -j "${JOBS}"

    # Collect the .a + headers under a slice-local stage dir for the
    # xcframework helper.
    local stage="${build_dir}/dist"
    mkdir -p "${stage}/lib" "${stage}/Headers" "${stage}/Modules"
    cp -f "${build_dir}/lib/libttsignal_ios.a" "${stage}/lib/"
    cp -f "${build_dir}/stage/Headers/"*.h     "${stage}/Headers/"
    cp -f "${build_dir}/stage/Modules/module.modulemap" "${stage}/Modules/"

    echo "[ios-core] ${label} -> ${stage}"
}

for slice in "${SLICES[@]}"; do
    IFS=':' read -r slice_id sdk arch platform <<<"${slice}"
    build_one_slice "${slice_id}" "${sdk}" "${arch}" "${platform}"
done

echo "[ios-core] all slices done (${BUILD_TYPE})."
