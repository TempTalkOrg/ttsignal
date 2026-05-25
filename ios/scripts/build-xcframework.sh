#!/usr/bin/env bash
# ============================================================================
# ios/scripts/build-xcframework.sh
#
# Combine the per-slice libttsignal_ios.a archives + dependency archives +
# public headers + module.modulemap into a single TTSignal.xcframework.
#
# Two physical slices: device (arm64) and simulator (arm64+x86_64 fat).
# This is the structure Xcode expects for "iOS device" / "iOS Simulator".
#
# Run AFTER build-deps.sh AND build-core.sh.
#
# Defaults to Release. Pass `BUILD_TYPE=Debug` to assemble a debuggable
# xcframework (built from `build/ios-<slice>-debug/dist`, which itself
# requires `BUILD_TYPE=Debug ios/scripts/build-core.sh` first). The
# output path stays at `build/ios-xcframework/TTSignal.xcframework`
# either way — Debug *overwrites* Release, so QUICTest doesn't need any
# .xcodeproj surgery to switch debug vs release; just rebuild the
# xcframework with the BUILD_TYPE you want.
#
# Output: build/ios-xcframework/TTSignal.xcframework
# ============================================================================
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
case "${BUILD_TYPE}" in
    Release|Debug) ;;
    *)
        echo "ERROR: BUILD_TYPE must be Release or Debug, got '${BUILD_TYPE}'" >&2
        exit 1
        ;;
esac
BUILD_SUFFIX="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"

# Deployment target & SDK version embedded in the partial-link .o via
# LC_BUILD_VERSION. xcodebuild -create-xcframework rejects mismatched platforms,
# so this MUST line up with whatever build-deps.sh / build-core.sh used.
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"

# ----------------------------------------------------------------------------
# Symbol-collision firewall.
#
# Why: ttsignal ships its own BoringSSL (libssl.a + libcrypto.a) compiled into
# the xcframework, and jquic is built against BoringSSL's headers — meaning
# `xqc_create_client_ssl_ctx` etc. expect BoringSSL's `SSL_CTX` struct layout.
# A typical iOS host app already links its own copy of OpenSSL (or a different
# BoringSSL) for unrelated reasons (gRPC, Firebase, networking SDKs, ...). The
# linker sees two definitions of `_SSL_CTX_new`, picks one based on archive
# order, and the *other* copy's call sites end up calling the wrong
# implementation. Because the structs differ, the very first SSL_CTX_new()
# returns a pointer that jquic immediately dereferences at the wrong offset
# and crashes (typical signature: SIGSEGV inside SSL_CTX_new on a wild
# address like 0x73910043fd, called from xqc_create_client_ssl_ctx).
#
# Fix: partial-link each slice with -exported_symbols_list so that ONLY the
# public `_tt_*` C-ABI surface stays as external (uppercase T in nm); every
# other global symbol — `_SSL_CTX_new`, `_X509_*`, `_EVP_*`, all of jquic,
# all of env — gets demoted to private extern (lowercase t/s/d). Cross-
# object calls inside the partial link are still resolved (ld -r keeps them
# visible internally), but the host-app linker no longer sees those symbols
# at all, so it can't accidentally bind jquic's `SSL_CTX_new` call to the
# host's OpenSSL.
#
# Wildcards in -exported_symbols_list are supported by Apple's ld64. We rely
# on the project-wide `tt_` (and `_tt_` after Mach-O symbol mangling) prefix
# convention — every public C symbol is named `tt_*`, see ios_bridge.h /
# ios_logger.h / AppleNetworkMonitor.mm.
# ----------------------------------------------------------------------------
KEEP_LIST="$(mktemp -t ttsignal_keep.XXXXXX)"
trap 'rm -f "${KEEP_LIST}"' EXIT
cat > "${KEEP_LIST}" <<'EOF'
_tt_*
EOF

OUT_ROOT="${REPO_ROOT}/build/ios-xcframework"
STAGING="${OUT_ROOT}/_staging"
rm -rf "${OUT_ROOT}" && mkdir -p "${OUT_ROOT}" "${STAGING}"

DEVICE_LABEL="ios-device-arm64-${BUILD_SUFFIX}"
SIM_ARM_LABEL="ios-sim-arm64-${BUILD_SUFFIX}"
SIM_X64_LABEL="ios-sim-x64-${BUILD_SUFFIX}"

DEVICE_DIST="${REPO_ROOT}/build/${DEVICE_LABEL}/dist"
SIM_ARM_DIST="${REPO_ROOT}/build/${SIM_ARM_LABEL}/dist"
SIM_X64_DIST="${REPO_ROOT}/build/${SIM_X64_LABEL}/dist"

DEVICE_DEPS="${REPO_ROOT}/build/ios-deps/iphoneos-arm64/lib"
SIM_ARM_DEPS="${REPO_ROOT}/build/ios-deps/iphonesimulator-arm64/lib"
SIM_X64_DEPS="${REPO_ROOT}/build/ios-deps/iphonesimulator-x86_64/lib"

for d in "${DEVICE_DIST}" "${SIM_ARM_DIST}" "${SIM_X64_DIST}" \
         "${DEVICE_DEPS}" "${SIM_ARM_DEPS}" "${SIM_X64_DEPS}"; do
    if [ ! -d "${d}" ]; then
        echo "ERROR: missing build artefact dir: ${d}" >&2
        echo "       Run build-deps.sh + build-core.sh first." >&2
        exit 1
    fi
done

# ----------------------------------------------------------------------------
# Step 1 — for each slice fuse all .a's into ONE partial-linked .o, with
# everything except the `_tt_*` public ABI demoted to private-extern. Then
# wrap that single .o in a static archive so the xcframework slice has the
# usual one-archive layout.
#
# The partial link is what actually firewalls our BoringSSL from the host
# app's OpenSSL (see KEEP_LIST setup above for the rationale). libtool
# -static alone would just unify the archives and leave every symbol
# externally visible, which is exactly what causes the SSL_CTX_new
# collision crash.
#
# Args:
#   $1 = output libTTSignal.a path
#   $2 = arch (arm64 / x86_64)
#   $3 = ld -platform_version platform name (ios / ios-simulator)
#   $4 = SDK name (iphoneos / iphonesimulator) — used to discover SDK_VERSION
#   $5..N = input static archives to fuse
# ----------------------------------------------------------------------------

merge_archive() {
    local out="$1" arch="$2" plat="$3" sdk="$4"
    shift 4

    local sdk_version
    sdk_version=$(xcrun --sdk "${sdk}" --show-sdk-version)

    local work
    work="$(mktemp -d -t ttsignal_merge.XXXXXX)"

    # Explode every .a into its own subdir. Two reasons we don't just `ar x`
    # them all into one directory: (1) different deps occasionally produce
    # member files with identical basenames (e.g. multiple `init.c.o`), and
    # whichever `ar x` runs last would silently overwrite the earlier one;
    # (2) keeping per-archive subdirs makes the resulting object listing
    # easier to scan when something goes wrong.
    local objlist="${work}/objs.list"
    : > "${objlist}"
    local i=0
    for a in "$@"; do
        local base
        base="$(basename "${a}" .a)"
        local sub="${work}/${base}-${i}"
        mkdir -p "${sub}"
        ( cd "${sub}" && ar x "${a}" )
        # Some boringssl members on macOS get archived as zero-symbol .o
        # (e.g. arch-specific cpu-*.c that compiles to nothing on iOS). They
        # are harmless to feed to ld -r — keep them so we don't mask a real
        # bug behind an over-eager filter.
        find "${sub}" -name '*.o' -print >> "${objlist}"
        i=$((i+1))
    done

    # Use -filelist to avoid blowing out ARG_MAX with the (often 1000+) .o
    # paths from libcrypto alone.
    local merged="${work}/merged.o"
    xcrun ld -r \
        -arch "${arch}" \
        -platform_version "${plat}" "${DEPLOYMENT_TARGET}" "${sdk_version}" \
        -exported_symbols_list "${KEEP_LIST}" \
        -filelist "${objlist}" \
        -o "${merged}"

    # libtool will preserve the private-extern bits we just set on every
    # non-_tt_* global, so the final archive only exposes the public surface.
    libtool -static -o "${out}" "${merged}"

    # Sanity: confirm `_tt_*` is the only externally-visible symbol class
    # in the merged archive. If a future refactor introduces a new public
    # name without updating KEEP_LIST, we want to fail loudly *here* rather
    # than ship an xcframework that breaks consumer linking.
    sanity_check_localized "${out}" "${arch}"

    rm -rf "${work}"
}

# Verify the localized archive: every symbol that's still external (T/D/B
# in `nm`) must match the `_tt_*` pattern. Fails the build with a leaked-
# symbol report if anything else snuck through.
sanity_check_localized() {
    local archive="$1" arch="$2"
    local leaks
    # nm -g lists only externally visible symbols. -arch picks the right
    # slice when the input archive is fat (single-arch here, but be
    # explicit). `U` undefineds are fine — those are dependencies on
    # libSystem (pthread, malloc, etc.) the host app provides.
    leaks=$(nm -g -arch "${arch}" "${archive}" 2>/dev/null \
        | awk '$2 != "U" && $3 !~ /^_tt_/ && $3 != "" { print $3 }' \
        | sort -u || true)
    if [ -n "${leaks}" ]; then
        echo "============================================================" >&2
        echo "ERROR: ${archive} still exports non-_tt_ external symbols:" >&2
        echo "${leaks}" | head -n 40 | sed 's/^/  /' >&2
        local n
        n=$(echo "${leaks}" | wc -l | tr -d ' ')
        if [ "${n}" -gt 40 ]; then
            echo "  ... and $((n - 40)) more" >&2
        fi
        echo "Update KEEP_LIST in ios/scripts/build-xcframework.sh." >&2
        echo "============================================================" >&2
        exit 1
    fi
    echo "[xcframework] ${archive} (${arch}): only _tt_* exported (OK)"
}

DEVICE_FAT="${STAGING}/device-arm64/libTTSignal.a"
mkdir -p "$(dirname "${DEVICE_FAT}")"
merge_archive "${DEVICE_FAT}" arm64 ios iphoneos \
    "${DEVICE_DIST}/lib/libttsignal_ios.a" \
    "${DEVICE_DEPS}/libjquic.a" \
    "${DEVICE_DEPS}/libssl.a" \
    "${DEVICE_DEPS}/libcrypto.a" \
    "${DEVICE_DEPS}/libenv.a"

# Simulator slice is a fat binary (arm64 + x86_64). Build per-arch unified
# archives first, then `lipo -create` them.
SIM_ARM_FAT="${STAGING}/sim-arm64-merged/libTTSignal.a"
SIM_X64_FAT="${STAGING}/sim-x64-merged/libTTSignal.a"
mkdir -p "$(dirname "${SIM_ARM_FAT}")" "$(dirname "${SIM_X64_FAT}")"
merge_archive "${SIM_ARM_FAT}" arm64 ios-simulator iphonesimulator \
    "${SIM_ARM_DIST}/lib/libttsignal_ios.a" \
    "${SIM_ARM_DEPS}/libjquic.a" \
    "${SIM_ARM_DEPS}/libssl.a" \
    "${SIM_ARM_DEPS}/libcrypto.a" \
    "${SIM_ARM_DEPS}/libenv.a"
merge_archive "${SIM_X64_FAT}" x86_64 ios-simulator iphonesimulator \
    "${SIM_X64_DIST}/lib/libttsignal_ios.a" \
    "${SIM_X64_DEPS}/libjquic.a" \
    "${SIM_X64_DEPS}/libssl.a" \
    "${SIM_X64_DEPS}/libcrypto.a" \
    "${SIM_X64_DEPS}/libenv.a"

SIM_FAT_DIR="${STAGING}/sim-fat"
mkdir -p "${SIM_FAT_DIR}"
SIM_FAT="${SIM_FAT_DIR}/libTTSignal.a"
lipo -create -output "${SIM_FAT}" "${SIM_ARM_FAT}" "${SIM_X64_FAT}"

# ----------------------------------------------------------------------------
# Step 2 — assemble per-slice "Headers" + module.modulemap directories.
# These are passed to xcodebuild via -headers in the create-xcframework call.
# ----------------------------------------------------------------------------

DEVICE_HDRS="${STAGING}/device-arm64/Headers"
SIM_HDRS="${STAGING}/sim-fat/Headers"
mkdir -p "${DEVICE_HDRS}" "${SIM_HDRS}"
cp -f "${DEVICE_DIST}/Headers/"*.h "${DEVICE_HDRS}/"
cp -f "${SIM_ARM_DIST}/Headers/"*.h "${SIM_HDRS}/"

# Stage module.modulemap into the same Headers dir — xcodebuild includes
# adjacent module maps automatically when the -headers directory is passed.
cp -f "${DEVICE_DIST}/Modules/module.modulemap" "${DEVICE_HDRS}/module.modulemap"
cp -f "${SIM_ARM_DIST}/Modules/module.modulemap" "${SIM_HDRS}/module.modulemap"

# ----------------------------------------------------------------------------
# Step 3 — xcodebuild -create-xcframework
# ----------------------------------------------------------------------------

XCFW_OUT="${OUT_ROOT}/TTSignal.xcframework"
rm -rf "${XCFW_OUT}"

xcodebuild -create-xcframework \
    -library "${DEVICE_FAT}" -headers "${DEVICE_HDRS}" \
    -library "${SIM_FAT}"    -headers "${SIM_HDRS}" \
    -output  "${XCFW_OUT}"

echo "============================================================"
echo "[xcframework] OK (${BUILD_TYPE}) -> ${XCFW_OUT}"
echo "[xcframework] swift sources: ${REPO_ROOT}/src/swift/"
echo "============================================================"
ls -la "${XCFW_OUT}"
