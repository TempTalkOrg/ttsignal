#!/usr/bin/env bash
# ============================================================================
# ios/scripts/build-deps.sh
#
# Compile boringssl + jquic + env as static libraries for the three iOS
# slices we ship in the xcframework:
#   - iphoneos / arm64           (real device)
#   - iphonesimulator / arm64    (Apple-silicon Mac simulator)
#   - iphonesimulator / x86_64   (Intel Mac simulator — keep for CI)
#
# Output layout (final, copied for stable consumption):
#   build/ios-deps/<sdk>-<arch>/lib/libssl.a
#   build/ios-deps/<sdk>-<arch>/lib/libcrypto.a
#   build/ios-deps/<sdk>-<arch>/lib/libjquic.a
#   build/ios-deps/<sdk>-<arch>/lib/libenv.a
#
# Each underlying CMake project also writes to
#   deps/<lib>/lib/iOS/<arch>/Release/lib*.a
# (their own LIBRARY_OUTPUT_PATH convention) — we copy from there into the
# stable build/ios-deps/ tree so build-core.sh has a single place to look.
#
# Requirements: Xcode 14+, CMake 3.20+. boringssl additionally needs perl
# and `go` on the host PATH (host tools, not iOS). If the network can't
# reach go.googlesource.com, this script tries to reuse a previously
# generated err_data.c from any existing build/* directory so boringssl
# can be built fully offline once any other slice has succeeded.
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/src/cpp/cmake/ios.toolchain.cmake"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

# CMAKE_SYSTEM_PROCESSOR values used by the toolchain — must match the
# `arch` part of the slice tuple below (drive the LIBRARY_OUTPUT_PATH dirs
# under deps/*/lib/iOS/<arch>/Release).
SLICES=(
    # sdk:arch:IOS_PLATFORM
    "iphoneos:arm64:OS64"
    "iphonesimulator:arm64:SIMULATOR_ARM64"
    "iphonesimulator:x86_64:SIMULATOR_X64"
)

OUT_ROOT="${REPO_ROOT}/build/ios-deps"
mkdir -p "${OUT_ROOT}"

# --------------------------------------------------------------------------
# Find any pre-generated err_data.c (boringssl's golang-generated catalog of
# OpenSSL-compatible error codes) so we can build offline if the network
# can't reach go.googlesource.com / goproxy.cn. err_data.c is platform-
# independent so any prior arch's copy works.
# --------------------------------------------------------------------------
find_pregenerated_err_data() {
    local f
    f=$(find "${REPO_ROOT}/build" -name 'err_data.c' \
            -path '*/boringssl*' 2>/dev/null \
            | head -n1 || true)
    if [ -n "${f}" ] && [ -s "${f}" ]; then
        echo "${f}"
    fi
}

# --------------------------------------------------------------------------
# CRITICAL: device (iphoneos) and simulator-arm64 (iphonesimulator) both
# have CMAKE_SYSTEM_PROCESSOR=arm64, and every dep's CMakeLists pins its
# LIBRARY_OUTPUT_PATH to .../iOS/${CMAKE_SYSTEM_PROCESSOR}/Release/. This
# means iphoneos:arm64 and iphonesimulator:arm64 both write to
# deps/<lib>/lib/iOS/arm64/Release/ — a shared bucket where whoever links
# last wins. On a re-run, cmake's incremental logic may decide the .o files
# are unchanged and skip the link step entirely, leaving the .a from the
# previous slice in place; the cp below then happily ships a simulator
# archive into iphoneos-arm64/lib/ (or vice versa) and `xcodebuild
# -create-xcframework` later dies with "binaries with multiple platforms".
#
# Workaround: nuke the shared .a files before each `cmake --build` so the
# link step is forced to re-run. The per-slice .o files (in this slice's
# private cmake build dir) are unchanged, so this is just a relink — no
# recompile cost.
# --------------------------------------------------------------------------
force_relink() {
    rm -f "$@"
}

build_one_slice() {
    local sdk="$1" arch="$2" platform="$3"
    local label="${sdk}-${arch}"
    local stage="${OUT_ROOT}/${label}"
    local lib_dir="${stage}/lib"
    mkdir -p "${lib_dir}"

    echo "============================================================"
    echo "[ios-deps] building ${label}"
    echo "============================================================"

    # ----- boringssl ------------------------------------------------
    local bssl_build="${stage}/boringssl-build"
    mkdir -p "${bssl_build}"
    cmake -S "${REPO_ROOT}/deps/boringssl/src" -B "${bssl_build}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DIOS_PLATFORM="${platform}" \
        -DIOS_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE=Release

    # Pre-seed err_data.c if available so the Go fetch step doesn't run.
    # Touch the timestamp far in the future so make believes the file is
    # newer than its generator script (the gmake "in the future" warning
    # is harmless).
    local prebaked
    prebaked=$(find_pregenerated_err_data || true)
    if [ -n "${prebaked}" ]; then
        echo "[ios-deps] reusing err_data.c from ${prebaked}"
        mkdir -p "${bssl_build}/crypto"
        cp -f "${prebaked}" "${bssl_build}/crypto/err_data.c"
        touch -t 203012010000 "${bssl_build}/crypto/err_data.c"
    fi
    force_relink "${REPO_ROOT}/deps/boringssl/lib/iOS/${arch}/Release/libssl.a" \
                 "${REPO_ROOT}/deps/boringssl/lib/iOS/${arch}/Release/libcrypto.a"
    cmake --build "${bssl_build}" --target ssl crypto -j "${JOBS}"

    # boringssl writes to deps/boringssl/lib/iOS/<arch>/Release/. Copy back
    # to the stable per-slice tree.
    cp -f "${REPO_ROOT}/deps/boringssl/lib/iOS/${arch}/Release/libssl.a"    "${lib_dir}/libssl.a"
    cp -f "${REPO_ROOT}/deps/boringssl/lib/iOS/${arch}/Release/libcrypto.a" "${lib_dir}/libcrypto.a"

    # ----- env -----------------------------------------------------
    local env_build="${stage}/env-build"
    mkdir -p "${env_build}"
    cmake -S "${REPO_ROOT}/deps/env/src" -B "${env_build}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DIOS_PLATFORM="${platform}" \
        -DIOS_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE=Release
    force_relink "${REPO_ROOT}/deps/env/lib/iOS/${arch}/Release/libenv.a"
    cmake --build "${env_build}" --target env -j "${JOBS}"
    cp -f "${REPO_ROOT}/deps/env/lib/iOS/${arch}/Release/libenv.a" "${lib_dir}/libenv.a"

    # ----- jquic ---------------------------------------------------
    local jquic_build="${stage}/jquic-build"
    mkdir -p "${jquic_build}"
    cmake -S "${REPO_ROOT}/deps/jquic" -B "${jquic_build}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DIOS_PLATFORM="${platform}" \
        -DIOS_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DSSL_PATH="${REPO_ROOT}/deps/boringssl" \
        -DSSL_LIB_PATH="${lib_dir}" \
        -DSSL_INC_PATH="${REPO_ROOT}/deps/boringssl/src/include" \
        -DCMAKE_BUILD_TYPE=Release
    force_relink "${REPO_ROOT}/deps/jquic/lib/iOS/${arch}/Release/libjquic.a"
    cmake --build "${jquic_build}" --target jquic -j "${JOBS}"
    cp -f "${REPO_ROOT}/deps/jquic/lib/iOS/${arch}/Release/libjquic.a" "${lib_dir}/libjquic.a"

    # Sanity: verify each archive in the stable per-slice tree was actually
    # built for the platform we asked for. Catches regressions where a
    # future `cmake --build` change re-introduces the cross-pollution
    # described above. Expected platform IDs from mach-o/loader.h:
    #   iphoneos        -> 2 (PLATFORM_IOS)
    #   iphonesimulator -> 7 (PLATFORM_IOSSIMULATOR)
    local expected_platform
    case "${sdk}" in
        iphoneos)        expected_platform=2 ;;
        iphonesimulator) expected_platform=7 ;;
        *) echo "[ios-deps] unknown sdk '${sdk}'" >&2; exit 1 ;;
    esac
    for a in libssl.a libcrypto.a libjquic.a libenv.a; do
        verify_platform "${lib_dir}/${a}" "${expected_platform}" "${label}/${a}"
    done

    echo "[ios-deps] ${label} -> ${lib_dir}"
    ls -la "${lib_dir}"
}

# Verify a static archive's first object file carries the expected
# LC_BUILD_VERSION platform ID. Bails out the whole script on mismatch so
# we never silently ship a cross-platform .a into the xcframework merge.
verify_platform() {
    local archive="$1" expected="$2" tag="$3"
    local tmp obj plat
    tmp=$(mktemp -d)
    ( cd "${tmp}" && ar x "${archive}" 2>/dev/null )
    obj=$(ls "${tmp}"/*.o 2>/dev/null | head -n1 || true)
    if [ -z "${obj}" ]; then
        rm -rf "${tmp}"
        echo "[ios-deps] WARN: ${tag} contains no .o files; skipping platform check" >&2
        return 0
    fi
    plat=$(otool -l "${obj}" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1; next} f && /platform/{print $2; exit}')
    rm -rf "${tmp}"
    if [ "${plat}" != "${expected}" ]; then
        echo "============================================================" >&2
        echo "ERROR: ${tag} was built for platform=${plat}, expected ${expected}." >&2
        echo "       This usually means a previous build of a different SDK" >&2
        echo "       overwrote the shared deps/<lib>/lib/iOS/<arch>/Release/" >&2
        echo "       output bucket. Wipe build/ios-deps/ and re-run." >&2
        echo "============================================================" >&2
        exit 1
    fi
}

for slice in "${SLICES[@]}"; do
    IFS=':' read -r sdk arch platform <<<"${slice}"
    build_one_slice "${sdk}" "${arch}" "${platform}"
done

echo "[ios-deps] all slices done. Output under ${OUT_ROOT}"
