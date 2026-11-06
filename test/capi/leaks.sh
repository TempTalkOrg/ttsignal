#!/bin/bash
# C ABI 释放路径的泄漏门槛（macOS）：在非 ASan 的 Debug 构建下用 leaks --atExit 跑 capi_connect_stub_release_test
# （反复 connect / 关连接 / 建关 connector）与 udpsendergroup_release_test，要求 0 泄漏。
# 覆盖：connect stub（IRPCStub）、xquic h3 上下文（xqc_h3_ctx_init）、UDPSenderGroup 内的 UDPSender。
#
#   test/capi/leaks.sh
#
# ASan 构建不能用 leaks（分配器被替换）；ASan 下跑同一组测试查越界 / UAF。
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO}/build/tests-debug"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
[ "$(uname)" = Darwin ] || { echo "leaks.sh 只在 macOS 上运行" >&2; exit 2; }

cmake -S "${REPO}/src" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON -DBUILD_TESTS=ON >/dev/null
cmake --build "${BUILD_DIR}" --target capi_connect_stub_release_test udpsendergroup_release_test -j "${JOBS}" >/dev/null

rc=0
LOG="$(mktemp "${TMPDIR:-/tmp}/ttsignal-leaks-XXXXXX")"
trap 'rm -f "${LOG}"' EXIT
for t in capi_connect_stub_release_test udpsendergroup_release_test; do
    # 输出写文件而不是命令替换：leaks 异常退出时被检查进程可能停在 T 状态并占着管道，命令替换会一直等 EOF
    MallocStackLogging=1 timeout 300 leaks --atExit -- "${BUILD_DIR}/bin/${t}" >"${LOG}" 2>&1 || true
    out="$(cat "${LOG}")"
    summary="$(grep -E 'leaks for [0-9]+ total leaked bytes' <<<"${out}" | tail -1 || true)"
    echo "${t}: ${summary:-（没有 leaks 摘要）}"
    if ! grep -q 'PASS' <<<"${out}"; then echo "${t} 本身失败" >&2; rc=1; fi
    if ! grep -qE ': 0 leaks for 0 total leaked bytes' <<<"${out}"; then
        grep -E "^STACK OF [0-9]+ INSTANCES" <<<"${out}" | sort | uniq -c >&2 || true
        rc=1
    fi
done
[ "${rc}" = 0 ] && echo PASS
exit "${rc}"
