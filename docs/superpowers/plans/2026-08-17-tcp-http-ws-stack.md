# TCP/TLS/HTTP/WS 客户端栈 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 ttsignal 支持 https/ws/wss，并让 `vpnPolicy` 三档策略在 TCP 与 DNS 两条路径上都生效，使 `force-physical` 下能用用户真实 IP 出网访问 `getServiceCallUrl` 这类接口。

**Architecture:** 抽出 `SocketPinner`（fd→网卡绑定）、`DnsResolver`（绕 VPN 的 UDP DNS）、`TlsContext`（SSL_CTX 工厂 + 证书校验）、`TcpChannel`（连接状态机）四层底座，`WSConnection` 与新增的 `HttpConnection` 共用。`UDPSender` 改为调用 `SocketPinner`，QUIC 侧与 TCP 侧共享同一份平台绑定代码。

**Tech Stack:** C++17、BC 框架（`deps/env`）、BoringSSL（`deps/boringssl`）、llhttp（`deps/llhttp`）、CMake

**Spec:** `docs/superpowers/specs/2026-08-17-tcp-http-ws-stack-design.md`

## Global Constraints

- **构建当前是坏的。** `file(GLOB ${BASE_DIR}/cpp/*.cpp)`（`src/CMakeLists.txt:220,401,490`）会把拷贝进来的 `WSConnector.cpp` / `WSParser.cpp` / `WSServer.cpp` 收进编译，而它们 include 不存在的 `Acceptor.h` / `JMPacket.h`。**Task 1 必须先恢复绿色构建**，否则后续任何任务都无法验证。
- **`src/cpp/*.cpp` 是 GLOB 自动收录的**，新增 `.cpp` 无需改 `CMakeLists.txt`；但 GLOB 在 configure 时求值，新增文件后必须重跑 `cmake`，不能只 `make`。
- 语言：注释与日志用中文，与 `VpnPolicy.h` / `UDPSender.cpp` 现有风格一致。标识符、类型名保持英文。
- 错误码惯例：`BC_R_NRESULTS + N`（见 `WSConnector.cpp:32-33`）。不要发明 `TT_ERR_*` 前缀。
- 单测惯例：`src/cpp/tests/*_test.cpp` 是 standalone 文件，**不进任何构建目标**，靠文件头注释里的 `c++` 命令手动编译运行（见 `src/cpp/tests/VpnPolicy_test.cpp:1-10`）。用 `CHECK(cond)` 宏累加 `g_failures`，`main` 返回 `g_failures != 0`。
- 头文件包含路径（手动编译单测和语法检查时用）：
  `-Isrc/cpp -Ideps/env/src -Ideps/llhttp/include -Ideps/boringssl/src/include`
- BoringSSL 静态库按平台/架构/构建类型分目录，macOS arm64 Debug 是
  `deps/boringssl/lib/Darwin/arm64/Debug/`（另有 `Release/`，以及
  `Darwin/x86_64/`、`iOS/`、`Android/`、`Linux/`、`Windows/`）。单测链接时用
  `-L deps/boringssl/lib/Darwin/arm64/Debug -lssl -lcrypto`，在别的机器上按
  `uname -m` 换 `arm64` / `x86_64`。
- **Task 2 是纯搬家，行为必须零变化。** 它动的是已上线、踩过坑的 QUIC 路径。
- `vpnPolicy` 解析一律调用现成的 `tt_vpn_policy_resolve()`（`src/cpp/VpnPolicy.h:66`），不要重写。
- 提交信息不带 `Co-Authored-By` 或任何 AI 署名。

---

### Task 1: 恢复绿色构建

删掉本轮不做的服务端与绑定层文件，并把尚未移植的 `WSConnector.cpp` / `WSParser.cpp` 暂时排除出编译，直到 Task 7 移植完成再放回。`SSLayer.cpp` 保留在构建里——它只依赖 BC 和 OpenSSL，已验证可独立编译通过。

**Files:**
- Delete: `src/cpp/WSServer.h`, `src/cpp/WSServer.cpp`
- Delete: `src/cpp/jni/JNI_WSConnectorWrap.h`, `src/cpp/jni/JNI_WSConnectorWrap.cpp`
- Delete: `src/cpp/jni/JNI_WSServerWrap.h`, `src/cpp/jni/JNI_WSServerWrap.cpp`
- Modify: `src/CMakeLists.txt`（三处 GLOB 之后，各加一段 `list(REMOVE_ITEM ...)`）

**Interfaces:**
- Consumes: 无
- Produces: 一个能 configure + build 通过的仓库。后续所有任务的验证前提。

- [ ] **Step 1: 先复现失败**

```bash
cd /Users/antonio/chative/ttsignal
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON
make ttsignal 2>&1 | grep -E "error|Error"
```

Expected: `src/cpp/WSConnector.h:18:10: fatal error: 'Acceptor.h' file not found`

- [ ] **Step 2: 删除服务端与 JNI 绑定文件**

```bash
cd /Users/antonio/chative/ttsignal
git rm -f --ignore-unmatch src/cpp/WSServer.h src/cpp/WSServer.cpp
rm -f src/cpp/WSServer.h src/cpp/WSServer.cpp
rm -f src/cpp/jni/JNI_WSConnectorWrap.h src/cpp/jni/JNI_WSConnectorWrap.cpp
rm -f src/cpp/jni/JNI_WSServerWrap.h src/cpp/jni/JNI_WSServerWrap.cpp
```

这些文件是 `~/work/jmp/jmp/src/cpp/` 的逐字节拷贝（未纳入 git 追踪，所以 `git rm` 加 `--ignore-unmatch`）。下轮做服务端/JNI 时从 jmp 重新拷一份改起，成本与现在一样。

- [ ] **Step 3: 在 CMakeLists 排除尚未移植的两个文件**

在 `src/CMakeLists.txt` 中，紧跟每一处 `file(GLOB ...)` 之后插入排除语句。共三处。

第一处，`src/CMakeLists.txt:219` 的 Node addon GLOB 之后：

```cmake
    file(GLOB NODE_ADDON_SRC_FILES
        ${BASE_DIR}/cpp/*.cpp
        ${BASE_DIR}/cpp/http-parser/*.c
        ${NODE_ADDON_SRC_DIR}/*.cpp
    )
    # WSConnector.cpp / WSParser.cpp 仍是 jmp 的原样拷贝，依赖 Acceptor.h 与
    # JMPacket.h（本项目都没有）。Task 7 移植接入 TcpChannel 后删掉这段排除。
    list(REMOVE_ITEM NODE_ADDON_SRC_FILES
        ${BASE_DIR}/cpp/WSConnector.cpp
        ${BASE_DIR}/cpp/WSParser.cpp
    )
```

第二处，`src/CMakeLists.txt:400` 的 JNI GLOB 之后，同样插入：

```cmake
    list(REMOVE_ITEM JNI_SRC_FILES
        ${BASE_DIR}/cpp/WSConnector.cpp
        ${BASE_DIR}/cpp/WSParser.cpp
    )
```

第三处，`src/CMakeLists.txt:489` 的 iOS GLOB 之后：

```cmake
    list(REMOVE_ITEM TT_IOS_CORE_SRC
        ${BASE_DIR}/cpp/WSConnector.cpp
        ${BASE_DIR}/cpp/WSParser.cpp
    )
```

注意保持每处 GLOB 原有的变量名与文件列表不变，只在其后追加 `list(REMOVE_ITEM ...)`。

- [ ] **Step 4: 验证构建恢复**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON
make ttsignal -j8 2>&1 | tail -5
```

Expected: 编译成功，最后一行形如 `[100%] Built target ttsignal`，无 `Error`。

- [ ] **Step 5: 确认 SSLayer.cpp 确实还在编译目标里**

```bash
grep -o "cpp/SSLayer\.cpp" /tmp/ttbuild/CMakeFiles/ttsignal.dir/build.make | sort -u
```

Expected: 输出 `cpp/SSLayer.cpp`（一行）。若为空说明误排除了它。

- [ ] **Step 6: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add -A src/CMakeLists.txt src/cpp
git commit -m "build: 恢复绿色构建，移除本轮不做的 WS 服务端与 JNI 绑定

拷贝进来的 WS* 文件被 file(GLOB src/cpp/*.cpp) 自动收进编译目标，
而它们 include 不存在的 Acceptor.h / JMPacket.h，导致构建失败。

- 删除 WSServer.h/.cpp 与 jni/JNI_WS*（本轮不做服务端与绑定层）
- 三处 GLOB 后排除 WSConnector.cpp / WSParser.cpp，Task 7 移植后放回
- SSLayer.cpp 保留在构建中，它只依赖 BC 与 OpenSSL，可独立编译"
```

---

### Task 2: SocketPinner 抽取（行为零变化）

把 `UDPSender::_InitSocket` 里的平台绑定代码和 `_TryClearInterfaceBinding` 里的解绑代码抽成独立模块，`UDPSender` 改为调用它。**这一步不改变任何行为**，只是搬家，让 TCP 侧后续能复用同一份实现。

**Files:**
- Create: `src/cpp/SocketPinner.h`
- Create: `src/cpp/SocketPinner.cpp`
- Create: `src/cpp/tests/SocketPinner_test.cpp`
- Modify: `src/cpp/UDPSender.cpp:530-681`（绑定块 → 调用 `tt_socket_pin`）
- Modify: `src/cpp/UDPSender.cpp:1304-1358`（解绑块 → 调用 `tt_socket_unpin`）

**Interfaces:**
- Consumes: `TTVpnPolicy`（`src/cpp/VpnPolicy.h:27`）
- Produces:
  - `TTPinResult tt_socket_pin(const TTPinRequest*, char* outMethod, size_t outMethodLen, int* outErrno)`
  - `TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx)`
  - `enum TTPinResult { TT_PIN_OK=0, TT_PIN_NOT_NEEDED=1, TT_PIN_UNSUPPORTED=2, TT_PIN_FAILED=3 }`
  - `struct TTPinRequest { int fd; uint32_t ifIndex; uint64_t androidNetHandle; int ipv6; int isTcp; TTVpnPolicy policy; void* loggerCtx; }`

- [ ] **Step 1: 写失败的测试**

Create `src/cpp/tests/SocketPinner_test.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : SocketPinner_test.cpp
//
// Standalone unit test for the socket-pinning helpers. Not part of any build
// target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
//       src/cpp/SocketPinner.cpp src/cpp/tests/SocketPinner_test.cpp \
//       -o /tmp/socketpinner_test && /tmp/socketpinner_test
//
///////////////////////////////////////////////////////////////////////////////
#include "SocketPinner.h"

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// ifIndex 为 0 表示调用方没有要求绑定，必须原样返回 NOT_NEEDED 且不碰 fd。
static void test_zero_ifindex_is_not_needed()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;              // 故意给非法 fd：不该被使用
    req.ifIndex = 0;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    char method[32] = "unset";
    int  err        = -1;
    CHECK(tt_socket_pin(&req, method, sizeof(method), &err) == TT_PIN_NOT_NEEDED);
    CHECK(strcmp(method, "") == 0);   // 未绑定时 method 置空串
    CHECK(err == 0);
}

// 传 NULL 必须安全返回，不能崩。
static void test_null_request_is_rejected()
{
    CHECK(tt_socket_pin(NULL, NULL, 0, NULL) == TT_PIN_FAILED);
}

// 非法 fd 且 ifIndex 非 0 时必须返回 FAILED，不能崩，也不能报成功。
static void test_bad_fd_fails()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;
    req.ifIndex = 1;               // loopback，任何平台都存在
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    CHECK(tt_socket_pin(&req, NULL, 0, NULL) == TT_PIN_FAILED);
}

// outMethod / outErrno 允许传 NULL，不能因此崩溃。
static void test_null_out_params_are_optional()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = fd;
    req.ifIndex = 1;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    // 只要不崩、返回值属于合法枚举即可。绑 loopback 在不同平台结果不同，
    // 所以这里不断言具体成败。
    TTPinResult r = tt_socket_pin(&req, NULL, 0, NULL);
    CHECK(r == TT_PIN_OK || r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);

    close(fd);
}

// 成功绑定时必须填出实际生效的手段名，供日志与诊断使用。
static void test_method_name_is_reported_on_success()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = fd;
    req.ifIndex = 1;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    char method[32] = "";
    TTPinResult r = tt_socket_pin(&req, method, sizeof(method), NULL);
    if (r == TT_PIN_OK) {
        CHECK(strlen(method) > 0);
    }

    close(fd);
}

// unpin 对非法 fd 必须安全返回。
static void test_unpin_bad_fd()
{
    TTPinResult r = tt_socket_unpin(-1, 0, NULL);
    CHECK(r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);
}

int main()
{
    test_zero_ifindex_is_not_needed();
    test_null_request_is_rejected();
    test_bad_fd_fails();
    test_null_out_params_are_optional();
    test_method_name_is_reported_on_success();
    test_unpin_bad_fd();

    if (g_failures == 0) {
        printf("SocketPinner_test: ALL PASS\n");
    } else {
        printf("SocketPinner_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
    src/cpp/SocketPinner.cpp src/cpp/tests/SocketPinner_test.cpp \
    -o /tmp/socketpinner_test
```

Expected: FAIL —— `no such file or directory: src/cpp/SocketPinner.cpp`

- [ ] **Step 3: 写 SocketPinner.h**

Create `src/cpp/SocketPinner.h`：

```c
///////////////////////////////////////////////////////////////////////////////
// file   : SocketPinner.h
// author : anto
//
// 把一个 socket fd 绑定到指定网卡。本文件是整个项目里唯一知道
// IP_BOUND_IF / IPV6_BOUND_IF / SO_BINDTODEVICE / IP_UNICAST_IF /
// IPV6_UNICAST_IF / android_setsocknetwork 这些平台选项的地方。
//
// 从 UDPSender::_InitSocket 与 UDPSender::_TryClearInterfaceBinding 抽出，
// 供 UDP（QUIC）与 TCP（HTTP/WS）两条路径共用。
//
// ⚠️ TCP 的硬约束：tt_socket_pin 必须在 connect() 之前调用。
//    * macOS/iOS —— IP_BOUND_IF 对已建立连接的缓存路由无效
//    * Linux     —— TCP 的路由在 connect() 时确定；SO_BINDTODEVICE 在
//                   connect 后设置不会改变已建立的连接
//    * Windows   —— IP_UNICAST_IF 文档明确要求在 connect 前设置
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_SOCKET_PINNER_H
#define TT_SOCKET_PINNER_H

#include <stdint.h>
#include <stddef.h>

#include "VpnPolicy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TT_PIN_OK          = 0,  // 绑定成功
    TT_PIN_NOT_NEEDED  = 1,  // ifIndex / androidNetHandle 均为 0，调用方未要求绑定
    TT_PIN_UNSUPPORTED = 2,  // 当前平台无此能力
    TT_PIN_FAILED      = 3,  // 参数非法，或 setsockopt 失败（errno 见 outErrno）
} TTPinResult;

typedef struct TTPinRequest {
    int         fd;                // 待绑定的 socket
    uint32_t    ifIndex;           // 0 = 不绑定。非 Android 平台使用
    uint64_t    androidNetHandle;  // Android 用 net handle 而非 ifIndex
    int         ipv6;              // 非 0 时同时设置 IPv6 版本的选项
    int         isTcp;             // 非 0 表示 TCP。仅影响日志措辞，见下
    TTVpnPolicy policy;            // FORCE_PHYSICAL 时 Linux 优先 SO_BINDTODEVICE
    void*       loggerCtx;         // LogQ 上下文，可为 NULL
} TTPinRequest;

// 把 req->fd 绑定到 req->ifIndex 指定的网卡。
//
// outMethod     : 可为 NULL。成功时写入实际生效的手段名（"IP_BOUND_IF" /
//                 "SO_BINDTODEVICE" / "IP_UNICAST_IF" /
//                 "android_setsocknetwork"），未绑定时写入空串。
// outMethodLen  : outMethod 缓冲区大小。建议 >= 32。
// outErrno      : 可为 NULL。失败时写入 errno（Windows 为 WSAGetLastError）。
//
// isTcp 只影响一处行为：Linux 上 force-physical 时若 SO_BINDTODEVICE 失败
// （缺 CAP_NET_RAW），UDP 路径打 _WARN_ 并说明"可能拿不到真实 IP"——因为
// IP_UNICAST_IF 对已 connect 的 UDP socket 在旧内核上被静默忽略；TCP 路径
// 只打 _INFO_，因为 IP_UNICAST_IF 对 connect 前的 TCP socket 是有效的。
TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno);

// 撤销绑定，把选项设回 0。UDPSender 的 fake-IP unpin 路径使用。
//
// ⚠️ force-physical 下调用方不应调用本函数（既有约定，见 commit 652a603）。
//    本函数不检查策略，约束由调用方保证。
//
// Android 无公开 API 撤销 android_setsocknetwork，该平台返回 TT_PIN_UNSUPPORTED。
TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx);

#ifdef __cplusplus
}
#endif

#endif // TT_SOCKET_PINNER_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 4: 写 SocketPinner.cpp**

Create `src/cpp/SocketPinner.cpp`。实现方式是把 `UDPSender.cpp` 的两段代码整体搬过来，逐条对应：

| 搬运源 | 搬到 | 变换 |
|---|---|---|
| `UDPSender.cpp:530-549`（Android） | `tt_socket_pin` 的 `#ifdef OS_ANDROID` 分支 | `m_nNetworkHandle` → `req->androidNetHandle`；`m_pSocket->GetFd()` → `req->fd`；`m_pLoggerCtx` → `req->loggerCtx`；`m_bInterfaceBindingActive = true` → `return TT_PIN_OK` |
| `UDPSender.cpp:550-577`（Apple） | `#elif defined(__APPLE__)` 分支 | 同上，另 `m_sConfig.ipv6` → `req->ipv6` |
| `UDPSender.cpp:578-654`（Linux） | `#elif defined(__linux__) && !defined(OS_ANDROID)` 分支 | 同上，另 `m_eVpnPolicy` → `req->policy`；SO_BINDTODEVICE 失败时的日志级别按 `req->isTcp` 分流 |
| `UDPSender.cpp:655-680`（Windows） | `#elif defined(_WIN32)` 分支 | 同上 |
| `UDPSender.cpp:1304-1358`（解绑） | `tt_socket_unpin` | `m_pSocket->GetFd()` → 参数 `fd`；`m_sConfig.ipv6` → 参数 `ipv6`；去掉 `m_bInterfaceBindingActive` 的读写（由调用方维护） |

**必须原样保留的东西**（它们是踩坑记录，不是可有可无的注释）：

- `UDPSender.cpp:588-600` 关于 `SO_BINDTODEVICE` 需要 `CAP_NET_RAW`、`IP_UNICAST_IF` 在 kernel < 6.0.16/6.1.2/6.2 上对已 connect UDP socket 被静默忽略的整段中文注释
- `UDPSender.cpp:581-582` 关于 Linux ifIndex 是主机字节序、**不要** `htonl` 的警告
- `UDPSender.cpp:655-658` 关于 Windows IPv4 需要 `htonl`、IPv6 保持主机序的说明
- `UDPSender.cpp:1268-1303` `tt_socket_unpin` 上方那段各平台解绑语义的长注释

文件骨架：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : SocketPinner.cpp
// author : anto
//
// 平台相关的 socket 网卡绑定实现。代码从 UDPSender::_InitSocket 与
// UDPSender::_TryClearInterfaceBinding 抽出，行为保持一致。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "SocketPinner.h"
#include "Utils.h"

#include <string.h>
#include <errno.h>

#ifdef OS_ANDROID
#include <dlfcn.h>
#include <android/api-level.h>
#endif

#if defined(__linux__) && !defined(OS_ANDROID)
#include <net/if.h>     // if_indextoname / IF_NAMESIZE for SO_BINDTODEVICE
// IPV6_UNICAST_IF 在 5.7 之前的 uapi 头里没有，但它是稳定的内核 ABI。
#ifndef IPV6_UNICAST_IF
#define IPV6_UNICAST_IF 76
#endif
#endif

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

// outMethod 允许为 NULL；统一走这个 helper 避免每个分支都判空。
static void _set_method(char* outMethod, size_t len, const char* name)
{
    if (outMethod == NULL || len == 0) return;
    snprintf(outMethod, len, "%s", name ? name : "");
}

static void _set_errno(int* outErrno, int value)
{
    if (outErrno) *outErrno = value;
}

TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno)
{
    _set_method(outMethod, outMethodLen, "");
    _set_errno(outErrno, 0);

    if (req == NULL) {
        return TT_PIN_FAILED;
    }

#ifdef OS_ANDROID
    if (req->androidNetHandle == 0) {
        return TT_PIN_NOT_NEEDED;
    }
#else
    if (req->ifIndex == 0) {
        return TT_PIN_NOT_NEEDED;
    }
#endif

    if (req->fd < 0) {
        _set_errno(outErrno, EBADF);
        return TT_PIN_FAILED;
    }

    // ... 四个平台分支，内容按上表从 UDPSender.cpp 搬运 ...

    return TT_PIN_UNSUPPORTED;
}

TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx)
{
    // ... 按上表从 UDPSender.cpp:1304-1358 搬运 ...
}
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
    src/cpp/SocketPinner.cpp src/cpp/tests/SocketPinner_test.cpp \
    -o /tmp/socketpinner_test && /tmp/socketpinner_test
```

Expected: `SocketPinner_test: ALL PASS`

若因 `StdAfx.h` / `Utils.h` 引入过多依赖导致单测链接不过，把 `LogQ` 调用改为通过一个本文件内的 `_pin_log(...)` 薄封装，并在单测编译命令里补上 `src/cpp/Utils.cpp`。不要为了让测试过而删掉日志。

- [ ] **Step 6: 改 UDPSender 调用新模块**

在 `src/cpp/UDPSender.cpp` 顶部 include 区加入：

```cpp
#include "SocketPinner.h"
```

把 `UDPSender.cpp:530-681` 整段（`#ifdef OS_ANDROID` 到对应 `#endif`）替换为：

```cpp
	// 平台相关的网卡绑定已抽到 SocketPinner，UDP 与 TCP 共用同一份实现。
	{
		TTPinRequest pin;
		memset(&pin, 0, sizeof(pin));
		pin.fd               = m_pSocket->GetFd();
		pin.ifIndex          = (uint32_t)m_nNetworkHandle;
		pin.androidNetHandle = (uint64_t)m_nNetworkHandle;
		pin.ipv6             = m_sConfig.ipv6 ? 1 : 0;
		pin.isTcp            = 0;
		pin.policy           = m_eVpnPolicy;
		pin.loggerCtx        = m_pLoggerCtx;

		char method[32] = "";
		int  pinErrno   = 0;
		TTPinResult pinResult =
			tt_socket_pin(&pin, method, sizeof(method), &pinErrno);
		if (pinResult == TT_PIN_OK)
		{
			m_bInterfaceBindingActive = true;
		}
	}
```

把 `UDPSender.cpp:1304-1358`（`#if defined(__APPLE__) || defined(_WIN32) || ...` 到对应 `#endif`）的 setsockopt 部分替换为对 `tt_socket_unpin(fd, m_sConfig.ipv6 ? 1 : 0, m_pLoggerCtx)` 的调用，**保留** `m_bInterfaceBindingActive` 的判断与置位逻辑（`1305-1315`、`1364`）以及 `1366-1370` 的 `_WARN_` 日志——那段日志解释了 TUN-mode VPN 的场景，对现场排查有用。

- [ ] **Step 7: 验证 QUIC 侧行为未变**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON
make ttsignal -j8 2>&1 | tail -3
```

Expected: `[100%] Built target ttsignal`

然后跑一次现有的 QUIC 冒烟：

```bash
cd /Users/antonio/chative/ttsignal
node js/server.js &
SERVER_PID=$!
sleep 2
node js/client.js 2>&1 | tail -20
kill $SERVER_PID
```

Expected: 客户端连接成功、收发数据正常，与改动前一致。日志里应出现 `UDP Sender: started at ...`。若 `js/client.js` 需要参数或远端服务，改为在日志里确认 `setsockopt(...)` 那行的输出内容与改动前逐字相同。

- [ ] **Step 8: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/SocketPinner.h src/cpp/SocketPinner.cpp \
        src/cpp/tests/SocketPinner_test.cpp src/cpp/UDPSender.cpp
git commit -m "refactor(net): 抽出 SocketPinner，UDP 与 TCP 共用网卡绑定实现

把 UDPSender::_InitSocket 的四个平台绑定分支和
_TryClearInterfaceBinding 的解绑分支搬进 SocketPinner，UDPSender 改为
调用。行为零变化，为 TCP 侧（HTTP/WS）复用同一份实现做准备。

原注释里关于 SO_BINDTODEVICE 需要 CAP_NET_RAW、IP_UNICAST_IF 在旧内核
上对已 connect UDP socket 无效、Linux 不要 htonl 而 Windows 要的说明
全部原样保留——那些是踩过的坑。"
```

---

### Task 3: TlsContext 抽取 + 修既有 bug

把 `SMPConnector.cpp` 里已经验证过的证书校验逻辑抽成不依赖 xquic 的独立模块，供 TLS 侧复用；同时建立集中的错误码头文件。

**Files:**
- Create: `src/cpp/TTErrors.h`
- Create: `src/cpp/TlsContext.h`
- Create: `src/cpp/TlsContext.cpp`
- Create: `src/cpp/tests/TlsContext_test.cpp`
- Modify: `src/cpp/SMPConnector.cpp:131-160`（`addTrustedCAsToStore` 等 helper 移走后改为引用）
- Modify: `src/cpp/SMPConnector.cpp:3441-3536`（校验主体改调 `TlsContext::VerifyChain`）

**Interfaces:**
- Consumes: 无（`SocketPinner` 与本任务无关）
- Produces:
  - `struct TlsConfig { std::string caCertsPem, spkiPin, clientCertFile, clientKeyFile, clientKeyPassword; bool insecureSkipVerify; void* loggerCtx; }`
  - `SSL_CTX* TlsContext::Create(const TlsConfig&, std::string& outErr)`
  - `BCRESULT TlsContext::PrepareSsl(SSL*, const std::string& host, const TlsConfig&, std::string& outErr)`
  - `BCRESULT TlsContext::VerifyChain(const std::vector<X509*>& chain, const std::string& host, const TlsConfig&, std::string& outErr)`
  - `std::string TlsContext::ComputeSpkiPinBase64(X509*)`
  - `std::string TlsContext::CanonicalizeSpkiPin(const char* pin, size_t len)`
  - 错误码：`BC_R_IDLE_TIMEOUT`(+1)、`BC_R_CONNECT_TIMEOUT`(+2)、`BC_R_PIN_FAILED`(+4)、`BC_R_ROUTE_MISMATCH`(+5)、`BC_R_DNS_FAILED`(+6)、`BC_R_TLS_VERIFY_FAILED`(+7)、`BC_R_RESPONSE_TOO_LARGE`(+8)
  - ⚠️ `BC_R_NO_PHYSICAL_INTERFACE` **不由本任务定义**，它已存在于 `deps/env/src/BC/Config.h:388`，取值 64，是跨语言契约值。`+3` 槽位空置。

- [ ] **Step 1: 写 TTErrors.h**

Create `src/cpp/TTErrors.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : TTErrors.h
// author : anto
//
// ttsignal 自定义的 BCRESULT 错误码，统一以 BC_R_NRESULTS + N 编号。
//
// 在此之前这些码散落在各 .cpp 里各自 #define（WSConnector.cpp 和
// WSServer.cpp 就各定义了一遍 BC_R_IDLE_TIMEOUT），集中到这里避免重复与
// 取值冲突。新增错误码请依次往下排，不要复用已有编号。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_ERRORS_H
#define TT_ERRORS_H

#include <BC/Config.h>

// 沿用既有取值，原先定义在 WSConnector.cpp:32-33
#define BC_R_IDLE_TIMEOUT               (BC_R_NRESULTS + 1)
#define BC_R_CONNECT_TIMEOUT            (BC_R_NRESULTS + 2)

// vpnPolicy 相关：策略要求物理网卡但拿不到
// BC_R_NO_PHYSICAL_INTERFACE 不在这里定义 —— deps/env/src/BC/Config.h:388
// 已经把它定成 64，而 64 是写进 src/swift/TTSignalConfig.swift:30,129 和
// src/js/index.js:1035 文档的跨语言契约值，不能改。BC_R_NRESULTS + 3 这个
// 槽位因此空置，不要拿它去定义别的码，避免将来有人对着编号连续性犯同样的错。
// force-physical 下 tt_socket_pin 失败
#define BC_R_PIN_FAILED                 (BC_R_NRESULTS + 4)
// 路由复核发现内核仍会把包送进隧道
#define BC_R_ROUTE_MISMATCH             (BC_R_NRESULTS + 5)
// 所有 DNS server 均失败
#define BC_R_DNS_FAILED                 (BC_R_NRESULTS + 6)
// 证书链 / 主机名 / SPKI pin 校验失败
#define BC_R_TLS_VERIFY_FAILED          (BC_R_NRESULTS + 7)
// 响应体超过 maxResponseBytes
#define BC_R_RESPONSE_TOO_LARGE         (BC_R_NRESULTS + 8)

#endif // TT_ERRORS_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 2: 写失败的测试**

Create `src/cpp/tests/TlsContext_test.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : TlsContext_test.cpp
//
// Standalone unit test. Not part of any build target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/boringssl/src/include \
//       src/cpp/TlsContext.cpp src/cpp/tests/TlsContext_test.cpp \
//       -L deps/boringssl/lib/Darwin/arm64/Debug -lssl -lcrypto \
//       -o /tmp/tlscontext_test && /tmp/tlscontext_test
//
///////////////////////////////////////////////////////////////////////////////
#include "TlsContext.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// caCerts 为空且未开 insecureSkipVerify 时，必须回落系统信任库并成功建 ctx。
// 这是与 SMPConnector 唯一有意的行为差异：SMP 为空是"跳过校验"，
// TLS 侧为空是"用系统信任库校验"。
static void test_empty_ca_falls_back_to_system_store()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    CHECK(err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// 显式传入的 CA 必须被加载，且不该报错。
static void test_explicit_ca_is_loaded()
{
    // certs/localhost.crt 是仓库里现成的自签证书
    FILE* f = fopen("certs/localhost.crt", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string pem;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pem.append(buf, n);
    fclose(f);

    TlsConfig cfg;
    cfg.caCertsPem = pem;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    CHECK(err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// 畸形 PEM 必须失败并给出可读原因，不能静默降级成"不校验"。
static void test_malformed_ca_fails_loudly()
{
    TlsConfig cfg;
    cfg.caCertsPem = "-----BEGIN CERTIFICATE-----\nbm90IGEgY2VydA==\n"
                     "-----END CERTIFICATE-----\n";
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx == nullptr);
    CHECK(!err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// SPKI pin 的规范化：去空白、剥掉 "sha256//" 前缀、保留 base64 本体。
static void test_spki_pin_canonicalization()
{
    const char* raw = "  sha256//YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg=  ";
    std::string got = TlsContext::CanonicalizeSpkiPin(raw, strlen(raw));
    CHECK(got == "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg=");

    const char* bare = "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg=";
    CHECK(TlsContext::CanonicalizeSpkiPin(bare, strlen(bare)) == bare);

    CHECK(TlsContext::CanonicalizeSpkiPin(nullptr, 0).empty());
}

// IP 字面量不能设 SNI（RFC 6066 禁止），域名必须设。
static void test_sni_skipped_for_ip_literal()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;

    SSL* ssl = SSL_new(ctx);
    CHECK(ssl != nullptr);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "192.0.2.1", cfg, err) == BC_R_SUCCESS);
        CHECK(SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) == nullptr);
        SSL_free(ssl);
    }

    ssl = SSL_new(ctx);
    CHECK(ssl != nullptr);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "example.com", cfg, err) == BC_R_SUCCESS);
        const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        CHECK(sni != nullptr && strcmp(sni, "example.com") == 0);
        SSL_free(ssl);
    }

    SSL_CTX_free(ctx);
}

// IPv6 字面量同样不设 SNI。
static void test_sni_skipped_for_ipv6_literal()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;

    SSL* ssl = SSL_new(ctx);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "2001:db8::1", cfg, err) == BC_R_SUCCESS);
        CHECK(SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) == nullptr);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
}

// 最低协议版本必须是 TLS 1.2。原 WSConnector.cpp:556 设的是 TLS1_1_VERSION。
static void test_min_proto_version_is_tls12()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;
    CHECK(SSL_CTX_get_min_proto_version(ctx) == TLS1_2_VERSION);
    SSL_CTX_free(ctx);
}

int main()
{
    test_empty_ca_falls_back_to_system_store();
    test_explicit_ca_is_loaded();
    test_malformed_ca_fails_loudly();
    test_spki_pin_canonicalization();
    test_sni_skipped_for_ip_literal();
    test_sni_skipped_for_ipv6_literal();
    test_min_proto_version_is_tls12();

    if (g_failures == 0) {
        printf("TlsContext_test: ALL PASS\n");
    } else {
        printf("TlsContext_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
```

- [ ] **Step 3: 运行测试确认失败**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/boringssl/src/include \
    src/cpp/TlsContext.cpp src/cpp/tests/TlsContext_test.cpp \
    -L deps/boringssl/lib/Darwin/arm64/Debug -lssl -lcrypto -o /tmp/tlscontext_test
```

Expected: FAIL —— `no such file or directory: src/cpp/TlsContext.cpp`

- [ ] **Step 4: 写 TlsContext.h**

Create `src/cpp/TlsContext.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : TlsContext.h
// author : anto
//
// SSL_CTX 工厂与证书校验。校验逻辑从 SMPConnector.cpp:3441-3536 抽出，改写
// 成不依赖 xquic 回调签名的独立函数，供 QUIC 侧与 TCP 侧（HTTP/WS）共用。
//
// 与 SMPConnector 的唯一行为差异：caCertsPem 为空时，SMP 是"跳过校验"，
// 本模块是"回落系统信任库"。理由是 https/wss 常用于访问公网 API，默认不
// 校验存在中间人风险；SMP 连的是自家 SFU，行为保持不变。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_TLS_CONTEXT_H
#define TT_TLS_CONTEXT_H

#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <BC/Config.h>

struct TlsConfig
{
    std::string caCertsPem;                 // 空 → 回落系统信任库
    std::string spkiPin;                    // base64 SPKI pin，可选
    std::string clientCertFile;             // 双向 TLS，可选
    std::string clientKeyFile;
    std::string clientKeyPassword;
    bool        insecureSkipVerify = false; // 仅供自签调试
    void*       loggerCtx          = nullptr;
};

class TlsContext
{
public:
    // 建 SSL_CTX。返回值由调用方 SSL_CTX_free。失败返回 nullptr 并填 outErr。
    //
    // 固定项：SSL_CTX_set_min_proto_version(TLS1_2_VERSION)、SSL_VERIFY_PEER
    // （insecureSkipVerify 时为 SSL_VERIFY_NONE）、SSL_OP_NO_COMPRESSION。
    //
    // caCertsPem 为空时依次尝试：
    //   1. SSL_CTX_set_default_verify_paths()
    //   2. Android 额外加 /system/etc/security/cacerts（目录形式）
    //   3. 两者都没拿到 CA 时只打 _WARN_，不失败 —— "绝不静默地不校验"由
    //      SSL_VERIFY_PEER 保证（空 store 下握手必然失败），这里再加硬失败
    //      守卫是多余的且有假阴性风险（X509_STORE 在部分平台惰性加载）。
    //
    // 只有畸形的 caCertsPem 会让本函数失败 —— 那是调用方明确传错了东西。
    static SSL_CTX* Create(const TlsConfig& cfg, std::string& outErr);

    // 每条连接调用一次：设 SNI + hostname 校验期望值。
    // host 是 IP 字面量（v4 或 v6）时不设 SNI —— RFC 6066 禁止。
    static BCRESULT PrepareSsl(SSL* ssl,
                               const std::string& host,
                               const TlsConfig& cfg,
                               std::string& outErr);

    // 校验证书链。chain[0] 是 leaf。
    // spkiPin 非空时只比对 pin，不做链校验（与 SMPConnector 现有语义一致）。
    static BCRESULT VerifyChain(const std::vector<X509*>& chain,
                                const std::string& host,
                                const TlsConfig& cfg,
                                std::string& outErr);

    // 从 SMPConnector.cpp 移过来的 helper，SMPConnector 改为引用这里。
    static std::string ComputeSpkiPinBase64(X509* leaf);
    static std::string CanonicalizeSpkiPin(const char* pin, size_t len);
    static int         AddTrustedCAsToStore(X509_STORE* store,
                                            const std::vector<X509*>& certs,
                                            void* loggerCtx);
    // 把 PEM 文本解析成一组 X509*。调用方负责 X509_free。
    static int         ParseCAsFromPem(const std::string& pem,
                                       std::vector<X509*>& out,
                                       std::string& outErr);
};

#endif // TT_TLS_CONTEXT_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 5: 写 TlsContext.cpp**

实现要点，逐条对应：

1. `ComputeSpkiPinBase64` / `CanonicalizeSpkiPin` / `AddTrustedCAsToStore` —— 从 `SMPConnector.cpp:131-160` 及其附近整体移过来，签名改为静态成员函数，`LogQ(logger_ctx, ...)` 的 ctx 改为参数传入。
2. `VerifyChain` —— 从 `SMPConnector.cpp:3441-3536` 移过来。保留那段失败时打印完整证书链 subject/issuer 与本地 CA 列表的诊断日志（`3498-3528`），它对现场排查很关键。把 `user_conn->cert_verify_error_` 的赋值改为写 `outErr`，`return -1/0` 改为 `BC_R_TLS_VERIFY_FAILED` / `BC_R_SUCCESS`。
3. `Create` —— 新写。顺序：`SSL_CTX_new(TLS_client_method())` → `SSL_CTX_set_options(SSL_OP_NO_COMPRESSION | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION)` → `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)` → CA 加载 → 客户端证书加载 → `SSL_CTX_set_verify`。

   CA 加载分支：

```cpp
    bool haveCa = false;
    if (!cfg.caCertsPem.empty()) {
        std::vector<X509*> cas;
        if (TlsContext::ParseCAsFromPem(cfg.caCertsPem, cas, outErr) != 0) {
            SSL_CTX_free(ctx);
            return nullptr;   // 畸形 PEM 必须响亮失败，不能静默降级
        }
        X509_STORE* store = SSL_CTX_get_cert_store(ctx);
        haveCa = (TlsContext::AddTrustedCAsToStore(store, cas, cfg.loggerCtx) == 0
                  && !cas.empty());
        for (X509* c : cas) X509_free(c);
    } else {
        haveCa = (SSL_CTX_set_default_verify_paths(ctx) == 1);
#ifdef OS_ANDROID
        if (SSL_CTX_load_verify_locations(
                ctx, nullptr, "/system/etc/security/cacerts") == 1) {
            haveCa = true;
        }
#endif
    }

    // 拿不到 CA 时只告警，不让 Create() 失败。
    //
    // "绝不静默地不校验"这个保证来自下面的 SSL_VERIFY_PEER —— 空 store 下
    // 握手必然以 "unable to get local issuer certificate" 失败，不存在静默
    // 放行。所以这里再加一道硬失败守卫是多余的，而且有假阴性风险：
    // BoringSSL 的 X509_STORE 在部分平台是惰性加载，判"空"会把本来能连的
    // https 全部拒掉。
    //
    // 实测（macOS 15 / BoringSSL）：set_default_verify_paths 返回 1，store
    // 里实际加载了 128 个 CA，来源 /etc/ssl/cert.pem。
    if (!haveCa && !cfg.insecureSkipVerify && cfg.spkiPin.empty()) {
        LogQ(cfg.loggerCtx, _WARN_,
             "TLS: 未提供 caCerts 且系统信任库看起来是空的。若握手报 "
             "unable to get local issuer certificate，请显式传入 caCerts。");
    }

    SSL_CTX_set_verify(ctx,
        cfg.insecureSkipVerify ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);
```

4. **客户端证书加载 —— 顺手修掉 `WSConnector.cpp:558-566` 的两个 bug**：

```cpp
    // 原 WSConnector.cpp:558-561 的条件里 certificate_file 判了两次，
    // 第二次本意是 private_key_file；结果 private_key_file 为 NULL 时
    // strlen() 会解引用空指针。
    if (!cfg.clientCertFile.empty() && !cfg.clientKeyFile.empty()) {
        // 原 WSConnector.cpp:562 写的是 if (!config_.private_key_password)，
        // 判反了 —— 只在密码为空时才去设密码回调。这里改为非空时才设。
        if (!cfg.clientKeyPassword.empty()) {
            SSL_CTX_set_default_passwd_cb_userdata(
                ctx, const_cast<char*>(cfg.clientKeyPassword.c_str()));
        }
        if (SSL_CTX_use_certificate_file(
                ctx, cfg.clientCertFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(
                ctx, cfg.clientKeyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            outErr = "加载客户端证书/私钥失败: " + cfg.clientCertFile;
            SSL_CTX_free(ctx);
            return nullptr;
        }
    }
```

5. `PrepareSsl` —— 判断 host 是否为 IP 字面量（`inet_pton(AF_INET, ...)` 或 `inet_pton(AF_INET6, ...)` 成功即是），是则跳过 SNI；否则 `SSL_set_tlsext_host_name(ssl, host.c_str())`。两种情况都调 `SSL_set1_host(ssl, host.c_str())` 让 BoringSSL 做主机名校验（`insecureSkipVerify` 时跳过）。

- [ ] **Step 6: 运行测试确认通过**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/boringssl/src/include \
    src/cpp/TlsContext.cpp src/cpp/tests/TlsContext_test.cpp \
    -L deps/boringssl/lib/Darwin/arm64/Debug -lssl -lcrypto -o /tmp/tlscontext_test \
    && /tmp/tlscontext_test
```

Expected: `TlsContext_test: ALL PASS`

- [ ] **Step 7: 改 SMPConnector 引用新模块**

在 `src/cpp/SMPConnector.cpp` include 区加 `#include "TlsContext.h"`。

删除 `SMPConnector.cpp:131-160` 的 `addTrustedCAsToStore` 定义及同区域的 `computeSpkiPinBase64` / `canonicalizeSpkiPin` 定义，把调用点改为 `TlsContext::AddTrustedCAsToStore(...)` / `TlsContext::ComputeSpkiPinBase64(...)` / `TlsContext::CanonicalizeSpkiPin(...)`。

`SMPConnector.cpp:3441-3536` 的校验主体改为组装 `std::vector<X509*> chain` 后调 `TlsContext::VerifyChain`，把 `outErr` 写回 `user_conn->cert_verify_error_`。

**注意保持 SMP 侧语义不变**：`trusted_cas->empty()` 时 `return 0`（跳过校验）。所以 SMP 侧调用 `VerifyChain` 前要先自行判空并短路，不要走进 `TlsContext` 的"回落系统信任库"分支。

- [ ] **Step 8: 验证 QUIC 侧证书校验未回归**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON
make ttsignal -j8 2>&1 | tail -3
```

Expected: `[100%] Built target ttsignal`

再跑一次 Task 2 Step 7 的 QUIC 冒烟，确认连接仍成功。

- [ ] **Step 9: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/TTErrors.h src/cpp/TlsContext.h src/cpp/TlsContext.cpp \
        src/cpp/tests/TlsContext_test.cpp src/cpp/SMPConnector.cpp
git commit -m "refactor(tls): 抽出 TlsContext，QUIC 与 TCP 共用证书校验

把 SMPConnector 的证书链校验、SPKI pin、CA store 构建抽成独立模块，
避免 TCP 侧再写一份导致行为漂移。新增 TTErrors.h 集中错误码。

顺带修掉两个从 jmp 带过来的 bug：
- private_key_password 判空写反，只在密码为空时才设密码回调
- 客户端证书条件里 certificate_file 判了两次，private_key_file 为
  NULL 时 strlen 会解引用空指针

最低协议版本从 TLS 1.1 提到 TLS 1.2。caCerts 为空时回落系统信任库
而非跳过校验——这是与 SMP 唯一有意的行为差异，见 spec。"
```

---

### Task 4: DnsResolver

绕过 VPN 的 DNS 解析。这是整个改造里最容易写错的部分——DNS 报文的 name compression 指针必须正确处理，且必须防御成环与越界。

**Files:**
- Create: `src/cpp/DnsResolver.h`
- Create: `src/cpp/DnsResolver.cpp`
- Create: `src/cpp/tests/DnsResolver_test.cpp`

**Interfaces:**
- Consumes: `tt_socket_pin`（Task 2）、`BC_R_DNS_FAILED`（Task 3 的 `TTErrors.h`）、`TTVpnPolicy`
- Produces:
  - `struct DnsConfig { TTVpnPolicy policy; uint32_t ifIndex; std::vector<std::string> servers; uint32_t timeoutMs; bool preferIpv6; void* loggerCtx; }`
  - `struct DnsResult { BCRESULT result; std::vector<BCSockAddrS> addrs; std::string errMessage; }`
  - `DnsResult DnsResolver::Resolve(const std::string& host, const DnsConfig&)`
  - `void DnsResolver::ClearCache()`
  - `std::vector<uint8_t> DnsMessage::BuildQuery(const std::string& host, uint16_t qtype, uint16_t txid)`
  - `bool DnsMessage::ParseResponse(const uint8_t* data, size_t len, uint16_t expectTxid, std::vector<BCSockAddrS>& outAddrs, uint32_t& outMinTtl, std::string& outErr)`

- [ ] **Step 1: 写失败的测试**

Create `src/cpp/tests/DnsResolver_test.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : DnsResolver_test.cpp
//
// Standalone unit test for DNS message encode/decode. Not part of any build
// target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
//       src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       deps/env/src/BC/BCSockAddr.cpp \
//       src/cpp/tests/DnsResolver_test.cpp \
//       -o /tmp/dnsresolver_test && /tmp/dnsresolver_test
//
// 只测报文编解码，不发真实网络请求。
///////////////////////////////////////////////////////////////////////////////
#include "DnsResolver.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// example.com 的标准 A 应答，answer 的 NAME 用压缩指针 0xC00C 指回 question。
static const uint8_t kResponseExampleCom[] = {
    // Header: ID=0x1234, Flags=0x8180 (QR|RD|RA), QD=1, AN=1, NS=0, AR=0
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    // Question: 7"example" 3"com" 0, QTYPE=A(1), QCLASS=IN(1)
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
    0x00, 0x01, 0x00, 0x01,
    // Answer: NAME=ptr->12, TYPE=A, CLASS=IN, TTL=300, RDLEN=4, RDATA=93.184.216.34
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
    0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22,
};

static void test_build_query_encodes_labels()
{
    std::vector<uint8_t> q = DnsMessage::BuildQuery("example.com", 1, 0x1234);

    // 12 字节 header + 13 字节 QNAME + 4 字节 QTYPE/QCLASS
    CHECK(q.size() == 29);
    CHECK(q[0] == 0x12 && q[1] == 0x34);        // txid
    CHECK(q[2] == 0x01 && q[3] == 0x00);        // flags: RD=1
    CHECK(q[4] == 0x00 && q[5] == 0x01);        // QDCOUNT=1
    CHECK(q[6] == 0x00 && q[7] == 0x00);        // ANCOUNT=0
    CHECK(q[12] == 0x07);                       // label len "example"
    CHECK(memcmp(&q[13], "example", 7) == 0);
    CHECK(q[20] == 0x03);                       // label len "com"
    CHECK(memcmp(&q[21], "com", 3) == 0);
    CHECK(q[24] == 0x00);                       // root label
    CHECK(q[25] == 0x00 && q[26] == 0x01);      // QTYPE=A
    CHECK(q[27] == 0x00 && q[28] == 0x01);      // QCLASS=IN
}

// 超长 label（>63）和超长域名（>253）必须被拒绝，返回空。
static void test_build_query_rejects_oversized_names()
{
    std::string longLabel(64, 'a');
    CHECK(DnsMessage::BuildQuery(longLabel + ".com", 1, 1).empty());

    std::string longName;
    for (int i = 0; i < 40; i++) longName += "abcdef.";
    longName += "com";
    CHECK(longName.size() > 253);
    CHECK(DnsMessage::BuildQuery(longName, 1, 1).empty());

    CHECK(DnsMessage::BuildQuery("", 1, 1).empty());
}

static void test_parse_response_extracts_a_record()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(kResponseExampleCom,
                                    sizeof(kResponseExampleCom),
                                    0x1234, addrs, ttl, err));
    CHECK(err.empty());
    CHECK(addrs.size() == 1);
    CHECK(ttl == 300);

    char text[64] = {0};
    bc_sockaddr_format(&addrs[0], text, sizeof(text));
    CHECK(strstr(text, "93.184.216.34") != nullptr);
}

// txid 不匹配必须拒绝 —— 防 off-path 伪造应答。
static void test_parse_response_rejects_txid_mismatch()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom,
                                     sizeof(kResponseExampleCom),
                                     0x9999, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}

// 压缩指针成环必须被检出，不能死循环。
static void test_parse_response_detects_pointer_loop()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    // answer 的 NAME 在偏移 29；改成指向它自己，构造自环
    msg[29] = 0xC0;
    msg[30] = 0x1D;   // 0x1D == 29

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}

// 越界的压缩指针必须被检出。
static void test_parse_response_detects_out_of_bounds_pointer()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[29] = 0xC0;
    msg[30] = 0xFF;   // 偏移 255 > 报文长度 45

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}

// 截断的报文（不足 header 长度）必须被拒绝，不能越界读。
static void test_parse_response_rejects_truncated_message()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom, 5,
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());

    // RDLENGTH 声称 4 字节但报文在此截断
    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom,
                                     sizeof(kResponseExampleCom) - 2,
                                     0x1234, addrs, ttl, err));
}

// RCODE 非 0（如 NXDOMAIN=3）必须失败并在 errMessage 里说明。
static void test_parse_response_reports_rcode()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[3] = 0x83;   // RCODE = 3 (NXDOMAIN)

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(err.find("3") != std::string::npos);
}

// 非 A/AAAA 的 answer（如 CNAME）必须被跳过而不是当成地址。
static void test_parse_response_skips_non_address_records()
{
    // Header: AN=2。第一条 CNAME，第二条 A。
    std::vector<uint8_t> msg = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        // Answer 1: CNAME(5)，RDATA 是 3"www" + ptr->12
        0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
        0x00, 0x06, 0x03, 'w', 'w', 'w', 0xC0, 0x0C,
        // Answer 2: A，TTL=60
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 0x01, 0x02, 0x03, 0x04,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(msg.data(), msg.size(),
                                    0x1234, addrs, ttl, err));
    CHECK(addrs.size() == 1);       // 只认 A，CNAME 被跳过
    CHECK(ttl == 60);               // 取所有记录 TTL 的最小值
}

// RDLENGTH 与记录类型不符（A 记录声称 16 字节）必须被跳过，不能当成地址。
static void test_parse_response_rejects_bad_rdlength()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[39] = 0x00;
    msg[40] = 0x10;   // RDLENGTH 改成 16，与 A 记录的 4 不符

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    DnsMessage::ParseResponse(msg.data(), msg.size(), 0x1234, addrs, ttl, err);
    CHECK(addrs.empty());
}

int main()
{
    test_build_query_encodes_labels();
    test_build_query_rejects_oversized_names();
    test_parse_response_extracts_a_record();
    test_parse_response_rejects_txid_mismatch();
    test_parse_response_detects_pointer_loop();
    test_parse_response_detects_out_of_bounds_pointer();
    test_parse_response_rejects_truncated_message();
    test_parse_response_reports_rcode();
    test_parse_response_skips_non_address_records();
    test_parse_response_rejects_bad_rdlength();

    if (g_failures == 0) {
        printf("DnsResolver_test: ALL PASS\n");
    } else {
        printf("DnsResolver_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
    src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
    deps/env/src/BC/BCSockAddr.cpp \
    src/cpp/tests/DnsResolver_test.cpp -o /tmp/dnsresolver_test
```

Expected: FAIL —— `no such file or directory: src/cpp/DnsResolver.cpp`

- [ ] **Step 3: 写 DnsResolver.h**

Create `src/cpp/DnsResolver.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : DnsResolver.h
// author : anto
//
// 绕过 VPN 的 DNS 解析。
//
// 为什么不能只用 getaddrinfo：在 Clash / Surge / mihomo 的 TUN 全局模式下，
// getaddrinfo 走系统 DNS，请求被 VPN 劫持；fake-IP 模式会返回 198.18.0.0/15
// 段的假地址。此时即使把 TCP socket 硬绑到物理网卡，目标 IP 本身是假的，
// 物理网卡上根本没有到 198.18.x.x 的路由，连接直接黑洞。
//
// 所以"用真实 IP 出网"必须从 DNS 开始就绕过 VPN，只绑 TCP 是不够的。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_DNS_RESOLVER_H
#define TT_DNS_RESOLVER_H

#include <string>
#include <vector>

#include <BC/BCSocket.h>
#include <BC/Config.h>

#include "VpnPolicy.h"

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// class : DnsMessage —— 纯函数，无 IO，可单测
///////////////////////////////////////////////////////////////////////////////

class DnsMessage
{
public:
    // 构造一个标准递归查询。qtype: 1=A, 28=AAAA。
    // host 非法（空、label > 63 字节、总长 > 253 字节）时返回空 vector。
    static std::vector<uint8_t> BuildQuery(const std::string& host,
                                           uint16_t qtype,
                                           uint16_t txid);

    // 解析应答，抽出 A/AAAA 地址。
    //
    // 返回 false 的情形：报文短于 header、txid 不匹配、RCODE 非 0、
    // 压缩指针越界或成环、answer section 越界。outErr 说明具体原因。
    //
    // outMinTtl 取所有被采纳记录 TTL 的最小值。没有地址记录时不修改它。
    // 非 A/AAAA 的记录（CNAME 等）被跳过，不算失败。
    // RDLENGTH 与记录类型不符的记录被跳过。
    static bool ParseResponse(const uint8_t* data, size_t len,
                              uint16_t expectTxid,
                              std::vector<BCSockAddrS>& outAddrs,
                              uint32_t& outMinTtl,
                              std::string& outErr);

private:
    // 跳过一个（可能被压缩的）域名，返回它在报文中占用的字节数。
    // 失败（越界 / 成环）返回 0。maxJumps 限制指针跳转次数，防成环。
    static size_t _SkipName(const uint8_t* data, size_t len, size_t offset);
};

///////////////////////////////////////////////////////////////////////////////
// struct : DnsConfig / DnsResult
///////////////////////////////////////////////////////////////////////////////

struct DnsConfig
{
    TTVpnPolicy              policy    = TT_VPN_POLICY_UNSET;
    uint32_t                 ifIndex   = 0;       // policy == OS 时忽略
    std::vector<std::string> servers;             // 空则用默认公共 DNS
    uint32_t                 timeoutMs = 2000;    // 单台 server
    bool                     preferIpv6 = false;
    void*                    loggerCtx = nullptr;
};

struct DnsResult
{
    BCRESULT                 result = BC_R_FAILURE;
    std::vector<BCSockAddrS> addrs;               // 按 preferIpv6 排序
    std::string              errMessage;
};

///////////////////////////////////////////////////////////////////////////////
// class : DnsResolver
///////////////////////////////////////////////////////////////////////////////

class DnsResolver
{
public:
    // ⚠️ 同步阻塞调用，必须在 BCTaskMgr 线程池里执行，不能在事件循环线程调用。
    //
    // policy == OS              : 直接 getaddrinfo
    // policy == PREFER_PHYSICAL : 先试自建查询；绑定失败或全部 server 超时
    //                             则回落 getaddrinfo 并打 _WARN_
    // policy == FORCE_PHYSICAL  : 只走自建查询，不回落。失败返回 BC_R_DNS_FAILED
    //
    // host 本身是 IP 字面量时直接返回它，不查询。
    static DnsResult Resolve(const std::string& host, const DnsConfig& cfg);

    // 切网时调用（INetworkPathMonitor 回调里），清空缓存。
    static void ClearCache();

    // 默认公共 DNS，DnsConfig::servers 为空时使用。
    static const char* kDefaultServers[2];   // {"223.5.5.5", "8.8.8.8"}
};

#endif // TT_DNS_RESOLVER_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 4: 写 DnsResolver.cpp**

实现要点：

**`BuildQuery`** —— 12 字节 header（txid 大端、flags `0x0100` 即 RD=1、QDCOUNT=1、其余 0），QNAME 按 `.` 切成 label 逐段写 `len + bytes`，末尾写 `0x00`，再写 QTYPE/QCLASS 各 2 字节大端。校验：host 非空、每个 label 长度在 `[1, 63]`、编码后 QNAME 总长 ≤ 255。

**`_SkipName`** —— 这是最容易写错的地方，必须严格：

```cpp
size_t DnsMessage::_SkipName(const uint8_t* data, size_t len, size_t offset)
{
    // 返回该名字在"当前位置"占用的字节数（遇到指针即结束，指针本身占 2 字节）。
    // 越界或 label 长度非法返回 0。
    size_t consumed = 0;
    size_t pos      = offset;

    while (pos < len) {
        uint8_t b = data[pos];

        if ((b & 0xC0) == 0xC0) {
            // 压缩指针，占 2 字节且一定是名字的结尾
            if (pos + 1 >= len) return 0;
            return consumed + 2;
        }
        if ((b & 0xC0) != 0x00) {
            return 0;   // 0x40 / 0x80 是保留的 label 类型，视为畸形
        }
        if (b == 0) {
            return consumed + 1;   // root label，名字结束
        }
        if (pos + 1 + b > len) return 0;   // label 越界
        pos      += 1 + b;
        consumed += 1 + b;
    }
    return 0;
}
```

注意 `_SkipName` **只跳过不解析**——我们不需要 answer 的 NAME 内容，只需要正确越过它拿到后面的 TYPE/CLASS/TTL/RDLENGTH。这样就不必真的跟随指针，成环风险天然消失。但**指针目标的合法性仍要校验**，否则畸形报文能骗过我们：在遇到指针时额外检查目标偏移是否 `< len`，不满足返回 0。把这个检查加进上面的指针分支：

```cpp
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return 0;
            size_t target = ((size_t)(b & 0x3F) << 8) | data[pos + 1];
            if (target >= len) return 0;      // 越界指针
            if (target == pos)  return 0;      // 自环
            return consumed + 2;
        }
```

**`ParseResponse`** —— 顺序：

1. `len < 12` → 失败，`outErr = "DNS 应答短于 12 字节 header"`
2. txid 比对，不等 → 失败（防 off-path 伪造）
3. `rcode = data[3] & 0x0F`，非 0 → 失败，`outErr` 里带上 rcode 数值
4. 读 QDCOUNT / ANCOUNT，逐个 `_SkipName` 跳过 question（每个 question 另占 4 字节 QTYPE/QCLASS）
5. 遍历 answer：`_SkipName` → 读 TYPE(2)/CLASS(2)/TTL(4)/RDLENGTH(2)，任一步越界即失败 → `TYPE==1 && RDLENGTH==4` 时按 IPv4 取地址，`TYPE==28 && RDLENGTH==16` 时按 IPv6 取，其余跳过 → 采纳地址时用 `bc_sockaddr_fromin` / `bc_sockaddr_fromin6` 构造 `BCSockAddrS`（端口先填 0，由调用方设置），并更新 `outMinTtl`
6. `TC` 位（`data[2] & 0x02`）置位时不做 TCP fallback，按"结果不完整"处理：已拿到地址就返回成功，没拿到就失败并在 `outErr` 说明被截断

**`Resolve`** —— 策略分支：

```cpp
    // host 本身是 IP 字面量：不查询，直接返回
    // policy 为 UNSET 时用 tt_vpn_policy_platform_default()

    if (policy == TT_VPN_POLICY_OS) {
        return _ResolveViaSystem(host, cfg);
    }

    DnsResult r = _ResolveViaUdp(host, cfg);
    if (r.result == BC_R_SUCCESS) return r;

    if (policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
        r.result = BC_R_DNS_FAILED;
        return r;    // 不回落
    }

    LogQ(cfg.loggerCtx, _WARN_,
        "DNS: prefer-physical 自建查询失败（%s），回落系统 getaddrinfo。"
        "TUN 全局代理下解析结果可能是 fake-IP。", r.errMessage.c_str());
    return _ResolveViaSystem(host, cfg);
```

**`_ResolveViaUdp`** —— 对每台 server 串行：`socket(AF_INET, SOCK_DGRAM, 0)` → `tt_socket_pin(&pin, ...)`（`isTcp = 0`，`policy` 透传；`force-physical` 下返回非 `TT_PIN_OK` 即整体失败）→ 同时 `sendto` A 和 AAAA 两个查询（不同 txid）→ `poll(POLLIN, timeoutMs)` 收包直到两个都收到或超时 → `ParseResponse` → `close(fd)`。任一 server 拿到地址即返回；全部失败时 `errMessage` 汇总每台 server 的失败原因。

**缓存** —— 进程内 `std::map<std::string, Entry>`，key 必须是 `host + "|" + policy + "|" + ifIndex`：

```cpp
    // key 里必须含 policy 和 ifIndex：同一域名在物理网卡和 VPN 下解析结果
    // 不同，混用会导致极难排查的串味问题。
    std::string key = host + "|" + std::to_string((int)policy)
                    + "|" + std::to_string(cfg.ifIndex);
```

TTL 取 `outMinTtl` 并夹在 `[10, 600]` 秒。加 `BCSpinMutex` 保护（`Resolve` 会被多个 task 线程并发调用）。

- [ ] **Step 5: 运行测试确认通过**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
    src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
    deps/env/src/BC/BCSockAddr.cpp \
    src/cpp/tests/DnsResolver_test.cpp -o /tmp/dnsresolver_test \
    && /tmp/dnsresolver_test
```

Expected: `DnsResolver_test: ALL PASS`

- [ ] **Step 6: 手工验证真实解析**

```bash
cd /Users/antonio/chative/ttsignal
cat > /tmp/dnsmain.cpp <<'EOF'
#include "DnsResolver.h"
#include <cstdio>
int main(int argc, char** argv) {
    DnsConfig cfg;
    cfg.policy = TT_VPN_POLICY_PREFER_PHYSICAL;
    DnsResult r = DnsResolver::Resolve(argc > 1 ? argv[1] : "example.com", cfg);
    printf("result=%u addrs=%zu err=%s\n",
           r.result, r.addrs.size(), r.errMessage.c_str());
    for (auto& a : r.addrs) {
        char t[64] = {0};
        bc_sockaddr_format(&a, t, sizeof(t));
        printf("  %s\n", t);
    }
    return r.addrs.empty();
}
EOF
c++ -std=c++17 -I src/cpp -I deps/env/src src/cpp/VpnPolicy.cpp \
    src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp /tmp/dnsmain.cpp \
    -o /tmp/dnsmain && /tmp/dnsmain example.com
```

Expected: `result=0 addrs>=1`，并列出至少一个 IPv4 地址。

- [ ] **Step 7: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/DnsResolver.h src/cpp/DnsResolver.cpp \
        src/cpp/tests/DnsResolver_test.cpp
git commit -m "feat(dns): 新增绕过 VPN 的 UDP DNS 解析

TUN 全局代理下 getaddrinfo 被劫持，fake-IP 模式返回 198.18.x.x 假地址，
此时只绑 TCP 不够——目标 IP 本身是假的。所以自建 UDP DNS 客户端，
socket 过 SocketPinner 绑物理网卡后直接打公共 DNS。

三档策略：os 走 getaddrinfo；prefer-physical 失败回落；force-physical
不回落。缓存 key 含 policy 与 ifIndex，避免物理网卡与 VPN 的结果串味。

报文解析严格校验压缩指针的越界与自环、RDLENGTH 与记录类型是否相符、
txid 是否匹配（防 off-path 伪造）。"
```

---

### Task 5: TcpChannel

把「DNS → 建 socket → 绑网卡 → 路由复核 → connect → TLS 握手 → 读写」串成一个状态机，`WSConnection` 与 `HttpConnection` 共用。

**Files:**
- Create: `src/cpp/TcpChannel.h`
- Create: `src/cpp/TcpChannel.cpp`

**Interfaces:**
- Consumes: `DnsResolver::Resolve`（Task 4）、`tt_socket_pin`（Task 2）、`TlsContext::Create/PrepareSsl`（Task 3）、`tt_route_lookup_ifindex`（`src/cpp/NetworkRouteLookup.h:66`）、`SSLayer`（`src/cpp/SSLayer.h`）、`TTErrors.h`
- Produces:
  - `class ITcpChannelHandler { virtual void OnChannelReady()=0; virtual void OnChannelData(const void*, size_t)=0; virtual void OnChannelClosed(BCRESULT, const std::string&)=0; }`
  - `struct TcpChannelConfig { std::string host; uint16_t port; bool tls; std::string resolvedIp; TTVpnPolicy policy; DnsConfig dns; TlsConfig tls_cfg; uint32_t connectTimeoutMs; void* loggerCtx; }`
  - `class TcpChannel { BCRESULT Open(const TcpChannelConfig&, ITcpChannelHandler*); BCRESULT Send(BufferPtr); void Close(); std::string PeerIp() const; uint32_t BoundIfIndex() const; std::string PinMethod() const; }`

- [ ] **Step 1: 写 TcpChannel.h**

Create `src/cpp/TcpChannel.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : TcpChannel.h
// author : anto
//
// TCP + TLS 连接底座。把 DNS → socket → 绑网卡 → 路由复核 → connect →
// TLS 握手 → 读写 串成一个状态机，WSConnection 与 HttpConnection 共用。
//
// 上层协议实现（WS 帧 / HTTP 报文）不碰任何 socket 代码。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_TCP_CHANNEL_H
#define TT_TCP_CHANNEL_H

#include <memory>
#include <list>
#include <string>

#include <BC/BCSocket.h>
#include <BC/BCEventQueue.h>
#include <BC/BCStream.h>

#include "DnsResolver.h"
#include "SSLayer.h"
#include "TlsContext.h"
#include "VpnPolicy.h"

using namespace BC;

class ITcpChannelHandler
{
public:
    ITcpChannelHandler() {}
    virtual ~ITcpChannelHandler() {}

    // TCP 已连上且（若 tls）TLS 握手已完成，可以开始收发
    virtual void OnChannelReady()                                = 0;
    // 收到明文数据（TLS 已解密）
    virtual void OnChannelData(const void* data, size_t size)    = 0;
    // 连接结束。result == BC_R_SUCCESS 表示对端正常关闭
    virtual void OnChannelClosed(BCRESULT result,
                                 const std::string& reason)      = 0;
};

struct TcpChannelConfig
{
    std::string host;                   // 域名或 IP 字面量
    uint16_t    port             = 0;
    bool        tls              = false;
    std::string resolvedIp;             // 非空则跳过 DNS
    TTVpnPolicy policy           = TT_VPN_POLICY_UNSET;
    DnsConfig   dns;
    TlsConfig   tls_cfg;
    uint32_t    connectTimeoutMs = 10000;
    void*       loggerCtx        = nullptr;
};

class TcpChannel : public BCEventQueue
{
public:
    typedef enum {
        TCPCH_IDLE           = 0,
        TCPCH_RESOLVING      = 1,
        TCPCH_PINNING        = 2,
        TCPCH_CONNECTING     = 3,
        TCPCH_TLS_HANDSHAKE  = 4,
        TCPCH_READY          = 5,
        TCPCH_CLOSING        = 6,
        TCPCH_CLOSED         = 7,
    } State;

    TcpChannel();
    ~TcpChannel() override;

    BCRESULT    Open(const TcpChannelConfig& cfg, ITcpChannelHandler* handler);
    BCRESULT    Send(BufferPtr buf);
    void        Close();

    // 诊断信息，业务据此判断是否真的走了物理网卡
    std::string PeerIp()       const { return peer_ip_; }
    uint32_t    BoundIfIndex() const { return bound_ifindex_; }
    std::string PinMethod()    const { return pin_method_; }
    State       GetState()     const { return state_; }

private:
    DECLARE_NO_COPY_CLASS(TcpChannel);
    // ... 成员见 Step 2 ...
};

#endif // TT_TCP_CHANNEL_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 2: 写 TcpChannel.cpp**

状态机流转：

```
IDLE → RESOLVING → PINNING → CONNECTING → TLS_HANDSHAKE → READY → CLOSING → CLOSED
                                              ↑ tls == false 时跳过
```

**线程约定**：`RESOLVING` 在 `BCTaskMgr` 线程池执行（`DnsResolver::Resolve` 是同步阻塞的），完成后 `PostEvent` 回事件循环；其余状态全在事件循环线程。

`Open` 的实现：

```cpp
BCRESULT TcpChannel::Open(const TcpChannelConfig& cfg, ITcpChannelHandler* handler)
{
    config_  = cfg;
    handler_ = handler;

    if (config_.policy == TT_VPN_POLICY_UNSET) {
        config_.policy = tt_vpn_policy_platform_default();
    }

    // 步骤 1：选网卡
    // policy == OS 时不选，bound_ifindex_ 保持 0，后续全部跳过绑定与复核
    if (config_.policy != TT_VPN_POLICY_OS) {
        bound_ifindex_ = _PickPhysicalIfIndex();
        if (bound_ifindex_ == 0) {
            if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
                _Fail(BC_R_NO_PHYSICAL_INTERFACE,
                      "force-physical：找不到可用的物理网卡（当前 active "
                      "interface 为虚拟网卡或取不到）。业务可改用 "
                      "prefer-physical 重试。");
                return BC_R_NO_PHYSICAL_INTERFACE;
            }
            LogQ(config_.loggerCtx, _WARN_,
                 "prefer-physical：找不到物理网卡，回落系统路由");
        }
    }

    _SetState(TCPCH_RESOLVING);
    // 派到 task 线程做 DNS，完成后 PostEvent 回来
    Runtime::PostTask([this]() { this->_DoResolve(); });
    return BC_R_SUCCESS;
}
```

`_PickPhysicalIfIndex` 从 `INetworkPathMonitor` 取当前 active ifIndex。若本模块拿不到 monitor 实例，改为由调用方通过 `TcpChannelConfig` 传入 `ifIndex`——两种做法二选一，实现时按 `INetworkPathMonitor.h` 实际暴露的接口决定，并在头文件注释里写明选了哪种。

`_DoResolve` 完成后回到事件循环，进入 `PINNING`：

```cpp
    // 步骤 3：路由复核。这是 force-physical 名副其实的关键。
    //
    // 只绑网卡不够：Linux 上 IP_UNICAST_IF 只是软提示，拿不到 CAP_NET_RAW
    // 时 SO_BINDTODEVICE 会失败，包照样进隧道。用内核路由表做 ground truth。
    //
    // tt_route_lookup_ifindex 在 iOS / Android 上是返回 0 的桩
    // （见 NetworkRouteLookup.h:15）。0 表示"不知道"，此时跳过复核而非判失败，
    // 沿用 UDPSender 既有约定。
    if (bound_ifindex_ != 0) {
        uint32_t routeIf = tt_route_lookup_ifindex(
            (const struct sockaddr*)&peer_addr_, peer_addr_len_);
        if (routeIf != 0 && routeIf != bound_ifindex_) {
            if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "force-physical：内核路由表显示到 %s 的包会从 ifIndex=%u "
                    "出去，而我们要求 ifIndex=%u。包会进隧道，服务端拿不到"
                    "真实 IP，拒绝继续。",
                    peer_ip_.c_str(), routeIf, bound_ifindex_);
                _Fail(BC_R_ROUTE_MISMATCH, msg);
                return;
            }
            LogQ(config_.loggerCtx, _WARN_,
                 "prefer-physical：路由复核不一致（内核 ifIndex=%u，"
                 "我们绑的是 %u），继续但服务端可能看到 VPN 出口 IP",
                 routeIf, bound_ifindex_);
        }
    }
```

`PINNING` 里建 socket 并绑定，**必须在 `Connect` 之前**：

```cpp
    socket_ = new BCSocket();
    result  = socket_->Create(Runtime::SocketMgr(),
                              is_ipv6_ ? PF_INET6 : PF_INET, bc_sockettype_tcp);
    if (result != BC_R_SUCCESS) { _Fail(result, "创建 TCP socket 失败"); return; }

    if (bound_ifindex_ != 0) {
        TTPinRequest pin;
        memset(&pin, 0, sizeof(pin));
        pin.fd               = socket_->GetFd();
        pin.ifIndex          = bound_ifindex_;
        pin.androidNetHandle = bound_ifindex_;
        pin.ipv6             = is_ipv6_ ? 1 : 0;
        pin.isTcp            = 1;      // ⚠️ 必须在 connect 之前
        pin.policy           = config_.policy;
        pin.loggerCtx        = config_.loggerCtx;

        char method[32] = "";
        int  pinErrno   = 0;
        TTPinResult pr = tt_socket_pin(&pin, method, sizeof(method), &pinErrno);
        if (pr == TT_PIN_OK) {
            pin_method_ = method;
        } else if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "force-physical：把 socket 绑到 ifIndex=%u 失败（errno=%d）。"
                "Linux 上 SO_BINDTODEVICE 需要 CAP_NET_RAW 或 root。",
                bound_ifindex_, pinErrno);
            _Fail(BC_R_PIN_FAILED, msg);
            return;
        } else {
            LogQ(config_.loggerCtx, _WARN_,
                 "prefer-physical：绑定 ifIndex=%u 失败 errno=%d，不绑继续",
                 bound_ifindex_, pinErrno);
            bound_ifindex_ = 0;
        }
    }
```

`CONNECTING` / `TLS_HANDSHAKE` / `READY` 的收发直接复用 `WSConnector.cpp` 现有的异步回调模式：`_ConnectDoneCallback`（`WSConnector.cpp:617-618` 的 `socket_->Connect(&addr, GetTask(), cb, this)` 写法）、`_RecvDoneCallback`、`_SendDoneCallback`。TLS 夹在中间：收到原始字节先过 `ssl_layer_->ReadFromSSL(buf, size)`，`SSLayer` 解密后经 `OnRecvDataFromSSL` 回调再转给 `handler_->OnChannelData`；`Send` 先过 `ssl_layer_->WriteToSSL(buf)`，`SSLayer` 加密后经 `OnSSLWrite` 回调再落到 socket。这与 `WSConnection` 今天的做法一致（`WSConnector.cpp:669-703, 815-817, 428-430`）。

`connectTimeoutMs` 用 `ScheduleTask` 起定时器（写法见 `WSConnector.cpp:624-630`），超时调 `_Fail(BC_R_CONNECT_TIMEOUT, ...)`。定时器必须在进入 `READY` 或 `CLOSED` 时取消。

- [ ] **Step 3: 验证编译通过**

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -fsyntax-only -I src/cpp -I deps/env/src \
    -I deps/llhttp/include -I deps/boringssl/src/include \
    src/cpp/TcpChannel.cpp
```

Expected: 无输出（语法检查通过）

- [ ] **Step 4: 全量构建验证**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON
make ttsignal -j8 2>&1 | tail -3
```

Expected: `[100%] Built target ttsignal`

（`TcpChannel.cpp` 是 `src/cpp/*.cpp`，会被 GLOB 自动收录——所以必须重跑 `cmake`，不能只 `make`。）

- [ ] **Step 5: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/TcpChannel.h src/cpp/TcpChannel.cpp
git commit -m "feat(net): 新增 TcpChannel，统一 TCP/TLS 连接底座

把 DNS → socket → 绑网卡 → 路由复核 → connect → TLS 握手 → 读写
串成一个状态机，WSConnection 与 HttpConnection 共用，上层协议实现
不碰任何 socket 代码。

网卡绑定严格在 connect 之前完成（TCP 的硬约束）。路由复核用内核
路由表做 ground truth：force-physical 下发现包会进隧道就明确失败，
而不是假装成功让服务端看到 VPN 出口 IP。"
```

---

### Task 6: HttpConnector + httpget 工具

在 `TcpChannel` 上实现 HTTP 请求/响应，并提供端到端验证工具。

**Files:**
- Create: `src/cpp/HttpConnector.h`
- Create: `src/cpp/HttpConnector.cpp`
- Create: `tools/httpget.cpp`
- Create: `tools/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`（加 `BUILD_TOOLS` 开关）

⚠️ **不要动 `src/cpp/Interface.h`。** `IHttpConnectorHandler` / `IHttpRequestHandler`
定义在 `HttpConnector.h` 里（见 Step 2 的头文件代码），与 spec 第八节的 API 表面一致。
`Interface.h` 现有内容全是 SMP/QUIC 系接口，HTTP 是独立子系统，不塞进去。
（Task 7 的 `IWSConnectorHandler` / `IWSConnectionHandler` 才进 `Interface.h`——
jmp 原本就放那里，下轮接绑定层时要对齐。）

**Interfaces:**
- Consumes: `TcpChannel`（Task 5）、`LLHTTPParser`（`src/cpp/WSParser.h:33`）、`TTErrors.h`
- Produces:
  - `struct HttpRequest { std::string method, url, body, resolvedIp; HttpHeaderMap headers; uint32_t timeoutMs; }`
  - `struct HttpResponse { int status; HttpHeaderMap headers; std::string body; std::string peerIp; uint32_t boundIfIndex; std::string pinMethod; }`
  - `class IHttpRequestHandler { virtual void OnHttpResponse(const HttpResponse&)=0; virtual void OnHttpError(BCRESULT, const std::string&)=0; }`
  - `class HttpConnector { BCRESULT Create(BCFObject*, IHttpConnectorHandler*); BCRESULT Request(const HttpRequest&, IHttpRequestHandler*); void Close(); }`

⚠️ `LLHTTPParser` 定义在 `WSParser.h` 里，而 `WSParser.cpp` 在 Task 1 被排除出构建。**本任务必须先把 `LLHTTPParser` 从 `WSParser.h/.cpp` 拆到独立的 `src/cpp/LLHTTPParser.h/.cpp`**，否则 `HttpConnector` 链接时找不到符号。拆出后 `WSParser.h` 改为 `#include "LLHTTPParser.h"`，Task 7 移植时不受影响。

- [ ] **Step 1: 先把 LLHTTPParser 拆出来**

Create `src/cpp/LLHTTPParser.h`，内容为 `WSParser.h:25-55` 的 `HttpHeaderMap`、`GetLowerCaseHeaders`、`LLHTTPParser` 三项，去掉 `namespace WS` 包裹（HTTP 侧也要用，不该困在 WS 命名空间里）。

Create `src/cpp/LLHTTPParser.cpp`，内容为 `WSParser.cpp:40-160` 附近的 llhttp 回调桩与 `LLHTTPParser` 构造/析构/`_Initialize`。

Modify `src/cpp/WSParser.h`：删掉那三项定义，改为顶部 `#include "LLHTTPParser.h"`。
Modify `src/cpp/WSParser.cpp`：删掉已搬走的实现。

验证：

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -fsyntax-only -I src/cpp -I deps/env/src \
    -I deps/llhttp/include -I deps/boringssl/src/include \
    src/cpp/LLHTTPParser.cpp
```

Expected: 无输出

- [ ] **Step 2: 写 HttpConnector.h**

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : HttpConnector.h
// author : anto
//
// 通用 HTTP 客户端。建在 TcpChannel 上，支持 http:// 与 https://。
//
// ⚠️ 本模块不认识任何业务接口。getServiceCallUrl 只是上层业务的一次普通
//    GET —— ttsignal 不拼它的 URL、不解析它的响应体。
//
// 一次请求一条连接，不做连接池复用，不自动跟随重定向（3xx 原样返回，
// Location 在 headers 里）。跟随重定向会让 force-physical 的语义变复杂：
// 重定向目标需要重新解析、重新复核路由，交给业务显式发第二次请求更清晰。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_HTTP_CONNECTOR_H
#define TT_HTTP_CONNECTOR_H

#include <string>
#include <memory>
#include <unordered_map>

#include "LLHTTPParser.h"
#include "TcpChannel.h"
#include "TTErrors.h"

struct HttpRequest
{
    std::string   method    = "GET";
    std::string   url;                    // http:// 或 https://
    HttpHeaderMap headers;
    std::string   body;
    uint32_t      timeoutMs = 10000;
    std::string   resolvedIp;             // 非空则跳过 DNS
};

struct HttpResponse
{
    int           status       = 0;
    HttpHeaderMap headers;
    std::string   body;
    // 以下三项让业务无需翻日志就能判断这次请求有没有真的走物理网卡
    std::string   peerIp;
    uint32_t      boundIfIndex = 0;
    std::string   pinMethod;
};

class IHttpRequestHandler
{
public:
    IHttpRequestHandler() {}
    virtual ~IHttpRequestHandler() {}

    virtual void OnHttpResponse(const HttpResponse& resp)                 = 0;
    virtual void OnHttpError(BCRESULT result, const std::string& message) = 0;
};

class IHttpConnectorHandler
{
public:
    IHttpConnectorHandler() {}
    virtual ~IHttpConnectorHandler() {}

    virtual void OnLog(int level, LPCSTR msg) = 0;
    virtual void OnClosed()                   = 0;
};

class HttpConnector
{
public:
    HttpConnector();
    ~HttpConnector();

    // config 键：vpnPolicy / bypassVpn / dnsServers / dnsTimeoutMs /
    //            caCerts / spkiPin / insecureSkipVerify / maxResponseBytes
    BCRESULT Create(BCFObject* pConfig, IHttpConnectorHandler* handler);
    BCRESULT Request(const HttpRequest& req, IHttpRequestHandler* handler);
    void     Close();
};

#endif // TT_HTTP_CONNECTOR_H
```

- [ ] **Step 3: 写 HttpConnector.cpp**

要点：

1. **配置解析** —— `vpnPolicy` 走 `tt_vpn_policy_from_string` + `tt_vpn_policy_resolve`（含 `bypassVpn` 兼容与 both-given 告警），照抄 `SMPConnector` 里现有的读法。`maxResponseBytes` 默认 `8 * 1024 * 1024`。

2. **URL 解析** —— 用 `HTTPProtocol::ParseAddrFromUrl(url, host, port, netType, location, bSSL)`（`WSConnector.cpp:502` 同款）。**注意它返回的 `strIP` 其实是 host，可能是域名**——这正是 `WSConnector.cpp:512` 直接喂给 `bc_net_pton` 会挂的原因。这里把 host 原样交给 `TcpChannel`，由 `TcpChannel` 走 `DnsResolver`。

3. **请求组装**：

```cpp
    std::string reqText = req.method + " " + location + " HTTP/1.1\r\n";
    reqText += "Host: " + host + "\r\n";
    // Host 头一定用域名而非解析出的 IP，否则虚拟主机会返回错误内容
    if (req.headers.find("User-Agent") == req.headers.end()) {
        reqText += "User-Agent: ttsignal/1.0\r\n";
    }
    if (!req.body.empty()) {
        reqText += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    reqText += "Connection: close\r\n";   // 一次请求一条连接
    for (auto& kv : req.headers) {
        reqText += kv.first + ": " + kv.second + "\r\n";
    }
    reqText += "\r\n";
    reqText += req.body;
```

4. **响应解析** —— `HttpConnection` 继承 `LLHTTPParser` 与 `ITcpChannelHandler`。`OnChannelData` 里把字节喂给 `llhttp_execute`；`http_on_status` 记 status（`llhttp_get_status_code`）、`http_on_header_field/value` 攒 headers、body 在 llhttp 的 `on_body` 回调里追加。`http_on_message_complete` 时组装 `HttpResponse`（带上 `channel_->PeerIp()` / `BoundIfIndex()` / `PinMethod()`）回调 `OnHttpResponse`。

   ⚠️ `LLHTTPParser` 当前没有 `on_body` 虚函数（`WSParser.h:39-44` 只有六个）。**需要在 `LLHTTPParser` 上补一个 `virtual int http_on_body(const char* at, size_t length)`**，默认实现返回 0，并在 `_Initialize` 里挂上 `settings.on_body`。`WSConnection` 不覆盖它，行为不受影响。

5. **响应体上限**：

```cpp
    if (body_.size() + length > max_response_bytes_) {
        _Fail(BC_R_RESPONSE_TOO_LARGE,
              "响应体超过 maxResponseBytes 上限（" +
              std::to_string(max_response_bytes_) + " 字节），已中断");
        return -1;   // 让 llhttp 停止解析
    }
```

6. **超时** —— `req.timeoutMs` 覆盖整个请求（含 DNS + connect + TLS + 收响应），用 `ScheduleTask` 起一个总定时器。

- [ ] **Step 4: 写 httpget 工具**

Create `tools/httpget.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : tools/httpget.cpp
//
// 端到端验证工具。用法：
//
//   httpget --url https://example.com/path \
//           [--method GET] [--header "K: V"]... [--body TEXT] \
//           [--vpn-policy os|prefer-physical|force-physical] \
//           [--dns 223.5.5.5] [--ca /path/ca.pem] [--insecure] \
//           [--timeout-ms 10000] [--log-level debug]
//
// 打印 status / headers / body 前 512 字节，以及 peerIp / boundIfIndex /
// pinMethod —— 后三项用来确认这次请求到底有没有真的走物理网卡。
///////////////////////////////////////////////////////////////////////////////
#include "HttpConnector.h"
#include "Runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

class Printer : public IHttpRequestHandler
{
public:
    int exitCode = 1;
    bool done    = false;

    void OnHttpResponse(const HttpResponse& r) override
    {
        printf("status: %d\n", r.status);
        printf("peerIp: %s\n", r.peerIp.c_str());
        printf("boundIfIndex: %u\n", r.boundIfIndex);
        printf("pinMethod: %s\n",
               r.pinMethod.empty() ? "(未绑定)" : r.pinMethod.c_str());
        printf("--- headers ---\n");
        for (auto& kv : r.headers) {
            printf("%s: %s\n", kv.first.c_str(), kv.second.c_str());
        }
        printf("--- body (%zu bytes, 前 512) ---\n", r.body.size());
        printf("%.512s\n", r.body.c_str());
        exitCode = (r.status >= 200 && r.status < 400) ? 0 : 1;
        done     = true;
    }

    void OnHttpError(BCRESULT result, const std::string& message) override
    {
        fprintf(stderr, "ERROR result=%u: %s\n", result, message.c_str());
        exitCode = 2;
        done     = true;
    }
};

// main：解析 argv → Runtime::Initialize → HttpConnector::Create →
//       Request → 轮询等待 printer.done → 返回 printer.exitCode
```

Create `tools/CMakeLists.txt`，把 `httpget` 链到 ttsignal 的核心对象与 `env` / `ssl` / `crypto` / `llhttp`。在 `src/CMakeLists.txt` 末尾加：

```cmake
option(BUILD_TOOLS "Build command-line verification tools" OFF)
if(BUILD_TOOLS)
    add_subdirectory(${BASE_DIR}/../tools tools.dir)
endif()
```

- [ ] **Step 5: 构建并对公网验证**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug \
      -DBUILD_NODE_ADDON=ON -DBUILD_TOOLS=ON
make httpget -j8 2>&1 | tail -3
./tools.dir/httpget --url https://example.com/ --vpn-policy os
```

Expected: `status: 200`，headers 中含 `content-type`，body 里含 `<title>Example Domain</title>`。

- [ ] **Step 6: 对本地自签 TLS 验证证书校验路径**

```bash
cd /Users/antonio/chative/ttsignal
openssl s_server -accept 8443 -cert certs/localhost.crt \
    -key certs/localhost.key -www &
SRV=$!
sleep 1

# 不给 CA：应当校验失败（系统信任库不认这张自签证书）
/tmp/ttbuild/tools.dir/httpget --url https://localhost:8443/ --vpn-policy os
echo "exit=$?  (期望非 0，且 stderr 提示证书校验失败)"

# 给 CA：应当成功
/tmp/ttbuild/tools.dir/httpget --url https://localhost:8443/ \
    --vpn-policy os --ca certs/localhost.crt
echo "exit=$?  (期望 0)"

kill $SRV
```

Expected: 第一次非 0 且报证书校验失败，第二次为 0 并打印 `status: 200`。这验证了"caCerts 为空时回落系统信任库且真的会拒绝不可信证书"——不能静默放行。

- [ ] **Step 7: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/LLHTTPParser.h src/cpp/LLHTTPParser.cpp \
        src/cpp/WSParser.h src/cpp/WSParser.cpp \
        src/cpp/HttpConnector.h src/cpp/HttpConnector.cpp \
        tools/ src/CMakeLists.txt
git commit -m "feat(http): 新增通用 HTTP 客户端与 httpget 验证工具

建在 TcpChannel 上，支持 http/https，可发任意 method + headers + body。
不认识任何业务接口——getServiceCallUrl 只是上层业务的一次普通 GET。

HttpResponse 带上 peerIp / boundIfIndex / pinMethod，业务无需翻日志
就能判断这次请求有没有真的走物理网卡。

顺带把 LLHTTPParser 从 WSParser 拆成独立文件（HTTP 侧也要用，不该困在
WS 命名空间里），并补一个 http_on_body 虚函数用于收集响应体。

不自动跟随重定向：3xx 原样返回，Location 在 headers 里。跟随会让
force-physical 的语义变复杂（目标需重新解析、重新复核路由）。"
```

---

### Task 7: WSConnector 移植接入 TcpChannel

把 `WSConnector.cpp` / `WSParser.cpp` 从 jmp 的原样拷贝改造成能编译进 ttsignal 的版本，连接部分换成 `TcpChannel`，并从 CMakeLists 的排除列表里放回。

**Files:**
- Modify: `src/cpp/WSConnector.h`（去 `Acceptor.h` / `JMPacket.h`，`JMP`→`SMP`，连接成员换 `TcpChannel`）
- Modify: `src/cpp/WSConnector.cpp`（删掉 DNS/socket/SSL 相关约 300 行，改用 `TcpChannel`）
- Modify: `src/cpp/WSParser.h`, `src/cpp/WSParser.cpp`（`JMPacketPtr`→`SMPacketPtr`）
- Modify: `src/cpp/Interface.h`（新增 `IWSConnectorHandler` / `IWSConnectionHandler`）
- Modify: `src/CMakeLists.txt`（删掉 Task 1 加的三处 `list(REMOVE_ITEM ...)`）

**Interfaces:**
- Consumes: `TcpChannel`（Task 5）、`LLHTTPParser`（Task 6 拆出）、`SMPacket`（`src/cpp/SMPacket.h`）、`TTErrors.h`
- Produces: 可用的 `WSConnector::CreateConnection` / `WSConnection::Connect(url, timeoutMs)`，支持 `ws://` 与 `wss://` 且支持域名

- [ ] **Step 1: 在 Interface.h 补 WS handler 接口**

参照 jmp 的 `~/work/jmp/jmp/src/cpp/Interface.h:225-256`，在 `src/cpp/Interface.h` 的 `IConnectionHandler` 之后加入。签名保持与 jmp 一致（下轮接绑定层时 Java/Swift 侧要对齐），但把 `JMPacket` 换成 `SMPacket`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// class : IWSConnectorHandler
///////////////////////////////////////////////////////////////////////////////

class IWSConnectorHandler
{
public:
	IWSConnectorHandler() {}
	virtual ~IWSConnectorHandler() {}

	virtual void		OnLog(int level, LPCSTR lpszMsg)	= 0;
	virtual void		OnClosed()							= 0;
	virtual void		OnException(BCException &)			= 0;
private:
	DECLARE_NO_COPY_CLASS(IWSConnectorHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IWSConnectionHandler
///////////////////////////////////////////////////////////////////////////////

class IWSConnectionHandler
{
public:
	IWSConnectionHandler(){}
	virtual ~IWSConnectionHandler(){}

	virtual void		OnConnectResult(
							BCRESULT result,
							const HttpHeaderMap &headers)	= 0;
	virtual void		OnRecvText(LPCSTR lpszText)			= 0;
	virtual void		OnRecvData(LPCVOID data, size_t size)= 0;
	virtual void		OnClosed(LPCSTR lpszReason)			= 0;
	virtual void		OnException(BCException &)			= 0;
private:
	DECLARE_NO_COPY_CLASS(IWSConnectionHandler);
};
```

在 `Interface.h` 顶部补 `#include "LLHTTPParser.h"`（`HttpHeaderMap` 来自那里），并在文件里补上 `WSConnection` 的前置声明与 `typedef std::shared_ptr<WSConnection> WSConnPtr;`——jmp 把它放在 `Interface.h:32`，本项目照做。

- [ ] **Step 2: 改 WSParser 去掉 JMP 依赖**

`src/cpp/WSParser.h`：
- 删 `#include "JMPacket.h"`，改为 `#include "SMPacket.h"`
- 删 `using namespace JMP;`，改为 `using namespace SMP;`
- `static BCRESULT PackPacket(JMPacketPtr pkt, bool has_mask = false);` → `SMPacketPtr`
- `LLHTTPParser` / `HttpHeaderMap` / `GetLowerCaseHeaders` 已在 Task 6 搬走，此处应只剩 `#include "LLHTTPParser.h"`

`src/cpp/WSParser.cpp`：对应改 `PackPacket` 的实现签名与内部字段访问。`SMPacket.h` 与 `JMPacket.h` 的字段名若不一致，按 `SMPacket.h` 实际定义调整——不要为了少改动而在 WSParser 里造别名。

验证：

```bash
cd /Users/antonio/chative/ttsignal
c++ -std=c++17 -fsyntax-only -I src/cpp -I deps/env/src \
    -I deps/llhttp/include -I deps/boringssl/src/include src/cpp/WSParser.cpp
```

Expected: 无输出

- [ ] **Step 3: 改 WSConnector 接入 TcpChannel**

`src/cpp/WSConnector.h`：
- 删 `#include "Acceptor.h"`、`#include "JMPacket.h"`
- 加 `#include "TcpChannel.h"`、`#include "SMPacket.h"`、`#include "TTErrors.h"`
- `using namespace JMP;` → `using namespace SMP;`
- `WSConnection` 的基类去掉 `ISSLayerHandler`（TLS 现在由 `TcpChannel` 管），加上 `ITcpChannelHandler`
- 删成员：`socket_`、`recv_buffer_[8192]`、`pending_connect_`、`connect_timer_`、`recv_event_`、`pending_recv_`、`pending_send_`、`ssl_layer_`、`ssl_ctx_`、`ssl_`
- 加成员：`std::unique_ptr<TcpChannel> channel_;`
- 删声明：`OnSSLWrite` / `OnRecvDataFromSSL` / `OnSSLReady` / `OnSSLFinished` / `OnSSLError` / `_OnConnectDone` / `_OnRecvDone` / `_OnSendDone` / `_TCP_Recv` / `_ConnectDoneCallback` / `_RecvDoneCallback` / `_SendDoneCallback`
- 加声明：`OnChannelReady` / `OnChannelData` / `OnChannelClosed`
- `Config` 里 `certificate_file` / `private_key_file` / `private_key_password` 换成一个 `TlsConfig tls_cfg;`，并加 `TTVpnPolicy policy;` / `DnsConfig dns;`
- `SendPacket(JMPacketPtr pkt)` → `SendPacket(SMPacketPtr pkt)`

`src/cpp/WSConnector.cpp`：
- 删 `#define BC_R_IDLE_TIMEOUT` / `BC_R_CONNECT_TIMEOUT`（`WSConnector.cpp:32-33`），改 `#include "TTErrors.h"`
- `_Inter_Connect`（`486-649`）整段重写：不再自己 `ParseAddrFromUrl` + `bc_net_pton` + 建 socket + 建 SSL_CTX，改为解析出 host/port/location/bSSL 后填 `TcpChannelConfig` 并 `channel_->Open(cfg, this)`。**域名支持就是在这里获得的**——`TcpChannel` 内部走 `DnsResolver`
- `OnChannelReady` 里做原来 `_OnConnectDone`（`651-679`）之后的事：发 WebSocket Upgrade 请求（`_Inter_Send_Request`）
- `OnChannelData` 里做原来 `OnRecvDataFromSSL` 之后的事：未 upgrade 时喂 `llhttp_execute` 解析握手响应，已 upgrade 时喂 `ParseWSFrame`
- `OnWSWrite` / `_SendV`（`815-817`）改为 `channel_->Send(buffer)`
- `_Cleanup`（`1029-1037`）删掉 `SSL_free` / `SSL_CTX_free`，改为 `channel_.reset()`
- `WSConnector::Close`（`1487-1488`）删掉遍历连接 `SSL_free` 的逻辑

- [ ] **Step 4: 从 CMakeLists 排除列表里放回**

删掉 Task 1 在 `src/CMakeLists.txt` 三处加的 `list(REMOVE_ITEM ... WSConnector.cpp WSParser.cpp)` 段落（连同上方的解释注释一起删）。

- [ ] **Step 5: 验证全量构建**

```bash
rm -rf /tmp/ttbuild && mkdir -p /tmp/ttbuild && cd /tmp/ttbuild
cmake /Users/antonio/chative/ttsignal/src -DCMAKE_BUILD_TYPE=Debug \
      -DBUILD_NODE_ADDON=ON -DBUILD_TOOLS=ON
make ttsignal -j8 2>&1 | tail -3
grep -o "cpp/WSConnector\.cpp\|cpp/WSParser\.cpp" \
     CMakeFiles/ttsignal.dir/build.make | sort -u
```

Expected: `[100%] Built target ttsignal`，且 grep 输出两行（确认已放回编译目标）。

- [ ] **Step 6: 验证 wss 能连上并支持域名**

用一个公共 WebSocket 回声服务验证域名解析 + TLS + WS 握手三件事一起工作：

```bash
/tmp/ttbuild/tools.dir/httpget --url https://echo.websocket.org/ --vpn-policy os
```

Expected: `status: 200`（先确认 HTTPS 到该域名通）。

WS 侧另写一个最小验证程序（或扩展 `tools/httpget.cpp` 加 `--ws` 分支），连 `wss://echo.websocket.org/`，发一条文本帧并确认收到回声。Expected: `OnConnectResult result=0`，随后 `OnRecvText` 收到发出去的内容。

- [ ] **Step 7: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add src/cpp/WSConnector.h src/cpp/WSConnector.cpp \
        src/cpp/WSParser.h src/cpp/WSParser.cpp \
        src/cpp/Interface.h src/CMakeLists.txt
git commit -m "feat(ws): WSConnector 接入 TcpChannel，支持 wss 与域名

把从 jmp 拷来的 WSConnector 改造成能编译进 ttsignal 的版本：
- 去掉 Acceptor.h / JMPacket.h 依赖，JMP 命名空间换成 SMP
- 连接部分（DNS/socket/SSL 约 300 行）整段换成 TcpChannel
- Interface.h 补上 IWSConnectorHandler / IWSConnectionHandler
- 从 CMakeLists 的排除列表放回编译目标

原实现把 ParseAddrFromUrl 返回的 host 直接喂给 bc_net_pton，只能连 IP
字面量；现在走 TcpChannel 的 DnsResolver，域名可用。TLS 也从"建了
SSL_CTX 但不校验证书、无 SNI"变成走 TlsContext 的完整校验。"
```

---

### Task 8: 真实 VPN 场景抓包验证

前面所有任务验证的都是"功能可用"，这一步验证的是"真的绕过了 VPN"——这是整个改造的目的，不能省略。

**Files:**
- Modify: `docs/vpn-policy.md`（追加 TCP/HTTP 路径的验证记录）

**Interfaces:**
- Consumes: `tools/httpget`（Task 6）
- Produces: 验证记录文档

- [ ] **Step 1: 准备对照环境**

需要一个能回显 client IP 的接口。`https://ifconfig.me/ip` 或 `https://api.ipify.org` 都可以。先在**关闭 VPN** 的情况下记下真实出口 IP：

```bash
curl -s https://ifconfig.me/ip; echo
```

记为 `REAL_IP`。

- [ ] **Step 2: 开启 VPN 全局 TUN 模式，确认基线被污染**

开启 Clash / Surge / mihomo 的全局 TUN 模式，然后：

```bash
curl -s https://ifconfig.me/ip; echo
```

Expected: 输出与 `REAL_IP` **不同**（VPN 出口 IP）。若相同说明 TUN 模式没真正生效，需先调整代理配置再继续——否则后面的对照没有意义。

- [ ] **Step 3: 验证 os 策略跟随 VPN**

```bash
/tmp/ttbuild/tools.dir/httpget --url https://ifconfig.me/ip --vpn-policy os
```

Expected: body 是 VPN 出口 IP，`boundIfIndex: 0`，`pinMethod: (未绑定)`。这确认了没有把 VPN 用户误伤成不能用。

- [ ] **Step 4: 验证 force-physical 拿到真实 IP**

```bash
/tmp/ttbuild/tools.dir/httpget --url https://ifconfig.me/ip \
    --vpn-policy force-physical --log-level debug
```

Expected: body **等于 `REAL_IP`**，`boundIfIndex` 非 0，`pinMethod` 为 `IP_BOUND_IF`（macOS）。

若失败，日志必须能说清卡在哪一步（找不到物理网卡 / 绑定失败 / 路由复核不一致 / DNS 超时）——这本身也是验收项：`force-physical` 的失败必须可诊断。

- [ ] **Step 5: 抓包确认 DNS 与 TCP 都走物理网卡**

开两个终端。终端 A 抓包（`en0` 按实际物理网卡名调整）：

```bash
sudo tcpdump -i en0 -n "port 53 or (tcp[tcpflags] & tcp-syn != 0)" -c 20
```

终端 B 发请求：

```bash
/tmp/ttbuild/tools.dir/httpget --url https://ifconfig.me/ip \
    --vpn-policy force-physical
```

Expected（在终端 A 看到）：
1. 一个到 `223.5.5.5:53` 或 `8.8.8.8:53` 的 DNS 查询——**目标是配置的公共 DNS，不是 VPN 的 DNS**
2. 一个 TCP SYN，目标 IP 是 `ifconfig.me` 的**真实 IP，不是 `198.18.x.x` 段的 fake-IP**

同时在 `utun` 接口上抓一次做反证：

```bash
sudo tcpdump -i utun0 -n "port 53 or (tcp[tcpflags] & tcp-syn != 0)" -c 5
```

Expected: 上述请求期间 `utun0` 上**看不到**对应的 DNS 查询与 SYN。

- [ ] **Step 6: 验证 prefer-physical 的回落行为**

关闭所有物理网卡（或断开 Wi-Fi），只留 VPN，然后：

```bash
/tmp/ttbuild/tools.dir/httpget --url https://ifconfig.me/ip --vpn-policy prefer-physical
/tmp/ttbuild/tools.dir/httpget --url https://ifconfig.me/ip --vpn-policy force-physical
```

Expected: 前者成功（回落隧道，body 是 VPN 出口 IP，日志有 `_WARN_` 说明回落）；后者失败，错误信息为 `force-physical：找不到可用的物理网卡...`。

- [ ] **Step 7: 把验证记录写进文档**

在 `docs/vpn-policy.md` 末尾追加一节「TCP / HTTP 路径的抓包对照验证」，格式参照该文件里 commit `22f7db9` 已有的 force-physical 验证记录。内容包含：环境（OS 版本、VPN 软件与模式）、每一步的实际命令与实际输出、`tcpdump` 的关键行、结论。

**如实记录。** 某一项没验证或验证失败，就写清楚没验证/失败了什么，不要写成通过。

- [ ] **Step 8: Commit**

```bash
cd /Users/antonio/chative/ttsignal
git add docs/vpn-policy.md
git commit -m "docs(vpn): 补充 TCP/HTTP 路径的抓包对照验证记录

在 Clash 全局 TUN 模式下验证 https 请求的三档策略行为：
- os 跟随 VPN，服务端看到 VPN 出口 IP
- force-physical 拿到真实 IP，DNS 查询与 TCP SYN 都从 en0 出去，
  utun0 上抓不到对应报文
- prefer-physical 在无物理网卡时回落隧道，force-physical 明确失败"
```

---

## Self-Review

**1. Spec coverage**

| Spec 章节 | 对应任务 |
|---|---|
| 一、模块划分 | Task 2/3/4/5/6 分别实现五个模块 |
| 二、文件清单（新增） | Task 3 建 `TTErrors.h`；Task 2/3/4/5/6 建各自模块；Task 6 建 `tools/httpget` |
| 二、文件清单（改造） | Task 2 改 `UDPSender`；Task 3 改 `SMPConnector`；Task 7 改 `WSConnector`/`WSParser`/`Interface.h`/`CMakeLists` |
| 二、文件清单（删除） | Task 1 删 `WSServer` 与 `jni/JNI_WS*` |
| 三、SocketPinner | Task 2 全部 |
| 四、DnsResolver | Task 4 全部 |
| 五、TlsContext | Task 3 全部 |
| 六、TcpChannel | Task 5 全部 |
| 七、VPN 绕过数据流 | Task 5 Step 2 实现六步；Task 8 抓包验证 |
| 八、HttpConnector API | Task 6 全部 |
| 九、配置项 | Task 6 Step 3 要点 1 |
| 十、错误处理 | Task 3 Step 1 建 `TTErrors.h`；各任务在失败分支填 `errMessage` |
| 测试 | Task 2/3/4 单测；Task 6 端到端；Task 8 真实 VPN |
| 实施顺序 | 与 spec 的 8 步一致，仅在最前面插入了 Task 1「恢复绿色构建」 |

**与 spec 的两处偏差**（均为实现期发现，已在计划中说明理由）：

1. **新增 Task 1「恢复绿色构建」**。spec 没料到 `file(GLOB)` 会把未移植的拷贝文件收进编译目标——实测 `make` 直接失败于 `Acceptor.h not found`。没有绿色构建，后续任何任务都无法验证，所以必须前置。
2. **Task 6 需要先把 `LLHTTPParser` 从 `WSParser` 拆出**。spec 的模块划分里没有这一项，但 `HttpConnector` 依赖它，而 `WSParser.cpp` 在 Task 7 之前一直被排除出构建，不拆会链接失败。同时需要给 `LLHTTPParser` 补一个 `http_on_body` 虚函数——原本六个回调里没有收 body 的那个。

**2. Placeholder scan**

`TcpChannel.cpp`（Task 5 Step 2）与 `HttpConnector.cpp`（Task 6 Step 3）给的是要点 + 关键代码片段而非整份实现，因为这两个文件的主体是对 `WSConnector.cpp` 现有异步回调模式的复用，计划中已逐条给出可照抄的行号来源（`WSConnector.cpp:617-618, 624-630, 669-703, 815-817, 428-430`）。失败分支、路由复核、绑定这三处最容易写错的地方给了完整代码。

`SocketPinner.cpp`（Task 2 Step 4）以搬运对照表 + 骨架的形式给出，因为它的内容是 `UDPSender.cpp:530-681` 与 `1304-1358` 的整体移动——把 150 行原文抄进计划反而增加抄错的机会，对照表把每处变量替换写死了。

**3. Type consistency**

- `TTPinRequest` / `TTPinResult` / `tt_socket_pin` / `tt_socket_unpin` —— Task 2 定义，Task 4（DNS socket）、Task 5（TCP socket）使用，签名一致
- `DnsConfig` / `DnsResult` / `DnsResolver::Resolve` —— Task 4 定义，Task 5 `TcpChannelConfig::dns` 引用
- `TlsConfig` / `TlsContext::Create` / `PrepareSsl` —— Task 3 定义，Task 5 `TcpChannelConfig::tls_cfg` 引用
- `HttpHeaderMap` —— Task 6 从 `WSParser.h` 拆到 `LLHTTPParser.h`，Task 6（`HttpRequest`/`HttpResponse`）与 Task 7（`IWSConnectionHandler::OnConnectResult`）共用同一定义
- `ITcpChannelHandler` 的三个方法 `OnChannelReady` / `OnChannelData` / `OnChannelClosed` —— Task 5 定义，Task 6（`HttpConnection`）与 Task 7（`WSConnection`）实现，名称一致
- `BC_R_*` 错误码 —— Task 3 集中定义，Task 4（`BC_R_DNS_FAILED`）、Task 5（`BC_R_PIN_FAILED` / `BC_R_ROUTE_MISMATCH` / `BC_R_CONNECT_TIMEOUT`）、Task 6（`BC_R_RESPONSE_TOO_LARGE`）使用。`BC_R_NO_PHYSICAL_INTERFACE`（Task 5 也用）来自 env 的 `Config.h`，取值 64，不重新定义
- `SMPacketPtr` —— Task 7 替换 `JMPacketPtr`，`WSParser::PackPacket` 与 `WSConnection::SendPacket` 两处签名同步
