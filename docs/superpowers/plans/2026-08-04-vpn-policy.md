# vpnPolicy 三态网卡选择策略 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把现有的 `bypassVpn` 布尔开关升级为 `os` / `prefer-physical` / `force-physical` 三态策略，让业务能强制 QUIC 流量走物理网卡，使服务端拿到客户端真实 IP 而非 VPN 出口 IP。

**Architecture:** 新增一个不依赖任何平台 API 的纯逻辑单元 `VpnPolicy`（枚举 + 字符串解析 + 新旧键合并），由它统一喂给四个消费方：三平台的路径监视器（决定挑哪块网卡）、`UDPSender`（决定是否允许撤销网卡绑定）、`SMPConnection::Connect`（`force-physical` 下无物理网卡时快速失败）。平台差异全部收敛在各自的 monitor 实现里，跨平台契约只有 `INetworkPathMonitor.h` 一个头文件。

**Tech Stack:** C++17、Objective-C++（Apple）、Win32 iphlpapi（Windows）、NETLINK_ROUTE（Linux）、N-API（Node.js 绑定）、Swift（iOS 绑定）、CMake。

## Global Constraints

- 依据的设计文档：`docs/superpowers/specs/2026-08-03-vpn-policy-design.md`。有冲突以 spec 为准，并在 PR 中说明。
- 策略取值编码固定为 `-1` unset / `0` os / `1` prefer-physical / `2` force-physical，四处（`TTVpnPolicy`、`TTNetworkMonitorOptions.vpnPolicy`、`TTConfig.vpnPolicy`、日志）必须一致。
- 平台默认值：macOS / Windows / Linux 为 `prefer-physical`，iOS（含模拟器，不含 Mac Catalyst）为 `os`。
- `TTNetworkMonitorOptions` 与 `TTConfig` 只能在结构体**末尾追加**字段，永不重排——两者都是已发布的 C ABI。
- `bypassVpn` 字段在所有层级一律**保留**，标记 deprecated，不删除。
- 新增 C++ 测试遵循仓库既有约定（见 `src/cpp/tests/MasqueFraming_test.cpp`）：standalone `.cpp` + `CHECK` 宏 + 文件头注释里写明手工编译命令，**不接入 CMake**。
- 中文注释与英文注释混排是本仓库既有风格，跟随被修改文件的现有语言，不要统一改写。
- 提交信息用中文，遵循仓库现有的 `type(scope): 描述` 格式，**禁止**添加 `Co-Authored-By` 或任何 AI 署名。

## 平台构建与验证能力（影响各任务的验证方式）

| 平台 | 能否在本机（macOS arm64）构建 | 命令 |
|---|---|---|
| macOS NAPI | 能，且能运行 | `cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8` |
| Linux x64/arm64 | **能交叉编译**（homebrew cross toolchain），不能运行 | `bash build/linux-x64-release/build` |
| iOS | 能构建，需真机/模拟器运行 | `build/ios-sim-arm64-release/build` |
| Windows | **不能**，需 Windows 机器 | `scripts/build-for-win.bat` |

Windows 相关任务（Task 7）只能做到"交付代码 + 在 Windows 机器上验证"，本机无法编译。执行到该任务时若手头没有 Windows 环境，**如实标注为未验证**，不要声称通过。

## File Structure

**新建**

| 文件 | 职责 |
|---|---|
| `src/cpp/VpnPolicy.h` | `TTVpnPolicy` 枚举 + 4 个纯函数声明。无平台依赖，可被任何 TU include |
| `src/cpp/VpnPolicy.cpp` | 上述纯函数实现。唯一的平台条件编译是 `tt_vpn_policy_platform_default()` 里的 `TargetConditionals` 判断 |
| `src/cpp/linux/LinuxPhysicalIface.h` | Linux 物理网卡判定声明。sysfs 根路径可注入，便于测试 |
| `src/cpp/linux/LinuxPhysicalIface.cpp` | 用 `/sys/class/net/<name>/device` 判定 + 名字黑名单兜底。纯 POSIX，可在 macOS 上编译测试 |
| `src/cpp/tests/VpnPolicy_test.cpp` | Task 1 的单测 |
| `src/cpp/tests/LinuxPhysicalIface_test.cpp` | Task 3 的单测，自建临时 sysfs 目录树 |

**修改**

| 文件 | 改什么 |
|---|---|
| `deps/env/src/BC/Config.h:388` | 新增 `BC_R_NO_PHYSICAL_INTERFACE 64`，`BC_R_NRESULTS` 改 65 |
| `deps/env/src/BC/BCStrError.cpp:682` | `bc_result2string` 加一个 case |
| `src/cpp/INetworkPathMonitor.h` | include `VpnPolicy.h`；`TTNetworkMonitorOptions` 末尾加 `int vpnPolicy`；新增 `tt_netmon_query_default_ifindex_ex` |
| `src/cpp/apple/AppleNetworkMonitor.mm` | Monitor 存 policy；`ResolveActiveIfIndex` 合并 macOS/iOS 分支并实现三态 |
| `src/cpp/win32/WinIpChangeMonitor.cpp` | Monitor 存 policy；`QueryBestIfIndex` 支持 physical-first |
| `src/cpp/linux/LinuxNetlinkMonitor.cpp` | Monitor 存 policy；`IsVirtualIface` 换成 `LinuxPhysicalIface` 且受 policy 控制 |
| `src/cpp/linux/CMakeLists.txt` | 加入 `LinuxPhysicalIface.cpp` |
| `src/cpp/SMPConnector.h` | `Config` 加 `vpn_policy` 字段 |
| `src/cpp/SMPConnector.cpp` | `Config::Init` 解析；`tt_netmon_start` 传 policy；`SMPConnection::Create` 下传给 UDPSenderGroup；`Connect` 快速失败 |
| `src/cpp/UDPSender.h` / `.cpp` | 存 policy；`force-physical` 下抑制两处 unpin；Linux `SO_BINDTODEVICE` |
| `src/cpp/UDPSenderGroup.h` / `.cpp` | 转发 `SetVpnPolicy` 给每个 sender |
| `src/cpp/apple/ios_bridge.h` / `.mm` | `TTConfig` 加 `vpnPolicy`，写入 BCFObject |
| `src/swift/TTSignalConfig.swift` | `TTSignalVPNPolicy` 枚举 + 属性，`bypassVpn` 标 deprecated |
| `src/js/index.js` | `createConnector` JSDoc + 取值校验 |
| `src/CMakeLists.txt` | 加入 `VpnPolicy.cpp` |

---

### Task 1: VpnPolicy 纯逻辑单元

策略的所有决策规则集中在这里，且完全不碰 OS API，所以能被真正单测覆盖。后续所有任务只消费它的输出。

**Files:**
- Create: `src/cpp/VpnPolicy.h`
- Create: `src/cpp/VpnPolicy.cpp`
- Create: `src/cpp/tests/VpnPolicy_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: 无
- Produces:
  - `typedef enum { TT_VPN_POLICY_UNSET=-1, TT_VPN_POLICY_OS=0, TT_VPN_POLICY_PREFER_PHYSICAL=1, TT_VPN_POLICY_FORCE_PHYSICAL=2 } TTVpnPolicy;`
  - `TTVpnPolicy tt_vpn_policy_platform_default(void);`
  - `TTVpnPolicy tt_vpn_policy_from_string(const char* s);`
  - `const char* tt_vpn_policy_to_string(TTVpnPolicy p);`
  - `TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy, int hasBypassVpn, int bypassVpn, TTVpnPolicy platformDefault, int* outBothGiven);`

- [ ] **Step 1: 写失败的测试**

创建 `src/cpp/tests/VpnPolicy_test.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : VpnPolicy_test.cpp
//
// Standalone unit test for the vpnPolicy resolution helpers. Not part of any
// build target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp src/cpp/VpnPolicy.cpp \
//       src/cpp/tests/VpnPolicy_test.cpp -o /tmp/vpnpolicy_test && /tmp/vpnpolicy_test
//
///////////////////////////////////////////////////////////////////////////////
#include "VpnPolicy.h"

#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_from_string()
{
    CHECK(tt_vpn_policy_from_string("os") == TT_VPN_POLICY_OS);
    CHECK(tt_vpn_policy_from_string("prefer-physical") == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(tt_vpn_policy_from_string("force-physical") == TT_VPN_POLICY_FORCE_PHYSICAL);
    // 未知 / 空 / NULL 一律 UNSET，由调用方决定是否告警
    CHECK(tt_vpn_policy_from_string("physical") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string("") == TT_VPN_POLICY_UNSET);
    CHECK(tt_vpn_policy_from_string(NULL) == TT_VPN_POLICY_UNSET);
    // 大小写敏感：不做归一化，避免和其它配置键的处理方式不一致
    CHECK(tt_vpn_policy_from_string("OS") == TT_VPN_POLICY_UNSET);
}

static void test_to_string()
{
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_OS), "os") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_PREFER_PHYSICAL), "prefer-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_FORCE_PHYSICAL), "force-physical") == 0);
    CHECK(strcmp(tt_vpn_policy_to_string(TT_VPN_POLICY_UNSET), "unset") == 0);
    // 越界值不能崩，也不能返回 NULL（日志里直接 %s 用）
    CHECK(tt_vpn_policy_to_string((TTVpnPolicy)99) != NULL);
}

// 完整的 4 x 3 组合矩阵：explicitPolicy x (hasBypassVpn, bypassVpn)
static void test_resolve_matrix()
{
    const TTVpnPolicy kDefault = TT_VPN_POLICY_PREFER_PHYSICAL;
    int both = -1;

    // --- explicitPolicy = UNSET：回落到 bypassVpn 兼容映射，再回落平台默认
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, kDefault, &both) == kDefault);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 0, kDefault, &both) == TT_VPN_POLICY_OS);
    CHECK(both == 0);
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 1, 1, kDefault, &both) == TT_VPN_POLICY_PREFER_PHYSICAL);
    CHECK(both == 0);

    // --- explicitPolicy 已给：永远胜出，且同时给了 bypassVpn 时置 outBothGiven
    const TTVpnPolicy kExplicit[] = {
        TT_VPN_POLICY_OS, TT_VPN_POLICY_PREFER_PHYSICAL, TT_VPN_POLICY_FORCE_PHYSICAL,
    };
    for (int i = 0; i < 3; i++) {
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 0, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 0);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 0, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
        CHECK(tt_vpn_policy_resolve(kExplicit[i], 1, 1, kDefault, &both) == kExplicit[i]);
        CHECK(both == 1);
    }

    // 平台默认值是注入的，换一个也要正确回落（模拟 iOS）
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0, TT_VPN_POLICY_OS, &both) == TT_VPN_POLICY_OS);

    // outBothGiven 允许传 NULL
    CHECK(tt_vpn_policy_resolve(TT_VPN_POLICY_OS, 1, 1, kDefault, NULL) == TT_VPN_POLICY_OS);
}

// 平台默认值是编译期决定的，只能断言"当前宿主平台"的期望值
static void test_platform_default()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_OS);
#else
    CHECK(tt_vpn_policy_platform_default() == TT_VPN_POLICY_PREFER_PHYSICAL);
#endif
}

int main()
{
    test_from_string();
    test_to_string();
    test_resolve_matrix();
    test_platform_default();
    if (g_failures == 0) {
        printf("VpnPolicy_test: ALL PASS\n");
        return 0;
    }
    printf("VpnPolicy_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
```

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
c++ -std=c++17 -I src/cpp src/cpp/VpnPolicy.cpp \
    src/cpp/tests/VpnPolicy_test.cpp -o /tmp/vpnpolicy_test && /tmp/vpnpolicy_test
```
Expected: 编译失败，`fatal error: 'VpnPolicy.h' file not found`

- [ ] **Step 3: 写头文件**

创建 `src/cpp/VpnPolicy.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : VpnPolicy.h
// author : anto
//
// VPN / 虚拟网卡选择策略。三态取代原先的 bypassVpn 布尔开关：
//
//   os              — 完全跟随系统路由，允许 QUIC 流量走 VPN 隧道
//   prefer-physical — 优先物理网卡（wifi / wired / cellular）；启动时找不到
//                     物理网卡可回落到隧道，运行中拒绝回落（保持现有 socket）
//   force-physical  — 只接受物理网卡。任何阶段都不回落，且禁止 UDPSender 撤销
//                     已有的网卡绑定。找不到物理网卡时连接直接失败，业务据此
//                     决定是否降级重连
//
// 本文件不依赖任何平台 API，可以被任意 TU include，也可以单独编译进单测。
// 唯一的条件编译在 tt_vpn_policy_platform_default() 里。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_VPN_POLICY_H
#define TT_VPN_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

// 数值编码是跨 ABI 契约的一部分（TTNetworkMonitorOptions::vpnPolicy 和
// TTConfig::vpnPolicy 都以 int 传递同一组取值），不要改动。
typedef enum {
    TT_VPN_POLICY_UNSET           = -1,  // 未设置，由调用方决定回落
    TT_VPN_POLICY_OS              = 0,
    TT_VPN_POLICY_PREFER_PHYSICAL = 1,
    TT_VPN_POLICY_FORCE_PHYSICAL  = 2,
} TTVpnPolicy;

// 当前平台在调用方什么都没配时应该采用的策略。
//   iOS / iPadOS / tvOS / watchOS（不含 Mac Catalyst） -> TT_VPN_POLICY_OS
//   macOS / Windows / Linux / 其它                     -> TT_VPN_POLICY_PREFER_PHYSICAL
//
// iOS 单独定为 os，是因为 iOS 上安装 per-app VPN 的用户通常就是希望流量走
// VPN；而 macOS/Linux 的既有默认行为本来就等价于 prefer-physical。Windows
// 的既有行为其实是 os，这里有意对齐为 prefer-physical，属于本次唯一有意引入
// 的默认行为变更（详见 spec）。
TTVpnPolicy tt_vpn_policy_platform_default(void);

// 解析配置里的字符串取值。大小写敏感，只认 "os" / "prefer-physical" /
// "force-physical"。NULL、空串、无法识别的取值一律返回 TT_VPN_POLICY_UNSET，
// 由调用方决定是否打告警日志。
TTVpnPolicy tt_vpn_policy_from_string(const char* s);

// 供日志使用。任何取值（含越界）都返回非 NULL 的静态字符串。
const char* tt_vpn_policy_to_string(TTVpnPolicy p);

// 合并新键 vpnPolicy 与旧键 bypassVpn，得出最终生效的策略。
//
//   explicitPolicy  : tt_vpn_policy_from_string 的结果，未配置时传 UNSET
//   hasBypassVpn    : 配置里是否出现过 bypassVpn 键（非 0 表示出现过）
//   bypassVpn       : 该键的布尔值。hasBypassVpn 为 0 时本参数被忽略
//   platformDefault : 两个键都没给时的回落值，通常传
//                     tt_vpn_policy_platform_default()。做成参数是为了让单测
//                     能在任意宿主平台上验证回落逻辑
//   outBothGiven    : 可传 NULL。两个键同时出现时置 1，否则置 0。调用方应据
//                     此打一条 _WARN_ 提示 bypassVpn 被忽略
//
// 优先级：explicitPolicy > bypassVpn 兼容映射 > platformDefault。
// 兼容映射：bypassVpn=false -> os，bypassVpn=true -> prefer-physical。
// 返回值永远是 OS / PREFER_PHYSICAL / FORCE_PHYSICAL 三者之一，不会是 UNSET。
TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven);

#ifdef __cplusplus
}
#endif

#endif // TT_VPN_POLICY_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 4: 写实现**

创建 `src/cpp/VpnPolicy.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : VpnPolicy.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "VpnPolicy.h"

#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

extern "C" {

TTVpnPolicy tt_vpn_policy_platform_default(void)
{
    // Mac Catalyst 虽然 TARGET_OS_IPHONE=1，但跑在桌面上、面对的是同一批
    // TUN 模式代理，所以按桌面处理。
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return TT_VPN_POLICY_OS;
#else
    return TT_VPN_POLICY_PREFER_PHYSICAL;
#endif
}

TTVpnPolicy tt_vpn_policy_from_string(const char* s)
{
    if (s == NULL || s[0] == '\0') {
        return TT_VPN_POLICY_UNSET;
    }
    if (strcmp(s, "os") == 0) {
        return TT_VPN_POLICY_OS;
    }
    if (strcmp(s, "prefer-physical") == 0) {
        return TT_VPN_POLICY_PREFER_PHYSICAL;
    }
    if (strcmp(s, "force-physical") == 0) {
        return TT_VPN_POLICY_FORCE_PHYSICAL;
    }
    return TT_VPN_POLICY_UNSET;
}

const char* tt_vpn_policy_to_string(TTVpnPolicy p)
{
    switch (p) {
        case TT_VPN_POLICY_OS:              return "os";
        case TT_VPN_POLICY_PREFER_PHYSICAL: return "prefer-physical";
        case TT_VPN_POLICY_FORCE_PHYSICAL:  return "force-physical";
        case TT_VPN_POLICY_UNSET:           return "unset";
        default:                            return "invalid";
    }
}

TTVpnPolicy tt_vpn_policy_resolve(TTVpnPolicy explicitPolicy,
                                  int hasBypassVpn,
                                  int bypassVpn,
                                  TTVpnPolicy platformDefault,
                                  int* outBothGiven)
{
    const int explicitGiven = (explicitPolicy == TT_VPN_POLICY_OS ||
                               explicitPolicy == TT_VPN_POLICY_PREFER_PHYSICAL ||
                               explicitPolicy == TT_VPN_POLICY_FORCE_PHYSICAL);

    if (outBothGiven) {
        *outBothGiven = (explicitGiven && hasBypassVpn) ? 1 : 0;
    }
    if (explicitGiven) {
        return explicitPolicy;
    }
    if (hasBypassVpn) {
        return bypassVpn ? TT_VPN_POLICY_PREFER_PHYSICAL : TT_VPN_POLICY_OS;
    }
    return platformDefault;
}

} // extern "C"

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 5: 运行测试确认通过**

Run:
```bash
c++ -std=c++17 -I src/cpp src/cpp/VpnPolicy.cpp \
    src/cpp/tests/VpnPolicy_test.cpp -o /tmp/vpnpolicy_test && /tmp/vpnpolicy_test
```
Expected: `VpnPolicy_test: ALL PASS`，退出码 0

- [ ] **Step 6: 把 VpnPolicy.cpp 加进构建**

在 `src/CMakeLists.txt` 里找到核心源文件列表（与 `cpp/Utils.cpp`、`cpp/UDPSender.cpp` 并列的那一组），加入 `cpp/VpnPolicy.cpp`。该列表可能出现在多个 target（NAPI / JNI / iOS）分支里，用下面的命令确认全部改到：

```bash
grep -n "cpp/UDPSender.cpp" src/CMakeLists.txt
```
每一处出现 `cpp/UDPSender.cpp` 的地方，都在其后追加一行 `cpp/VpnPolicy.cpp`（保持原有缩进）。

- [ ] **Step 7: 构建 macOS NAPI 确认没破坏现有编译**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
```
Expected: 构建成功，产出 `node_modules/ttsignal/Debug/ttsignal.darwin.arm64.node`

- [ ] **Step 8: 提交**

```bash
git add src/cpp/VpnPolicy.h src/cpp/VpnPolicy.cpp src/cpp/tests/VpnPolicy_test.cpp src/CMakeLists.txt
git commit -m "feat(vpn): 新增 TTVpnPolicy 三态策略与纯解析逻辑

os / prefer-physical / force-physical 三态取代 bypassVpn 布尔开关。
本单元不依赖任何平台 API，兼容映射与平台默认值回落规则全部由
tt_vpn_policy_resolve 集中决定，配套 4x3 组合矩阵单测。"
```

---

### Task 2: BC_R_NO_PHYSICAL_INTERFACE 错误码

`force-physical` 下快速失败需要一个业务能区分的错误码。`deps/env` 是仓库内 vendored 的依赖（163 个文件，git 跟踪，非 submodule），可以直接改。已确认 `BC_R_NRESULTS` 在全仓没有被当作数组长度使用，`bc_result2string` 是 `switch` 而非索引数组，所以追加是安全的。

**Files:**
- Modify: `deps/env/src/BC/Config.h:388-390`
- Modify: `deps/env/src/BC/BCStrError.cpp:682`

**Interfaces:**
- Consumes: 无
- Produces: 宏 `BC_R_NO_PHYSICAL_INTERFACE`（值 `64`），可被 Task 10 的 `SMPConnection::Connect` 与 iOS/NAPI 回调直接使用

- [ ] **Step 1: 写失败的测试**

创建 `/tmp/bcresult_test.cpp`（临时文件，不入库——这个断言的价值只在本任务内，长期由编译器保证）：

```cpp
#include "BC/Config.h"
#include <cstdio>
#include <cstring>

extern "C" LPCSTR bc_result2string(BCRESULT result);

int main()
{
    if (BC_R_NO_PHYSICAL_INTERFACE != 64) {
        printf("FAIL: BC_R_NO_PHYSICAL_INTERFACE != 64\n");
        return 1;
    }
    if (BC_R_NRESULTS != 65) {
        printf("FAIL: BC_R_NRESULTS != 65\n");
        return 1;
    }
    const char* s = bc_result2string(BC_R_NO_PHYSICAL_INTERFACE);
    if (s == NULL || strcmp(s, "no physical network interface") != 0) {
        printf("FAIL: bc_result2string -> %s\n", s ? s : "(null)");
        return 1;
    }
    printf("bcresult_test: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
c++ -std=c++17 -I deps/env/src /tmp/bcresult_test.cpp \
    deps/env/src/BC/BCStrError.cpp -o /tmp/bcresult_test 2>&1 | head -5
```
Expected: 编译失败，`use of undeclared identifier 'BC_R_NO_PHYSICAL_INTERFACE'`

- [ ] **Step 3: 加错误码宏**

在 `deps/env/src/BC/Config.h` 中，把：

```c
#define BC_R_INVALIDARG			63  /*%< invalid arguments */

/*% Not a result code: the number of results. */
#define BC_R_NRESULTS 			64
```

改为：

```c
#define BC_R_INVALIDARG			63  /*%< invalid arguments */
#define BC_R_NO_PHYSICAL_INTERFACE	64  /*%< no physical network interface */

/*% Not a result code: the number of results. */
#define BC_R_NRESULTS 			65
```

- [ ] **Step 4: 加字符串映射**

在 `deps/env/src/BC/BCStrError.cpp` 的 `bc_result2string` switch 里，找到 `case BC_R_INVALIDARG` 那一行（switch 的最后一个 case），在它后面追加：

```cpp
	case    BC_R_NO_PHYSICAL_INTERFACE : return "no physical network interface";
```

- [ ] **Step 5: 运行测试确认通过**

Run:
```bash
c++ -std=c++17 -I deps/env/src /tmp/bcresult_test.cpp \
    deps/env/src/BC/BCStrError.cpp -o /tmp/bcresult_test && /tmp/bcresult_test
```
Expected: `bcresult_test: ALL PASS`

- [ ] **Step 6: 提交**

```bash
rm -f /tmp/bcresult_test /tmp/bcresult_test.cpp
git add deps/env/src/BC/Config.h deps/env/src/BC/BCStrError.cpp
git commit -m "feat(env): 新增 BC_R_NO_PHYSICAL_INTERFACE 错误码

force-physical 策略下找不到物理网卡时，connect 需要返回一个业务可区分的
错误码，而不是退化成通用超时。bc_result2string 是 switch 实现、
BC_R_NRESULTS 全仓无其它引用，追加安全。"
```

---

### Task 3: Linux 物理网卡判定

`LinuxNetlinkMonitor.cpp:80` 现有的 `IsVirtualIface` 是纯名字黑名单，用户重命名网卡就会失效。换成以 `/sys/class/net/<name>/device` 是否存在为主依据——只有背后真有硬件设备的网卡才有这个符号链接。拆成独立文件是为了让 sysfs 根路径可注入，从而能在 macOS 开发机上单测。

**Files:**
- Create: `src/cpp/linux/LinuxPhysicalIface.h`
- Create: `src/cpp/linux/LinuxPhysicalIface.cpp`
- Create: `src/cpp/tests/LinuxPhysicalIface_test.cpp`
- Modify: `src/cpp/linux/CMakeLists.txt`

**Interfaces:**
- Consumes: 无
- Produces:
  - `int tt_linux_iface_is_physical(const char* ifName);`
  - `int tt_linux_iface_is_physical_at(const char* ifName, const char* sysClassNetRoot);`

- [ ] **Step 1: 写失败的测试**

创建 `src/cpp/tests/LinuxPhysicalIface_test.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file : LinuxPhysicalIface_test.cpp
//
// Standalone unit test for the Linux physical-interface detector. The code
// under test is plain POSIX, so this compiles and runs on macOS too:
//
//   c++ -std=c++17 -I src/cpp src/cpp/linux/LinuxPhysicalIface.cpp \
//       src/cpp/tests/LinuxPhysicalIface_test.cpp -o /tmp/linuxphy_test \
//       && /tmp/linuxphy_test
//
///////////////////////////////////////////////////////////////////////////////
#include "linux/LinuxPhysicalIface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// 造一棵假的 /sys/class/net 目录树：
//   eth0      有 device/  -> 物理
//   wlan0     有 device/  -> 物理
//   tun0      无 device/  -> 虚拟
//   docker0   无 device/  -> 虚拟
//   myvpn     无 device/  -> 虚拟（名字不在黑名单里，验证 sysfs 判定确实生效）
//   eth-tun   有 device/  -> 物理（名字含 tun，验证 sysfs 优先于名字黑名单）
static std::string MakeFakeSysfs()
{
    char tmpl[] = "/tmp/ttsig_sysfs_XXXXXX";
    const char* root = mkdtemp(tmpl);
    if (!root) {
        printf("FAIL: mkdtemp\n");
        exit(1);
    }
    std::string base(root);
    const char* withDevice[]    = {"eth0", "wlan0", "eth-tun", NULL};
    const char* withoutDevice[] = {"tun0", "docker0", "myvpn", NULL};

    for (int i = 0; withDevice[i]; i++) {
        std::string d = base + "/" + withDevice[i];
        mkdir(d.c_str(), 0755);
        mkdir((d + "/device").c_str(), 0755);
    }
    for (int i = 0; withoutDevice[i]; i++) {
        mkdir((base + "/" + withoutDevice[i]).c_str(), 0755);
    }
    return base;
}

static void test_sysfs_detection()
{
    std::string root = MakeFakeSysfs();

    CHECK(tt_linux_iface_is_physical_at("eth0",    root.c_str()) == 1);
    CHECK(tt_linux_iface_is_physical_at("wlan0",   root.c_str()) == 1);
    // sysfs 是主依据：名字里带 tun 但有真实硬件，判为物理
    CHECK(tt_linux_iface_is_physical_at("eth-tun", root.c_str()) == 1);

    CHECK(tt_linux_iface_is_physical_at("tun0",    root.c_str()) == 0);
    CHECK(tt_linux_iface_is_physical_at("docker0", root.c_str()) == 0);
    // 名字不在任何黑名单里，但没有 device/，仍判为虚拟
    CHECK(tt_linux_iface_is_physical_at("myvpn",   root.c_str()) == 0);
}

// sysfs 整体不可读（容器 / 挂载受限）时回落到名字黑名单
static void test_name_blacklist_fallback()
{
    const char* kMissingRoot = "/tmp/ttsig_sysfs_does_not_exist_12345";

    CHECK(tt_linux_iface_is_physical_at("eth0",      kMissingRoot) == 1);
    CHECK(tt_linux_iface_is_physical_at("wlan0",     kMissingRoot) == 1);
    CHECK(tt_linux_iface_is_physical_at("enp3s0",    kMissingRoot) == 1);

    CHECK(tt_linux_iface_is_physical_at("lo",        kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tun0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tap0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tailscale0",kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("docker0",   kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("br-abc123", kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("veth1234",  kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("virbr0",    kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("vmnet1",    kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("zt0",       kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("kube-br0",  kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("cni0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("flannel.1", kMissingRoot) == 0);
}

static void test_bad_input()
{
    CHECK(tt_linux_iface_is_physical_at(NULL, "/tmp") == 0);
    CHECK(tt_linux_iface_is_physical_at("",   "/tmp") == 0);
    // 路径穿越 / 分隔符不该被当成合法网卡名
    CHECK(tt_linux_iface_is_physical_at("../etc", "/tmp") == 0);
    CHECK(tt_linux_iface_is_physical_at("a/b",    "/tmp") == 0);
    // root 传 NULL 时用默认 /sys/class/net，不能崩
    (void)tt_linux_iface_is_physical_at("eth0", NULL);
}

int main()
{
    test_sysfs_detection();
    test_name_blacklist_fallback();
    test_bad_input();
    if (g_failures == 0) {
        printf("LinuxPhysicalIface_test: ALL PASS\n");
        return 0;
    }
    printf("LinuxPhysicalIface_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
```

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
c++ -std=c++17 -I src/cpp src/cpp/linux/LinuxPhysicalIface.cpp \
    src/cpp/tests/LinuxPhysicalIface_test.cpp -o /tmp/linuxphy_test 2>&1 | head -5
```
Expected: 编译失败，`no such file or directory: 'src/cpp/linux/LinuxPhysicalIface.cpp'`

- [ ] **Step 3: 写头文件**

创建 `src/cpp/linux/LinuxPhysicalIface.h`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : LinuxPhysicalIface.h
// author : anto
//
// 判断一块 Linux 网卡是不是"真实物理网卡"。用于 prefer-physical /
// force-physical 策略下过滤掉 VPN 隧道与各种虚拟网桥。
//
// 主依据是 /sys/class/net/<name>/device 这个符号链接——只有背后真的挂着硬件
// 设备（PCI / USB / SDIO ...）的网卡才有它。tun/tap/bridge/veth/wireguard 都
// 是纯软件设备，没有。这比原先的名字前缀黑名单可靠得多：用户完全可以把物理
// 网卡改名叫 tun0，也可以把 WireGuard 接口改名叫 eth9。
//
// 名字黑名单保留为兜底，只在 sysfs 整体不可读时才生效（受限容器、挂了
// hidepid 的宿主、以及本文件的单测在 macOS 上跑的时候）。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_LINUX_PHYSICAL_IFACE_H
#define TT_LINUX_PHYSICAL_IFACE_H

#ifdef __cplusplus
extern "C" {
#endif

// 返回 1 表示物理网卡，0 表示虚拟网卡或无法判定。
// 内部走默认根路径 /sys/class/net。
int tt_linux_iface_is_physical(const char* ifName);

// 同上，但允许指定 /sys/class/net 的替代根路径，供单测注入假目录树。
// sysClassNetRoot 传 NULL 等同于调用 tt_linux_iface_is_physical。
//
// 判定顺序：
//   1. ifName 非法（NULL / 空 / 含 '/' 或以 '.' 开头）-> 0
//   2. <root>/<ifName>/device 存在 -> 1
//   3. <root>/<ifName> 存在但没有 device/ -> 0（确实是软件设备）
//   4. <root> 本身不可读 -> 回落名字黑名单
int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot);

#ifdef __cplusplus
}
#endif

#endif // TT_LINUX_PHYSICAL_IFACE_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 4: 写实现**

创建 `src/cpp/linux/LinuxPhysicalIface.cpp`：

```cpp
///////////////////////////////////////////////////////////////////////////////
// file   : LinuxPhysicalIface.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "LinuxPhysicalIface.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char* const kDefaultSysClassNet = "/sys/class/net";

// 名字前缀黑名单。仅在 sysfs 不可读时兜底使用，条目沿用
// LinuxNetlinkMonitor.cpp 原有的 IsVirtualIface 列表。
bool NameLooksVirtual(const char* name)
{
    static const char* const kBlacklist[] = {
        "lo",        // loopback
        "docker",    // docker0, docker_gwbridge
        "br-",       // docker bridge networks
        "veth",      // virtual eth between container and host
        "virbr",     // libvirt
        "vmnet",     // VMware host-only
        "tailscale", // tailscale0
        "tun",       // tun0..N (OpenVPN/WireGuard)
        "tap",       // tap0..N
        "zt",        // zerotier
        "kube-",     // kube-bridge variants
        "cni",       // cni0
        "flannel",   // flannel.1
        NULL,
    };
    for (int i = 0; kBlacklist[i]; i++) {
        if (strncmp(name, kBlacklist[i], strlen(kBlacklist[i])) == 0) {
            return true;
        }
    }
    return false;
}

// 网卡名不能含路径分隔符，也不能以 '.' 开头——否则拼进 sysfs 路径会变成
// 目录穿越。内核本身也不允许这样的接口名。
bool NameIsSane(const char* name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '.') return false;
    if (strchr(name, '/') != NULL) return false;
    return true;
}

bool PathExists(const char* path)
{
    struct stat st;
    // stat 而非 lstat：device 是符号链接，我们关心它指向的目标存不存在。
    return stat(path, &st) == 0;
}

} // namespace

extern "C" {

int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot)
{
    if (!NameIsSane(ifName)) {
        return 0;
    }
    const char* root = (sysClassNetRoot && sysClassNetRoot[0])
                     ? sysClassNetRoot
                     : kDefaultSysClassNet;

    // sysfs 根都不可读，只能靠名字猜
    if (!PathExists(root)) {
        return NameLooksVirtual(ifName) ? 0 : 1;
    }

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/device", root, ifName);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        // 名字长到把缓冲区撑爆，不可能是正常网卡（IF_NAMESIZE 是 16）
        return 0;
    }
    if (PathExists(path)) {
        return 1;
    }

    // <root>/<ifName> 存在但没有 device/：确凿的软件设备
    n = snprintf(path, sizeof(path), "%s/%s", root, ifName);
    if (n > 0 && (size_t)n < sizeof(path) && PathExists(path)) {
        return 0;
    }

    // 网卡在 sysfs 里根本不存在（刚被删掉 / 名字来自过期的 netlink 事件），
    // 退回名字判断而不是武断地判成物理网卡。
    return NameLooksVirtual(ifName) ? 0 : 1;
}

int tt_linux_iface_is_physical(const char* ifName)
{
    return tt_linux_iface_is_physical_at(ifName, NULL);
}

} // extern "C"

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
```

- [ ] **Step 5: 运行测试确认通过**

Run:
```bash
c++ -std=c++17 -I src/cpp src/cpp/linux/LinuxPhysicalIface.cpp \
    src/cpp/tests/LinuxPhysicalIface_test.cpp -o /tmp/linuxphy_test && /tmp/linuxphy_test
```
Expected: `LinuxPhysicalIface_test: ALL PASS`

- [ ] **Step 6: 加进 Linux 构建**

在 `src/cpp/linux/CMakeLists.txt` 里找到源文件列表（含 `LinuxNetlinkMonitor.cpp` 的那一行/那一组），加入 `LinuxPhysicalIface.cpp`。

- [ ] **Step 7: 交叉编译 Linux 确认没破坏构建**

Run:
```bash
bash build/linux-x64-release/build 2>&1 | tail -20
```
Expected: 构建成功。若本机缺 `x86_64-unknown-linux-gnu` 交叉工具链，脚本会明确报错——此时如实记录"Linux 构建未验证"，不要跳过后继续声称通过。

- [ ] **Step 8: 提交**

```bash
git add src/cpp/linux/LinuxPhysicalIface.h src/cpp/linux/LinuxPhysicalIface.cpp \
        src/cpp/tests/LinuxPhysicalIface_test.cpp src/cpp/linux/CMakeLists.txt
git commit -m "feat(linux): 以 sysfs device 链接判定物理网卡

原 IsVirtualIface 是纯名字前缀黑名单，用户重命名网卡即失效。改以
/sys/class/net/<name>/device 是否存在为主依据，名字黑名单降级为
sysfs 不可读时的兜底。拆成独立文件以便注入假 sysfs 树做单测。"
```

---

### Task 4: C 契约扩展 —— TTNetworkMonitorOptions.vpnPolicy

只扩契约、只让三个 monitor 把 policy 解析出来存好并打进日志，**不改任何选网卡行为**。行为改动留给 Task 5/6/7，这样一旦后续任务出问题，可以精确定位到是契约层还是某个平台实现。

**Files:**
- Modify: `src/cpp/INetworkPathMonitor.h`
- Modify: `src/cpp/apple/AppleNetworkMonitor.mm:42`（Monitor 结构体）、`:352`（tt_netmon_start）、`:527`（query 函数）
- Modify: `src/cpp/win32/WinIpChangeMonitor.cpp:246`（tt_netmon_start）、`:299`（query 函数）
- Modify: `src/cpp/linux/LinuxNetlinkMonitor.cpp:371`（tt_netmon_start）、`:444`（query 函数）

**Interfaces:**
- Consumes: Task 1 的 `TTVpnPolicy`、`tt_vpn_policy_resolve`、`tt_vpn_policy_to_string`、`tt_vpn_policy_platform_default`
- Produces:
  - `TTNetworkMonitorOptions` 末尾新增 `int vpnPolicy;`
  - `int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy);`（三平台各自实现）
  - 各 `Monitor` 结构体内部新增 `TTVpnPolicy policy;` 成员（Task 5/6/7 消费）

- [ ] **Step 1: 扩展契约头文件**

在 `src/cpp/INetworkPathMonitor.h` 顶部的 `#include` 区加入：

```c
#include "VpnPolicy.h"
```

在 `TTNetworkMonitorOptions` 结构体**末尾**（`void* rawLogCtx;` 之后、右花括号之前）追加：

```c
    // 三态 VPN / 虚拟网卡策略，取值见 TTVpnPolicy。-1（TT_VPN_POLICY_UNSET）
    // 表示未设置，实现会回落到 bypassVpn 的兼容映射，再回落到平台默认值，
    // 因此不认识本字段的旧调用方（结构体零初始化会得到 0 = os，所以必须
    // 显式写 -1）行为不变。
    //
    // 注意：C-struct 零初始化会把本字段填成 0，也就是 TT_VPN_POLICY_OS，
    // 这跟"未设置"不是一回事。所有调用方在填充 options 时必须显式赋值，
    // SMPConnector 已经这么做（见 SMPConnector.cpp 的 tt_netmon_start 调用）。
    int vpnPolicy;
```

在 `tt_netmon_query_default_ifindex` 声明之后追加：

```c
// 同 tt_netmon_query_default_ifindex，但显式指定策略（取值见 TTVpnPolicy）。
//
// 原函数是已发布的导出符号且当前全仓没有内部调用方，为免改动其签名，新能力
// 走这个 _ex 变体；原函数保留为"传平台默认策略"的薄包装。
//
//   TT_VPN_POLICY_OS              — 返回系统默认路由的出口网卡，含 VPN 隧道
//   TT_VPN_POLICY_PREFER_PHYSICAL — 优先物理网卡，没有物理网卡时回落隧道
//   TT_VPN_POLICY_FORCE_PHYSICAL  — 只返回物理网卡，没有则返回 0
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy);
```

- [ ] **Step 2: Apple monitor 解析并记录 policy**

`src/cpp/apple/AppleNetworkMonitor.mm`：

(a) 在 `Monitor` 结构体里，把 `bool bypass_vpn = true;` 这一行替换为：

```cpp
    // 生效策略。由 tt_netmon_start 从 options->vpnPolicy 与 options->bypassVpn
    // 合并得出，start 返回后不再变更，无需加锁。
    TTVpnPolicy             policy       = TT_VPN_POLICY_PREFER_PHYSICAL;
```

(b) 在 `tt_netmon_start` 里，把：

```cpp
    // NULL options preserves the historical default (bypass VPN). Apps
    // that want OS-native behaviour explicitly pass bypassVpn=0.
    self->bypass_vpn = (options == nullptr) || (options->bypassVpn != 0);
```

替换为：

```cpp
    // options 为 NULL 时保持历史默认（等价于 bypassVpn=1，也就是
    // prefer-physical）。给了 options 就走 vpnPolicy / bypassVpn 合并规则。
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            /*hasBypassVpn=*/1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            /*outBothGiven=*/nullptr);
    }
```

(c) 在同一函数里，把 `ResolveActiveIfIndex(path, desc, ifName, /*physicalOnly=*/self->bypass_vpn)` 暂时改成 `/*physicalOnly=*/(self->policy != TT_VPN_POLICY_OS)`——**这是过渡写法**，Task 5 会把整个函数签名换掉。这样做是为了让本任务结束时行为与改动前完全等价（旧 `bypass_vpn==true` ⇔ 新 `policy!=OS`），保持每个任务都可独立验证。

(d) 在 `tt_netmon_query_default_ifindex` 之前新增 `_ex` 变体，原函数改为包装：

```cpp
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    (void)vpnPolicy;   // Task 5 接入；此刻行为与原函数一致
    return tt_netmon_query_default_ifindex();
}
```

- [ ] **Step 3: Windows monitor 解析并记录 policy**

`src/cpp/win32/WinIpChangeMonitor.cpp`：

(a) 在 `Monitor` 结构体末尾追加 `TTVpnPolicy policy = TT_VPN_POLICY_PREFER_PHYSICAL;`

(b) 在 `tt_netmon_start` 里，把 `(void)options;` 及其上方那段"bypassVpn intentionally ignored"注释替换为：

```cpp
    // 生效策略由 vpnPolicy / bypassVpn 合并得出。Windows 在本次改动前实际
    // 等价于 os（GetBestInterfaceEx 不做任何接口类型过滤），改动后默认对齐
    // 桌面其它平台的 prefer-physical——这是有意引入的默认行为变更，详见
    // docs/superpowers/specs/2026-08-03-vpn-policy-design.md。
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            /*hasBypassVpn=*/1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            /*outBothGiven=*/nullptr);
    }
```

注意 `self->policy` 的赋值必须在 `self->worker = std::thread(WorkerLoop, self);` **之前**，否则工作线程可能读到未初始化的值。

(c) 新增 `_ex` 变体，原函数改为包装（与 Apple 同形）：

```cpp
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    (void)vpnPolicy;   // Task 6 接入
    return tt_netmon_query_default_ifindex();
}
```

- [ ] **Step 4: Linux monitor 解析并记录 policy**

`src/cpp/linux/LinuxNetlinkMonitor.cpp`：

(a) 在 `Monitor` 结构体末尾追加 `TTVpnPolicy policy = TT_VPN_POLICY_PREFER_PHYSICAL;`

(b) 在 `tt_netmon_start` 里，把 `(void)options;` 及其上方注释替换为与 Windows 同形的合并逻辑（注释改为说明 Linux 现状：`IsVirtualIface` 今天就是无条件过滤，等价于 prefer-physical，所以默认值不变）。赋值必须在 `self->worker` 启动之前。

(c) 新增 `_ex` 变体，原函数改为包装：

```cpp
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    (void)vpnPolicy;   // Task 7 接入
    return tt_netmon_query_default_ifindex();
}
```

- [ ] **Step 5: 构建 macOS 与 Linux**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
cd .. && bash build/linux-x64-release/build 2>&1 | tail -10
```
Expected: 两个平台都构建成功

- [ ] **Step 6: 冒烟验证行为未变**

Run:
```bash
node -e "
const ttsignal = require('./src/js/index.js');
const c = ttsignal.createConnector({ alpn: 'ttsignal' });
console.log('connector created ok');
setTimeout(() => process.exit(0), 1500);
"
```
Expected: 打印 `connector created ok` 且进程正常退出。日志里 `tt_netmon_start ok bypassVpn=1` 这一行仍然出现（该日志在 Task 8 才会改成打印 policy）。

- [ ] **Step 7: 提交**

```bash
git add src/cpp/INetworkPathMonitor.h src/cpp/apple/AppleNetworkMonitor.mm \
        src/cpp/win32/WinIpChangeMonitor.cpp src/cpp/linux/LinuxNetlinkMonitor.cpp
git commit -m "feat(netmon): TTNetworkMonitorOptions 增加 vpnPolicy 字段

三平台 monitor 统一从 vpnPolicy / bypassVpn 合并出生效策略并存进
Monitor 结构体，同时新增 tt_netmon_query_default_ifindex_ex 变体
（原导出符号保留为薄包装）。本次只扩契约不改选网卡行为，各平台的
三态实现分别在后续任务落地。"
```

---

### Task 5: Apple monitor 实现三态并合并 macOS/iOS 分支

`ResolveActiveIfIndex` 现在是 `#if TARGET_OS_OSX` 分成两套重复实现，iOS 分支直接 `(void)physicalOnly`。合并成一份、由 policy 驱动，iOS 就自动获得 physical-first 能力。

**Files:**
- Modify: `src/cpp/apple/AppleNetworkMonitor.mm:107-227`（`ResolveActiveIfIndex` 整个函数）、`:418`（调用点）、`:527`（query 函数）

**Interfaces:**
- Consumes: Task 4 的 `Monitor::policy`、`tt_netmon_query_default_ifindex_ex`
- Produces: `static int64_t ResolveActiveIfIndex(nw_path_t path, std::string& descOut, std::string& ifNameOut, TTVpnPolicy policy, bool isStartupQuery)`

- [ ] **Step 1: 重写 ResolveActiveIfIndex**

把 `AppleNetworkMonitor.mm` 中从注释 `// Pick the interface to bind QUIC sockets to.` 开始、到 `ResolveActiveIfIndex` 函数结尾（含整个 `#if TARGET_OS_OSX` / `#else` / `#endif` 块）的内容，整体替换为：

```objc
// 挑选 QUIC socket 要绑定的网卡。Network.framework 按系统偏好顺序（默认路由
// 赢家在前）把网卡交给我们。NWPathMonitor 在 iOS 12 上不直接给
// nw_interface_get_index，用 if_nametoindex 取名字换索引是等价的，系统内部
// 也是这么做的。
//
// policy 决定"物理优先"和"能否回落到隧道"两件事：
//
//   TT_VPN_POLICY_OS
//       完全跟随系统顺序，第一块网卡胜出（含 utun / ipsec / ppp）。iOS 的
//       历史行为，也是 iOS 的默认值——装了 per-app VPN 的用户通常就是希望
//       流量走 VPN。
//
//   TT_VPN_POLICY_PREFER_PHYSICAL
//       遍历整条 path，优先挑第一块物理网卡（wifi / wired / cellular）。
//       找不到物理网卡时：启动查询允许回落到隧道（VPN-only 机器还得能
//       bootstrap），运行中的 update_handler 不回落而是返回 0。
//
//       为什么运行中不回落：Wi-Fi 切换过程中 path 常常有几百毫秒只剩 utun4，
//       这时把 socket 弹到隧道上只会打死连接（底下的物理链路同时也断了）。
//       返回 0 让 update_handler 保持当前 socket，下一次带着真实物理网卡的
//       更新再提交迁移。
//
//   TT_VPN_POLICY_FORCE_PHYSICAL
//       只接受物理网卡，启动查询也不回落。业务显式声明"必须拿到真实 IP"。
//
// 公司 VPN 客户端（Cisco、GlobalProtect）、Tailscale、WireGuard 和 iCloud
// 私密代理都会装一块 nw_interface_type_other 的 utunN / ipsecN / ppp* 并抢走
// 默认路由，这正是 prefer/force 两档要绕开的东西。
static int64_t ResolveActiveIfIndex(nw_path_t path,
                                    std::string& descOut,
                                    std::string& ifNameOut,
                                    TTVpnPolicy policy,
                                    bool isStartupQuery)
{
    __block int64_t      physicalIdx  = 0;
    __block std::string  physicalDesc;
    __block std::string  physicalName;

    __block int64_t      fallbackIdx  = 0;
    __block std::string  fallbackDesc;
    __block std::string  fallbackName;

    const bool osOrder = (policy == TT_VPN_POLICY_OS);

    nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
        const char* name = nw_interface_get_name(iface);
        if (!name || !name[0]) return true; // continue
        unsigned int idx = if_nametoindex(name);
        if (idx == 0) return true;

        nw_interface_type_t t = nw_interface_get_type(iface);
        const char* typeStr = "other";
        bool isPhysical = false;
        switch (t) {
            case nw_interface_type_wifi:     typeStr = "wifi";     isPhysical = true; break;
            case nw_interface_type_cellular: typeStr = "cellular"; isPhysical = true; break;
            case nw_interface_type_wired:    typeStr = "wired";    isPhysical = true; break;
            case nw_interface_type_loopback: typeStr = "loopback";                    break;
            default: break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%s)", typeStr, name);

        if (osOrder) {
            // 系统顺序：第一块可用网卡（loopback 除外）直接胜出。
            if (t == nw_interface_type_loopback) return true;
            fallbackIdx  = (int64_t)idx;
            fallbackName = name;
            fallbackDesc = buf;
            return false;
        }

        if (isPhysical) {
            if (physicalIdx == 0) {
                physicalIdx  = (int64_t)idx;
                physicalName = name;
                physicalDesc = buf;
            }
            // 第一块物理网卡胜出，提前结束枚举。
            return false;
        }
        if (t != nw_interface_type_loopback && fallbackIdx == 0) {
            // VPN / 隧道。先记下来当备选，继续找物理网卡。
            fallbackIdx  = (int64_t)idx;
            fallbackName = name;
            fallbackDesc = buf;
        }
        return true;
    });

    if (osOrder) {
        descOut   = std::move(fallbackDesc);
        ifNameOut = std::move(fallbackName);
        return fallbackIdx;
    }

    if (physicalIdx > 0) {
        descOut   = std::move(physicalDesc);
        ifNameOut = std::move(physicalName);
        return physicalIdx;
    }

    // 没有物理网卡。只有 prefer-physical 的启动查询允许回落到隧道。
    const bool allowTunnelFallback =
        (policy == TT_VPN_POLICY_PREFER_PHYSICAL && isStartupQuery);
    if (!allowTunnelFallback) {
        descOut.clear();
        ifNameOut.clear();
        return 0;
    }
    descOut   = std::move(fallbackDesc);
    ifNameOut = std::move(fallbackName);
    return fallbackIdx;
}
```

- [ ] **Step 2: 更新 update_handler 调用点**

把 `tt_netmon_start` 的 update_handler 里：

```objc
        int64_t ifIndex = ResolveActiveIfIndex(path, desc, ifName,
                                               /*physicalOnly=*/(self->policy != TT_VPN_POLICY_OS));
```

改为：

```objc
        int64_t ifIndex = ResolveActiveIfIndex(path, desc, ifName,
                                               self->policy,
                                               /*isStartupQuery=*/false);
```

同时把它上方那段解释 `bypass_vpn` 的注释改写为指向新的三态语义（不要留下 `bypass_vpn` 这个已不存在的标识符）。

- [ ] **Step 3: 让启动查询走 policy**

`tt_netmon_query_default_ifindex` 内部对 `ResolveActiveIfIndex` 的调用（原本传 `physicalOnly=false`）改为接收参数，并把 `_ex` 变体实现为真正的入口：

```objc
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }
    // ... 原 tt_netmon_query_default_ifindex 的函数体，只是把内部那次
    //     ResolveActiveIfIndex(path, desc, ifName, /*physicalOnly=*/false)
    //     换成 ResolveActiveIfIndex(path, desc, ifName, policy,
    //                               /*isStartupQuery=*/true)
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex((int)tt_vpn_policy_platform_default());
}
```

具体做法：把原 `tt_netmon_query_default_ifindex` 的函数体整体挪进 `_ex`，原函数只留上面那一行委托。

- [ ] **Step 4: 构建 macOS**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
```
Expected: 构建成功，且**没有任何** `bypass_vpn` 相关的 unused-variable 警告（说明旧标识符已清理干净）

- [ ] **Step 5: 构建 iOS 模拟器 slice**

Run:
```bash
build/ios-sim-arm64-release/build 2>&1 | tail -20
```
Expected: 构建成功。这一步是本任务的关键验证——合并分支后 iOS 侧必须仍能编过（原 iOS 分支不引用 `physicalIdx` 等变量）。

- [ ] **Step 6: 三档 policy 的运行时冒烟验证**

Run:
```bash
for p in os prefer-physical force-physical; do
  echo "=== $p ==="
  node -e "
    const ttsignal = require('./src/js/index.js');
    ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: '$p' });
    setTimeout(() => process.exit(0), 1200);
  "
done
```
Expected: 三次都不崩溃。注意此刻 `vpnPolicy` 还没被 `SMPConnector::Config` 解析（Task 8 才做），所以三次的实际行为相同——本步只验证新代码路径在真实 `NWPathMonitor` 回调下不崩。

- [ ] **Step 7: 提交**

```bash
git add src/cpp/apple/AppleNetworkMonitor.mm
git commit -m "feat(apple): ResolveActiveIfIndex 支持三态策略并合并 macOS/iOS 分支

原实现按 #if TARGET_OS_OSX 分成两套重复代码，iOS 分支直接忽略
physicalOnly。合并为一份 policy 驱动的实现后，iOS 自动获得
physical-first 能力（默认值仍为 os，行为不变）。回落隧道的时机
收敛为'仅 prefer-physical 的启动查询'。"
```

---

### Task 6: Windows monitor 实现 physical-first

**⚠️ 本任务无法在 macOS 上编译验证。** 需要 Windows 机器跑 `scripts/build-for-win.bat`。若手头没有，交付代码并**明确标注未验证**。

**Files:**
- Modify: `src/cpp/win32/WinIpChangeMonitor.cpp:89-107`（`QueryBestIfIndex`）、`:299`（query 函数）、`WorkerLoop` 中的调用点

**Interfaces:**
- Consumes: Task 4 的 `Monitor::policy`
- Produces: `static int64_t QueryBestIfIndex(TTVpnPolicy policy)`（原无参版本被替换）

- [ ] **Step 1: 新增物理网卡判定辅助函数**

在 `WinIpChangeMonitor.cpp` 的匿名 namespace 里、`QueryBestIfIndex` 之前插入：

```cpp
// 判断一块网卡是不是真实硬件。
//
// 只看 MIB_IF_ROW2::Type 是不够的：TAP-Windows（OpenVPN）和 wintun
// （WireGuard / Clash / mihomo）都把自己上报成 IF_TYPE_ETHERNET_CSMACD，
// 光看类型会把它们误判成物理网卡。InterfaceAndOperStatusFlags.HardwareInterface
// 是 NDIS 给出的"背后有没有真实硬件"的权威答案，正是我们要的。
static bool IsPhysicalAdapter(NET_IFINDEX ifIndex)
{
    MIB_IF_ROW2 row;
    memset(&row, 0, sizeof(row));
    row.InterfaceIndex = ifIndex;
    if (GetIfEntry2(&row) != NO_ERROR) {
        return false;
    }
    if (row.InterfaceAndOperStatusFlags.HardwareInterface == 0) {
        return false;
    }
    switch (row.Type) {
        case IF_TYPE_TUNNEL:
        case IF_TYPE_PPP:
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return false;
        default:
            break;
    }
    return true;
}

// 枚举所有网卡，返回 metric 最小的那块 up 状态物理网卡。没有则返回 0。
static int64_t QueryBestPhysicalIfIndex()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return 0;
    }
    int64_t  best       = 0;
    ULONG    bestMetric = 0xFFFFFFFFu;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp) continue;
        if (!IsPhysicalAdapter(row.InterfaceIndex)) continue;

        // MIB_IF_ROW2 没有路由 metric，用接口 metric 近似；两块都可用时
        // 这个排序跟 Windows 自己挑默认路由的偏好一致。
        MIB_IPINTERFACE_ROW ipRow;
        memset(&ipRow, 0, sizeof(ipRow));
        ipRow.Family         = AF_INET;
        ipRow.InterfaceIndex = row.InterfaceIndex;
        ULONG metric = 0xFFFFFFFEu;
        if (GetIpInterfaceEntry(&ipRow) == NO_ERROR) {
            metric = ipRow.Metric;
        }
        if (best == 0 || metric < bestMetric) {
            best       = (int64_t)row.InterfaceIndex;
            bestMetric = metric;
        }
    }
    FreeMibTable(table);
    return best;
}
```

确认文件顶部已 include `<netioapi.h>`（`GetIfTable2` / `GetIfEntry2` / `GetIpInterfaceEntry` / `FreeMibTable` 都在这里）。若没有，在 `<iphlpapi.h>` 之后补上。

- [ ] **Step 2: 改写 QueryBestIfIndex**

把原 `static int64_t QueryBestIfIndex()` 改为：

```cpp
// GetBestInterfaceEx 要一个 sockaddr 目的地址。用 INADDR_ANY（0.0.0.0）问
// Windows "到未指定 IPv4 目的地的最佳网卡"，实际等价于"给我默认路由的网卡"。
// 先试 IPv4，失败再试 IPv6 未指定地址。
static int64_t QueryBestIfIndexOsOrder()
{
    struct sockaddr_in dst4 = {};
    dst4.sin_family = AF_INET;
    dst4.sin_addr.s_addr = INADDR_ANY;
    DWORD ifx = 0;
    if (GetBestInterfaceEx((struct sockaddr*)&dst4, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }

    struct sockaddr_in6 dst6 = {};
    dst6.sin6_family = AF_INET6;
    if (GetBestInterfaceEx((struct sockaddr*)&dst6, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }
    return 0;
}

// policy 语义与 AppleNetworkMonitor.mm 的 ResolveActiveIfIndex 一致：
//   os              — 系统默认路由赢家，含 TUN
//   prefer-physical — 默认路由赢家若是物理网卡则直接用（快路径，覆盖绝大多数
//                     无 VPN 场景）；否则枚举物理网卡取 metric 最小者；再没有
//                     才回落到默认路由赢家
//   force-physical  — 同上，但最后一步不回落，返回 0
static int64_t QueryBestIfIndex(TTVpnPolicy policy)
{
    const int64_t osBest = QueryBestIfIndexOsOrder();
    if (policy == TT_VPN_POLICY_OS) {
        return osBest;
    }
    if (osBest > 0 && IsPhysicalAdapter((NET_IFINDEX)osBest)) {
        return osBest;
    }
    const int64_t phys = QueryBestPhysicalIfIndex();
    if (phys > 0) {
        return phys;
    }
    return (policy == TT_VPN_POLICY_FORCE_PHYSICAL) ? 0 : osBest;
}
```

- [ ] **Step 3: 更新所有调用点**

`QueryBestIfIndex()` 在本文件里有两处调用（`WorkerLoop` 内的循环，以及 `tt_netmon_start` 里的"initial fire"）。用下面的命令定位并逐一改为 `QueryBestIfIndex(self->policy)`：

```bash
grep -n "QueryBestIfIndex()" src/cpp/win32/WinIpChangeMonitor.cpp
```

- [ ] **Step 4: 实现 _ex 查询函数**

把 Task 4 留下的占位实现替换为：

```cpp
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }
    return QueryBestIfIndex(policy);
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex((int)tt_vpn_policy_platform_default());
}
```

原 `tt_netmon_query_default_ifindex` 的旧函数体（若与 `QueryBestIfIndex` 重复）一并删除。

- [ ] **Step 5: 在 Windows 机器上构建**

Run（在 Windows 上）：
```cmd
scripts\build-for-win.bat
```
Expected: 构建成功，产出 `.node`

**若没有 Windows 环境**：跳过本步，并在提交信息与向用户的汇报中**明确写出"Windows 构建与运行均未验证"**。不要因为代码看起来正确就声称通过。

- [ ] **Step 6: 提交**

```bash
git add src/cpp/win32/WinIpChangeMonitor.cpp
git commit -m "feat(win): 路径监视器支持 physical-first 选网卡

原 QueryBestIfIndex 只有一条 GetBestInterfaceEx(0.0.0.0)，不区分接口
类型，TUN 抢了默认路由就选 TUN。新增基于
MIB_IF_ROW2.InterfaceAndOperStatusFlags.HardwareInterface 的物理网卡
判定——TAP-Windows 与 wintun 都上报 IF_TYPE_ETHERNET_CSMACD，只看
Type 会误判。

注意：Windows 默认值随之从 os 变为 prefer-physical，属有意引入的
默认行为变更，需写入 release note。"
```

---

### Task 7: Linux monitor 接入 policy 与 sysfs 判定

**Files:**
- Modify: `src/cpp/linux/LinuxNetlinkMonitor.cpp:80`（删除 `IsVirtualIface`）、`:107-230`（`QueryDefaultIfIndex`）、`:341`、`:444`

**Interfaces:**
- Consumes: Task 3 的 `tt_linux_iface_is_physical`、Task 4 的 `Monitor::policy`
- Produces: `static int64_t QueryDefaultIfIndex(int family, TTVpnPolicy policy)`（原签名只有 `family`）

- [ ] **Step 1: 换掉 IsVirtualIface**

在 `LinuxNetlinkMonitor.cpp` 顶部 include 区加入：

```cpp
#include "LinuxPhysicalIface.h"
```

删除整个 `static bool IsVirtualIface(const char* name)` 函数及其上方的注释块——它的职责已经完整迁移到 `LinuxPhysicalIface.cpp`（含同一份名字黑名单作为兜底）。

- [ ] **Step 2: 让 QueryDefaultIfIndex 接收 policy**

把签名 `static int64_t QueryDefaultIfIndex(int family)` 改为 `static int64_t QueryDefaultIfIndex(int family, TTVpnPolicy policy)`。

在 RTA_OIF 解析处，把：

```cpp
                if (attr->rta_type == RTA_OIF) {
                    int oif = *(int*)RTA_DATA(attr);
                    char name[IF_NAMESIZE] = {0};
                    if_indextoname(oif, name);
                    if (IsVirtualIface(name)) continue;
                    found = (int64_t)oif;
                    goto done;
                }
```

改为：

```cpp
                if (attr->rta_type == RTA_OIF) {
                    int oif = *(int*)RTA_DATA(attr);
                    char name[IF_NAMESIZE] = {0};
                    if_indextoname(oif, name);
                    // os 策略下不过滤，默认路由赢家是谁就是谁（含 tun /
                    // wireguard）。prefer / force 两档跳过虚拟网卡，继续在
                    // 后续的 RTM_NEWROUTE 里找物理网卡的默认路由。
                    if (policy != TT_VPN_POLICY_OS &&
                        !tt_linux_iface_is_physical(name)) {
                        // 记下第一个虚拟网卡备选，prefer 档在找不到物理网卡时
                        // 回落到它——否则 VPN-only 的机器会拿到 0，socket 完全
                        // 不绑定，反而白白把包交给内核路由送进隧道。
                        if (fallback == 0) fallback = (int64_t)oif;
                        continue;
                    }
                    found = (int64_t)oif;
                    goto done;
                }
```

在函数开头 `found` 声明旁边加上 `int64_t fallback = 0;`，并在 `done:` 标签之后、`return` 之前插入：

```cpp
    if (found == 0 && policy == TT_VPN_POLICY_PREFER_PHYSICAL) {
        // 只有 prefer 档回落。force 档宁可返回 0，让 SMPConnection::Connect
        // 以 BC_R_NO_PHYSICAL_INTERFACE 明确失败。
        found = fallback;
    }
```

- [ ] **Step 3: 更新所有调用点**

Run:
```bash
grep -n "QueryDefaultIfIndex(" src/cpp/linux/LinuxNetlinkMonitor.cpp
```

把每一处调用改为传入 policy：`WorkerLoop` 与 `tt_netmon_start` 里的调用传 `self->policy`。

- [ ] **Step 4: 实现 _ex 查询函数**

替换 Task 4 留下的占位：

```cpp
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }
    int64_t idx = QueryDefaultIfIndex(AF_INET, policy);
    if (idx <= 0) {
        idx = QueryDefaultIfIndex(AF_INET6, policy);
    }
    return idx;
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex((int)tt_vpn_policy_platform_default());
}
```

若原 `tt_netmon_query_default_ifindex` 的函数体与上面重复，删掉重复部分。

- [ ] **Step 5: 交叉编译 Linux**

Run:
```bash
bash build/linux-x64-release/build 2>&1 | tail -20
bash build/linux-arm64-release/build 2>&1 | tail -20
```
Expected: 两个架构都构建成功

- [ ] **Step 6: 提交**

```bash
git add src/cpp/linux/LinuxNetlinkMonitor.cpp
git commit -m "feat(linux): 路径监视器接入 vpnPolicy 与 sysfs 物理网卡判定

IsVirtualIface 原本无条件生效且不受任何开关控制，现改为受 policy
控制并复用 LinuxPhysicalIface 的 sysfs 判定。同时补上 prefer 档的
隧道回落——原实现只有 tun 时返回 0，socket 干脆不绑定，反而让内核
把包直接送进隧道。"
```

---

### Task 8: SMPConnector 解析配置并下发

**Files:**
- Modify: `src/cpp/SMPConnector.h:451-458`（Config 构造）、`:501`（字段声明）
- Modify: `src/cpp/SMPConnector.cpp:2671`（`Config::Init`）、`:3046-3065`（`tt_netmon_start`）

**Interfaces:**
- Consumes: Task 1 的全部 4 个函数、Task 4 的 `TTNetworkMonitorOptions::vpnPolicy`
- Produces: `SMPConnector::Config::vpn_policy`（类型 `TTVpnPolicy`），Task 9/10 消费

- [ ] **Step 1: 加配置字段**

`src/cpp/SMPConnector.h`：

在文件顶部 include 区加入 `#include "VpnPolicy.h"`（若 `INetworkPathMonitor.h` 已被包含则可省略，但显式写出更清晰）。

在 `Config` 构造函数初始化列表里，`, bypassVpn(true)` 之后追加：

```cpp
            , vpn_policy(TT_VPN_POLICY_UNSET)
```

在 `bool bypassVpn;` 字段声明之后追加：

```cpp
        // 三态 VPN / 虚拟网卡策略。Config::Init 里由 "vpnPolicy" 字符串键与
        // 旧的 "bypassVpn" 布尔键合并得出，UNSET 表示两者都没配、应回落到
        // 平台默认值。语义见 VpnPolicy.h。
        //
        // bypassVpn 字段本身保留但已废弃，只在兼容映射时读取一次。
        TTVpnPolicy                 vpn_policy;
```

- [ ] **Step 2: 解析配置键**

`src/cpp/SMPConnector.cpp` 的 `Config::Init`，把：

```cpp
    pVar = pConfig->Get("bypassVpn");
    if (IS_BCF_BOOL(pVar))
    {
        bypassVpn = GET_BCF_BOOL(pVar);
    }
```

替换为：

```cpp
    // 旧键 bypassVpn（已废弃）与新键 vpnPolicy 合并。两者都给时新键胜出。
    bool hasBypassVpn = false;
    pVar = pConfig->Get("bypassVpn");
    if (IS_BCF_BOOL(pVar))
    {
        bypassVpn    = GET_BCF_BOOL(pVar);
        hasBypassVpn = true;
    }
    TTVpnPolicy explicitPolicy = TT_VPN_POLICY_UNSET;
    pVar = pConfig->Get("vpnPolicy");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR raw = GET_BCF_STRING(pVar);
        explicitPolicy = tt_vpn_policy_from_string(raw);
        if (explicitPolicy == TT_VPN_POLICY_UNSET && raw && raw[0])
        {
            LogQ(NULL, _WARN_,
                 "[SMPConnector] 无法识别的 vpnPolicy=\"%s\"，"
                 "已忽略；合法取值为 os / prefer-physical / force-physical",
                 raw);
        }
    }
    int bothGiven = 0;
    vpn_policy = tt_vpn_policy_resolve(explicitPolicy,
                                       hasBypassVpn ? 1 : 0,
                                       bypassVpn ? 1 : 0,
                                       tt_vpn_policy_platform_default(),
                                       &bothGiven);
    if (bothGiven)
    {
        LogQ(NULL, _WARN_,
             "[SMPConnector] 同时配置了 vpnPolicy 与已废弃的 bypassVpn，"
             "以 vpnPolicy=%s 为准，bypassVpn 被忽略",
             tt_vpn_policy_to_string(vpn_policy));
    }
```

`Config::Init` 是否能拿到 logger context 需要现场确认：用 `grep -n "LogQ" src/cpp/SMPConnector.cpp | sed -n '1,5p'` 看同文件里 `Config::Init` 内部（若有）既有的 LogQ 调用形式，照抄第一个参数。如果 `Config::Init` 里完全没有 LogQ 先例，就把这两条告警移到 `SMPConnector::Create` 中 `config_.Init(pConfig)` 之后，用 `logger_ctx_` 作为第一个参数——**不要**传 `NULL` 除非确认该函数接受 NULL。

- [ ] **Step 3: 传给路径监视器**

把 `SMPConnector::Create` 里：

```cpp
        TTNetworkMonitorOptions netmon_opts{};
        netmon_opts.bypassVpn = config_.bypassVpn ? 1 : 0;
```

替换为：

```cpp
        TTNetworkMonitorOptions netmon_opts{};
        // vpnPolicy 是权威字段；bypassVpn 仍然填上，让链接到旧版本 monitor
        // 实现的场景（理论上不该出现，但结构体是公开 ABI）仍有合理行为。
        netmon_opts.vpnPolicy = (int)config_.vpn_policy;
        netmon_opts.bypassVpn =
            (config_.vpn_policy == TT_VPN_POLICY_OS) ? 0 : 1;
```

把成功日志：

```cpp
            LogQ(logger_ctx_, _INFO_,
                 "[SMPConnector] tt_netmon_start ok bypassVpn=%d",
                 netmon_opts.bypassVpn);
```

改为：

```cpp
            LogQ(logger_ctx_, _INFO_,
                 "[SMPConnector] tt_netmon_start ok vpnPolicy=%s",
                 tt_vpn_policy_to_string(config_.vpn_policy));
```

- [ ] **Step 4: 构建**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
```
Expected: 构建成功

- [ ] **Step 5: 验证配置真的生效**

Run:
```bash
for p in os prefer-physical force-physical; do
  echo "=== vpnPolicy=$p ==="
  node -e "
    const ttsignal = require('./src/js/index.js');
    ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: '$p' });
    setTimeout(() => process.exit(0), 1200);
  " 2>&1 | grep -i "vpnPolicy" || echo "(日志里没出现 vpnPolicy，检查日志级别)"
done
echo "=== 未配置（应为平台默认 prefer-physical）==="
node -e "
  const ttsignal = require('./src/js/index.js');
  ttsignal.createConnector({ alpn: 'ttsignal' });
  setTimeout(() => process.exit(0), 1200);
" 2>&1 | grep -i "vpnPolicy"
echo "=== 旧键兼容：bypassVpn=false 应映射为 os ==="
node -e "
  const ttsignal = require('./src/js/index.js');
  ttsignal.createConnector({ alpn: 'ttsignal', bypassVpn: false });
  setTimeout(() => process.exit(0), 1200);
" 2>&1 | grep -i "vpnPolicy"
echo "=== 冲突告警：两者同时给 ==="
node -e "
  const ttsignal = require('./src/js/index.js');
  ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: 'force-physical', bypassVpn: false });
  setTimeout(() => process.exit(0), 1200);
" 2>&1 | grep -iE "vpnPolicy|废弃"
echo "=== 非法取值告警 ==="
node -e "
  const ttsignal = require('./src/js/index.js');
  ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: 'physical' });
  setTimeout(() => process.exit(0), 1200);
" 2>&1 | grep -iE "无法识别|vpnPolicy"
```

Expected：
- 三档各自打印对应的 `vpnPolicy=<值>`
- 未配置时打印 `vpnPolicy=prefer-physical`
- `bypassVpn: false` 打印 `vpnPolicy=os`
- 冲突时既打印 `vpnPolicy=force-physical` 又出现"被忽略"告警
- 非法取值出现"无法识别"告警且回落到 `prefer-physical`

若日志没打到 stdout，用 `grep -rn "log_file" src/js/index.js` 找到日志文件位置后改成 `grep` 该文件。

- [ ] **Step 6: 提交**

```bash
git add src/cpp/SMPConnector.h src/cpp/SMPConnector.cpp
git commit -m "feat(connector): 解析 vpnPolicy 配置键并下发给路径监视器

新键 vpnPolicy 与已废弃的 bypassVpn 合并，两者同时出现时新键胜出并
告警；无法识别的取值告警后回落平台默认值。tt_netmon_start 的日志从
打印 bypassVpn 改为打印生效的 policy 名。"
```

---

### Task 9: UDPSender 在 force-physical 下抑制解绑

这是修复用户原始问题的关键一步。`UDPSender::Connect` 的路由表校验会在 TUN 抢走默认路由时把 `IP_BOUND_IF` 撤掉，让流量重回 VPN——`force-physical` 必须阻止它。

**Files:**
- Modify: `src/cpp/UDPSender.h`（新增 `SetVpnPolicy` 与成员）
- Modify: `src/cpp/UDPSender.cpp:314-380`（`Connect` 的主动 unpin）、`:1303-1330`（`_OnConnectDone` 的被动 unpin）
- Modify: `src/cpp/UDPSenderGroup.h` / `.cpp`（转发）
- Modify: `src/cpp/SMPConnector.cpp:1014`（`SMPConnection::Create` 里调用）

**Interfaces:**
- Consumes: Task 1 的 `TTVpnPolicy`、Task 8 的 `SMPConnector::Config::vpn_policy`
- Produces:
  - `void UDPSender::SetVpnPolicy(TTVpnPolicy policy);`
  - `void UDPSenderGroup::SetVpnPolicy(TTVpnPolicy policy);`

- [ ] **Step 1: UDPSender 加 setter 与成员**

`src/cpp/UDPSender.h`：顶部 include 区加 `#include "VpnPolicy.h"`。

在 public 方法区（`GetSockName` 附近）加入：

```cpp
	// 设置生效的 VPN 策略。必须在 Connect() 之前调用；SMPConnection::Create
	// 在 UDPSenderGroup::Create 成功后立刻调用，此时 socket 已建好但还没
	// connect，正是时机。
	//
	// TT_VPN_POLICY_FORCE_PHYSICAL 下，本对象拒绝撤销已有的网卡绑定——
	// 无论是 Connect() 里的路由表主动校验，还是 _OnConnectDone 收到
	// ENETUNREACH 的被动回收。业务显式要求"必须走物理网卡"，那么"连不上"
	// 就是正确结果，静默回落到 VPN 才是错的。
	void			SetVpnPolicy(TTVpnPolicy policy) { m_eVpnPolicy = policy; }
```

在私有成员区（`m_nNetworkHandle` 附近）加入：

```cpp
	// 生效的 VPN 策略，默认 UNSET（等同于不抑制解绑，保持历史行为）。
	TTVpnPolicy				m_eVpnPolicy = TT_VPN_POLICY_UNSET;
```

- [ ] **Step 2: 抑制 Connect() 的主动解绑**

`src/cpp/UDPSender.cpp` 的 `Connect()`，在 `if (mismatch || fake_ip_fallback)` 块内、任何 `_TryClearInterfaceBinding` 调用**之前**插入：

```cpp
			if (m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL)
			{
				char ipbuf2[INET6_ADDRSTRLEN] = {0};
				const void* ip_src2 = (refSockAddr.type.sa.sa_family == AF_INET6)
					? static_cast<const void*>(&refSockAddr.type.sin6.sin6_addr)
					: static_cast<const void*>(&refSockAddr.type.sin.sin_addr);
				inet_ntop(refSockAddr.type.sa.sa_family, ip_src2,
					ipbuf2, sizeof(ipbuf2));
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: 内核认为对端 %s 应走 ifIndex=%u，与我们绑定的 "
					"ifIndex=%lld 不一致；vpnPolicy=force-physical，保持绑定不解除。"
					"若对端确实只能经 VPN 到达，本连接会超时失败——这是该策略的"
					"预期行为，改用 prefer-physical 可恢复自动回落。",
					ipbuf2, natural_idx, (long long)m_nNetworkHandle);
				// 不调用 _TryClearInterfaceBinding，直接跳过整个解绑分支。
			}
			else
			{
```

并在该分支原有内容（三个平台的 `#if` 分支 + `_TryClearInterfaceBinding` 调用）之后补上对应的右花括号 `}`。

**注意**：原代码里 `ipbuf` 变量是在 `if (mismatch || fake_ip_fallback)` 块开头声明的，插入的新分支在它之后，所以复用 `ipbuf` 也可以——上面用 `ipbuf2` 是为了让这段插入不依赖原代码的变量位置。实施时读一遍现场代码，若 `ipbuf` 已在作用域内且已填好值，直接复用并删掉 `ipbuf2` 那几行。

- [ ] **Step 3: 抑制 _OnConnectDone 的被动解绑**

把 `_OnConnectDone` 里：

```cpp
	if (result == BC_R_NETUNREACH || result == BC_R_HOSTUNREACH ||
		result == BC_R_ADDRNOTAVAIL)
	{
		_TryClearInterfaceBinding(result);
	}
```

改为：

```cpp
	if (result == BC_R_NETUNREACH || result == BC_R_HOSTUNREACH ||
		result == BC_R_ADDRNOTAVAIL)
	{
		if (m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL)
		{
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: connect 失败 result=%u (%s) 且已绑定 ifIndex=%lld，"
				"但 vpnPolicy=force-physical，不解除绑定。错误如实上抛给业务。",
				(unsigned)result, BC::bc_result2string(result),
				(long long)m_nNetworkHandle);
		}
		else
		{
			_TryClearInterfaceBinding(result);
		}
	}
```

- [ ] **Step 4: UDPSenderGroup 转发**

`src/cpp/UDPSenderGroup.h`：顶部加 `#include "VpnPolicy.h"`，在 public 区（`GetSockName` 附近）加：

```cpp
    // 把策略转发给组内每一个 sender。见 UDPSender::SetVpnPolicy。
    void        SetVpnPolicy(TTVpnPolicy policy);
```

`src/cpp/UDPSenderGroup.cpp` 加实现：

```cpp
void UDPSenderGroup::SetVpnPolicy(TTVpnPolicy policy)
{
    for (auto& entry : senders_)
    {
        if (entry.sender)
        {
            entry.sender->SetVpnPolicy(policy);
        }
    }
}
```

- [ ] **Step 5: SMPConnection::Create 下发**

`src/cpp/SMPConnector.cpp`，在：

```cpp
    result = udp_socket_->Create(connector->logger_ctx_, pTaskMgr, pTimerMgr, 
        Runtime::SocketMgr(), pConfig, this, false, false);
    if (result != BC_R_SUCCESS)
    {
        goto delete_socket;
    }
```

之后插入：

```cpp
    // 策略是 connector 级配置，但抑制解绑发生在每条连接的 UDPSender 里，
    // 所以这里显式下发。必须在 Connect() 之前——此刻 socket 已建好、尚未
    // connect，正是时机。
    udp_socket_->SetVpnPolicy(connector->config_.vpn_policy);
```

`connector` 是该函数已有的形参名——插入前用 `grep -n "connector->logger_ctx_" src/cpp/SMPConnector.cpp` 确认这一行附近用的确实是 `connector` 而不是 `connector_`。

- [ ] **Step 6: 构建**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
```
Expected: 构建成功

- [ ] **Step 7: 回归验证（无 VPN 环境）**

Run:
```bash
node js/server.js &
SERVER_PID=$!
sleep 2
node js/client.js 2>&1 | tail -20
kill $SERVER_PID 2>/dev/null
```
Expected: 客户端正常连上并完成收发。这一步确认抑制逻辑没有在正常路径上误伤——无 VPN 时根本不会触发 mismatch，行为应与改动前完全一致。

若 `js/client.js` / `js/server.js` 需要额外参数或证书，先 `head -30 js/server.js` 看用法。

- [ ] **Step 8: 提交**

```bash
git add src/cpp/UDPSender.h src/cpp/UDPSender.cpp \
        src/cpp/UDPSenderGroup.h src/cpp/UDPSenderGroup.cpp src/cpp/SMPConnector.cpp
git commit -m "feat(udp): force-physical 下拒绝撤销网卡绑定

这是'服务端拿到 VPN 出口 IP'的直接成因：Connect() 的路由表校验发现
内核要走 utun 而我们钉了 en0 时，会主动 setsockopt(IP_BOUND_IF, 0)，
流量立刻回到 VPN。_OnConnectDone 的 ENETUNREACH 回收路径同理。

force-physical 下两条路径都改为只告警、不解绑，错误如实上抛。其余
策略行为不变。"
```

---

### Task 10: force-physical 无物理网卡时快速失败

**Files:**
- Modify: `src/cpp/SMPConnector.cpp`（`SMPConnection::Connect`）

**Interfaces:**
- Consumes: Task 2 的 `BC_R_NO_PHYSICAL_INTERFACE`、Task 4/5/6/7 的 `tt_netmon_query_default_ifindex_ex`、Task 8 的 `config_.vpn_policy`
- Produces: 无新接口；`Connect` 在该场景下返回 `BC_R_NO_PHYSICAL_INTERFACE`

- [ ] **Step 1: 定位 Connect 入口**

Run:
```bash
grep -n "BCRESULT SMPConnection::Connect" src/cpp/SMPConnector.cpp
```

记下行号。下一步在该函数**参数校验之后、任何网络动作之前**插入检查。

- [ ] **Step 2: 插入快速失败检查**

在 `SMPConnection::Connect` 的参数校验之后插入：

```cpp
#if defined(TT_HAS_PATH_MONITOR)
    // force-physical：没有可用物理网卡就不发包，直接以专属错误码失败。
    //
    // 让业务在一次回调里就能决策（降级到 prefer-physical 重连，或提示用户
    // 关掉 VPN），而不是等一个分不清"网络慢"还是"被策略挡住"的通用超时。
    if (connector_->config_.vpn_policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        int64_t phys = tt_netmon_query_default_ifindex_ex(
            (int)TT_VPN_POLICY_FORCE_PHYSICAL);
        if (phys <= 0)
        {
            // 把系统当前看到的默认出口一并打出来，用于区分"真的没有物理网卡"
            // 和"物理网卡判定误判"——三平台的判定实现各不相同，这是最主要的
            // 现场排查线索。
            int64_t osBest = tt_netmon_query_default_ifindex_ex(
                (int)TT_VPN_POLICY_OS);
            LogQ(connector_->logger_ctx_, _ERROR_,
                 "[SMPConnection] vpnPolicy=force-physical 但找不到可用物理网卡"
                 "（系统默认出口 ifIndex=%lld）。连接不发起，返回 "
                 "BC_R_NO_PHYSICAL_INTERFACE。常见原因：只挂了 VPN 而 Wi-Fi/"
                 "有线均未连接。",
                 (long long)osBest);
            return BC_R_NO_PHYSICAL_INTERFACE;
        }
    }
#endif
```

`connector_` 与 `logger_ctx_` 的确切名字需现场确认：`SMPConnection` 内部访问 connector 配置的既有写法见 `SMPConnector.cpp:1327` 的 `connector_->config_.ipv6`，照抄该形式。

- [ ] **Step 3: 确认错误码能传到业务层**

Run:
```bash
grep -n "m_result" src/cpp/apple/ios_bridge.mm | head -3
grep -rn "OnConnectResult" src/cpp/napi/*.cpp | head -3
```
Expected: 确认 iOS 侧 `on_connect_result(userdata_, (int32_t)pStub->m_result, msg)` 直接透传 BCRESULT；NAPI 侧同样透传。若发现某一侧对错误码做了白名单映射，需要在那里补上新码——**这一步不能跳过，否则新错误码在业务侧会变成"未知错误"**。

- [ ] **Step 4: 构建**

Run:
```bash
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j8 2>&1 | tail -20
```
Expected: 构建成功

- [ ] **Step 5: 验证正常网络下不误伤**

Run:
```bash
node js/server.js &
SERVER_PID=$!
sleep 2
node -e "
const ttsignal = require('./src/js/index.js');
const c = ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: 'force-physical' });
const conn = c.createConnection({ alpn: 'h3' });
conn.connect('https://127.0.0.1:8443/', {}, 5000, (err) => {
  console.log('connect result:', err);
  process.exit(err ? 1 : 0);
});
"
kill $SERVER_PID 2>/dev/null
```
Expected: 有物理网卡时连接成功（`connect result: 0` 或等价的成功值）。URL 与端口需与 `js/server.js` 实际监听的一致，先 `grep -n "8443\|port" js/server.js | head` 确认。

**注意**：连的是 `127.0.0.1`，走 loopback。若发现 force-physical 把 loopback 目标也挡住了，说明快速失败检查放得过早——它检查的是"有没有物理网卡"而非"对端是否可达"，本机有 Wi-Fi 时不该失败。若确实失败，记录下来作为已知限制反馈，不要为了让测试过而削弱检查。

- [ ] **Step 6: 提交**

```bash
git add src/cpp/SMPConnector.cpp
git commit -m "feat(connector): force-physical 无物理网卡时快速失败

不发包、不等 connect timeout，直接返回 BC_R_NO_PHYSICAL_INTERFACE，
让业务在一次回调里就能决策。错误日志同时打出系统当前默认出口
ifIndex，用于区分'真的没有物理网卡'和'物理网卡判定误判'。"
```

---

### Task 11: Linux 用 SO_BINDTODEVICE 硬绑物理网卡

`UDPSender.cpp` 自己的注释已写明：`IP_UNICAST_IF` 在 kernel < 6.0.16 / 6.1.2 / 6.2 上对已 `connect()` 的 UDP socket 被静默忽略。`force-physical` 在这些内核上等于没生效，必须换手段。

**Files:**
- Modify: `src/cpp/UDPSender.cpp`（`_InitSocket` 的 Linux 分支）

**Interfaces:**
- Consumes: Task 9 的 `m_eVpnPolicy`
- Produces: 无新接口

- [ ] **Step 1: 定位 Linux 绑定分支**

Run:
```bash
grep -n "IP_UNICAST_IF" src/cpp/UDPSender.cpp
```

找到 `_InitSocket` 里（不是 `_TryClearInterfaceBinding` 里）执行 `setsockopt(..., IP_UNICAST_IF, ...)` 的那一处 Linux 分支。

- [ ] **Step 2: 在该分支前插入 SO_BINDTODEVICE 尝试**

```cpp
#if defined(__linux__) && !defined(OS_ANDROID)
	// force-physical 下优先用 SO_BINDTODEVICE。
	//
	// 这是 Linux 上唯一能真正保证"包从这块网卡出去"的手段。IP_UNICAST_IF
	// 在 kernel < 6.0.16 / 6.1.2 / 6.2 上对已 connect 的 UDP socket 被静默
	// 忽略（路由在 connect 时被缓存，缓存绕过 fib_lookup），等于没绑。
	//
	// 而"只绑源 IP"是不够的：路由仍走 tun 时，包带着物理网卡的源 IP 进入
	// VPN，会被 NAT 改写或直接丢弃——服务端照样看不到真实 IP。
	//
	// SO_BINDTODEVICE 需要 CAP_NET_RAW（或 root）。没权限时回落到
	// IP_UNICAST_IF 并告警：此时 force-physical 只能保证"失败可见"，
	// 不能保证"拿到真实 IP"。
	bool bound_to_device = false;
	if (m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL && ifIndex > 0)
	{
		char ifname[IF_NAMESIZE] = {0};
		if (if_indextoname((unsigned int)ifIndex, ifname) != NULL)
		{
			int r = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
				ifname, (socklen_t)strlen(ifname));
			if (r == 0)
			{
				bound_to_device = true;
				LogQ(m_pLoggerCtx, _INFO_,
					"UDP Sender: setsockopt(SO_BINDTODEVICE, %s) 成功，"
					"force-physical 已硬绑物理网卡", ifname);
			}
			else
			{
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: setsockopt(SO_BINDTODEVICE, %s) 失败 errno=%d"
					"（缺 CAP_NET_RAW / 非 root）。回落 IP_UNICAST_IF——在 "
					"kernel < 6.0.16/6.1.2/6.2 上该选项对已 connect 的 UDP "
					"socket 无效，TUN 全局代理下 force-physical 可能拿不到"
					"真实 IP，只能保证失败可见。",
					ifname, errno);
			}
		}
	}
	if (!bound_to_device)
	{
		// 原有的 IP_UNICAST_IF 逻辑
	}
#endif
```

把原有的 `IP_UNICAST_IF` `setsockopt` 调用整体挪进 `if (!bound_to_device)` 块内。变量名 `fd` / `ifIndex` 需与该函数现场的实际变量名对齐——先完整读一遍 `_InitSocket` 再动手。

确认文件顶部已 include `<net/if.h>`（`if_indextoname` / `IF_NAMESIZE`）。

- [ ] **Step 3: 交叉编译 Linux**

Run:
```bash
bash build/linux-x64-release/build 2>&1 | tail -20
bash build/linux-arm64-release/build 2>&1 | tail -20
```
Expected: 两个架构都构建成功

- [ ] **Step 4: 确认 macOS 构建未受影响**

Run:
```bash
cd build && make -j8 2>&1 | tail -10
```
Expected: 构建成功（新代码全在 `#if defined(__linux__)` 内，macOS 应完全不受影响）

- [ ] **Step 5: 提交**

```bash
git add src/cpp/UDPSender.cpp
git commit -m "feat(linux): force-physical 优先用 SO_BINDTODEVICE 硬绑网卡

IP_UNICAST_IF 在 kernel < 6.0.16/6.1.2/6.2 上对已 connect 的 UDP
socket 被静默忽略，force-physical 在这些内核上形同虚设。改为优先
SO_BINDTODEVICE（需 CAP_NET_RAW），失败则回落并明确告警：无权限时
该策略只能保证失败可见，不能保证拿到真实 IP。"
```

---

### Task 12: NAPI 文档与入参校验

**Files:**
- Modify: `src/js/index.js:995-1040`（`createConnector` JSDoc 与函数体）

**Interfaces:**
- Consumes: Task 8 已实现的原生侧解析
- Produces: JS 层对非法 `vpnPolicy` 取值的早失败

- [ ] **Step 1: 加 JSDoc**

在 `src/js/index.js` 的 `createConnector` JSDoc 中，`@param config.disableAutoRestart` 之后插入：

```js
 * @param config.vpnPolicy {String=} optional, one of `'os'`,
 *   `'prefer-physical'`, `'force-physical'`. Controls whether QUIC traffic is
 *   allowed to ride a VPN / virtual interface (utun on macOS/iOS, tun on
 *   Linux, wintun/TAP on Windows).
 *
 *   - `'os'` — follow the kernel's routing decision, including VPN tunnels.
 *   - `'prefer-physical'` — prefer wifi/wired/cellular. At startup, fall back
 *     to a tunnel if no physical interface exists; while running, refuse to
 *     migrate onto a tunnel (keep the current socket instead).
 *   - `'force-physical'` — physical interfaces only. Never fall back, and
 *     never undo the interface pin even when the kernel routing table says
 *     the peer is reachable only through the tunnel. Use this when the server
 *     must observe the client's real IP rather than the VPN egress IP.
 *
 *   Defaults: `'prefer-physical'` on macOS / Windows / Linux, `'os'` on iOS.
 *
 *   With `'force-physical'`, `connection.connect()` fails immediately with
 *   `BC_R_NO_PHYSICAL_INTERFACE` (64) when no physical interface is
 *   available — it does NOT wait for the connect timeout. Handle that error
 *   by either reconnecting with `'prefer-physical'` or telling the user to
 *   turn off their VPN.
 *
 *   Linux caveat: only `SO_BINDTODEVICE` can truly guarantee packets leave
 *   via the physical NIC, and it requires `CAP_NET_RAW` (or root). Without
 *   that capability the addon falls back to `IP_UNICAST_IF`, which is
 *   silently ignored on kernels older than 6.0.16 / 6.1.2 / 6.2 for connected
 *   UDP sockets. On such hosts under a full-tunnel proxy, `'force-physical'`
 *   guarantees only that failures are visible, not that the real IP gets
 *   through. A warning is logged when this fallback happens.
 * @param config.bypassVpn {Boolean=} **deprecated**, use `vpnPolicy` instead.
 *   Mapped as `false` -> `'os'`, `true` -> `'prefer-physical'`. When both are
 *   given, `vpnPolicy` wins and a warning is logged.
```

- [ ] **Step 2: 加入参校验**

在 `createConnector` 函数体里，`if (!config.alpn){ ... }` 之后插入：

```js
    if (config.vpnPolicy !== undefined) {
        var VALID_VPN_POLICIES = ['os', 'prefer-physical', 'force-physical'];
        if (VALID_VPN_POLICIES.indexOf(config.vpnPolicy) === -1) {
            throw Error('Invalid vpnPolicy: ' + config.vpnPolicy +
                '. Expected one of ' + VALID_VPN_POLICIES.join(', ') + '.');
        }
    }
```

在 JS 层就抛，比让原生层告警后静默回落平台默认值更容易被开发者发现拼写错误。

- [ ] **Step 3: 验证校验生效**

Run:
```bash
node -e "
const ttsignal = require('./src/js/index.js');
try {
  ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: 'physical' });
  console.log('FAIL: 非法取值没有抛错');
  process.exit(1);
} catch (e) {
  console.log('OK 抛错:', e.message);
}
ttsignal.createConnector({ alpn: 'ttsignal', vpnPolicy: 'force-physical' });
console.log('OK 合法取值通过');
ttsignal.createConnector({ alpn: 'ttsignal' });
console.log('OK 不传也通过');
setTimeout(() => process.exit(0), 800);
"
```
Expected: 三行 OK，无 FAIL

- [ ] **Step 4: 提交**

```bash
git add src/js/index.js
git commit -m "docs(napi): createConnector 增加 vpnPolicy 文档与入参校验

JS 层直接对非法取值抛错，比让原生层告警后静默回落更容易发现拼写
错误。文档写明三档语义、各平台默认值、force-physical 的专属错误码，
以及 Linux 无 CAP_NET_RAW 时无法保证真实 IP 的限制。"
```

---

### Task 13: iOS bridge 与 Swift 绑定

**Files:**
- Modify: `src/cpp/apple/ios_bridge.h:95`（`TTConfig` 末尾）
- Modify: `src/cpp/apple/ios_bridge.mm:114`（写入 BCFObject）
- Modify: `src/swift/TTSignalConfig.swift`

**Interfaces:**
- Consumes: Task 1 的 `TTVpnPolicy` 编码、Task 8 的 `vpnPolicy` 配置键
- Produces:
  - `TTConfig.vpnPolicy`（`int32_t`，`-1/0/1/2`）
  - `public enum TTSignalVPNPolicy`、`TTSignalConfig.vpnPolicy`

- [ ] **Step 1: 扩展 TTConfig**

在 `src/cpp/apple/ios_bridge.h` 的 `TTConfig` 结构体**末尾**（`int32_t bypassVpn;` 之后、右花括号之前）追加：

```c
    // 三态 VPN / 虚拟网卡策略，取代 bypassVpn。
    //   -1 未设置（用平台默认值：iOS 是 os）
    //    0 os              — 跟随系统路由，允许走 VPN
    //    1 prefer-physical — 优先物理网卡，运行中拒绝回落隧道
    //    2 force-physical  — 只走物理网卡，找不到就让 connect 失败并返回
    //                        BC_R_NO_PHYSICAL_INTERFACE (64)
    //
    // 与 bypassVpn 同时设置时本字段胜出。沿用 -1 作 unset 哨兵（而非 0），
    // 这样 C-struct 零初始化的调用方拿到的是"未设置"而不是"os"。
    int32_t     vpnPolicy;
```

- [ ] **Step 2: 写入 BCFObject**

`src/cpp/apple/ios_bridge.mm`，在 `p->PutBool("disableAutoRestart", ...)` 附近找到写配置的位置，插入：

```objc
    // -1 表示未设置：不写这个键，让 SMPConnector::Config::Init 走
    // bypassVpn 兼容映射 / 平台默认值的回落链路。
    if (cfg->vpnPolicy >= 0) {
        const char* policyStr = tt_vpn_policy_to_string((TTVpnPolicy)cfg->vpnPolicy);
        p->PutString("vpnPolicy", policyStr);
    }
```

顶部需 `#include "VpnPolicy.h"`。注意 `tt_vpn_policy_to_string` 对越界值返回 `"invalid"`，原生侧解析时会告警并回落平台默认——这是期望行为，不需要在这里额外校验。

同时确认现有的 `bypassVpn` 写入逻辑（若存在）保持不变。

- [ ] **Step 3: Swift 侧新增枚举与属性**

`src/swift/TTSignalConfig.swift`，在 `bypassVpn` 属性之前加入枚举定义（放在 `TTSignalConfig` 类型之外，文件顶层）：

```swift
/// VPN / 虚拟网卡选择策略。取代已废弃的 `bypassVpn`。
public enum TTSignalVPNPolicy: Int32 {
    /// 跟随系统路由，允许 QUIC 流量走 VPN / utun 隧道。iOS 默认值——
    /// 装了 per-app VPN 的用户通常就是希望流量走 VPN。
    case os = 0
    /// 优先物理网卡（wifi / wired / cellular）。启动时找不到物理网卡可回落
    /// 隧道，运行中拒绝回落（保持当前 socket）。macOS / Windows / Linux 默认值。
    case preferPhysical = 1
    /// 只走物理网卡，任何阶段都不回落，也不撤销已有的网卡绑定。找不到物理
    /// 网卡时 `connect` 立即失败并回调 `BC_R_NO_PHYSICAL_INTERFACE`(64)。
    /// 用于服务端必须观测到客户端真实 IP 而非 VPN 出口 IP 的场景。
    case forcePhysical = 2
}
```

把现有的 `bypassVpn` 属性改为：

```swift
    /// VPN / 虚拟网卡选择策略。`nil` 表示用平台默认值（iOS 上是 `.os`）。
    ///
    /// 与 `bypassVpn` 同时设置时本属性胜出，原生层会打一条警告日志。
    public var vpnPolicy: TTSignalVPNPolicy?   = nil

    @available(*, deprecated, message: "改用 vpnPolicy。false 等价于 .os，true 等价于 .preferPhysical")
    public var bypassVpn: Bool?                = nil
```

保留 `bypassVpn` 原有的注释内容作为 deprecated 说明的补充，不要整段删除。

- [ ] **Step 4: Swift 侧填充 C 结构体**

在 `withCConfig` 里，找到填充 `c.bypassVpn` 的那一行，在其后加入：

```swift
        c.vpnPolicy = vpnPolicy?.rawValue ?? -1
```

若 `withCConfig` 里目前没有 `c.bypassVpn = ...`（因为它是 `Bool?` 需要转 tri-state），照它现有的写法对齐即可；两个字段都要填，`vpnPolicy` 未设置时必须显式写 `-1`。

- [ ] **Step 5: 构建 iOS**

Run:
```bash
build/ios-sim-arm64-release/build 2>&1 | tail -20
build/ios-device-arm64-release/build 2>&1 | tail -20
```
Expected: 两个 slice 都构建成功

- [ ] **Step 6: 构建完整 xcframework**

Run:
```bash
ios/scripts/build-core.sh 2>&1 | tail -10
ios/scripts/build-xcframework.sh 2>&1 | tail -10
```
Expected: 产出 `build/ios-xcframework/TTSignal.xcframework`

- [ ] **Step 7: 提交**

```bash
git add src/cpp/apple/ios_bridge.h src/cpp/apple/ios_bridge.mm src/swift/TTSignalConfig.swift
git commit -m "feat(ios): TTConfig 与 Swift 绑定暴露 vpnPolicy

TTConfig 末尾追加 int32_t vpnPolicy（-1 unset / 0 os / 1 prefer / 2
force），Swift 侧提供 TTSignalVPNPolicy 枚举，bypassVpn 标记 deprecated。
iOS 默认仍为 os，行为不变；显式设置 preferPhysical / forcePhysical 时
Task 5 合并后的 ResolveActiveIfIndex 会真正生效。"
```

---

### Task 14: 跨项目类型声明与 release note

**Files:**
- Modify: `../rtc-client/…/ttsignal.d.ts`（确切路径需现场查找）
- Create/Modify: 仓库的 release note（`README.md` 或 `docs/` 下的变更记录，现场确认）

**Interfaces:**
- Consumes: 前面所有任务
- Produces: 无代码接口

- [ ] **Step 1: 定位 rtc-client 的类型声明**

Run:
```bash
ls ../rtc-client 2>/dev/null && find ../rtc-client -name "ttsignal.d.ts" -not -path "*/node_modules/*" 2>/dev/null
```

若 `../rtc-client` 不存在，跳过 Step 2-3，只做 Step 4，并在汇报中说明"rtc-client 不在本机，类型声明需另行同步"。

- [ ] **Step 2: 补类型声明**

在 `ttsignal.d.ts` 的 connector config 接口里加入：

```ts
  /**
   * VPN / 虚拟网卡选择策略。
   * - 'os'              跟随系统路由，允许走 VPN
   * - 'prefer-physical' 优先物理网卡，运行中拒绝回落隧道（桌面默认）
   * - 'force-physical'  只走物理网卡，找不到时 connect 立即失败（错误码 64）
   */
  vpnPolicy?: 'os' | 'prefer-physical' | 'force-physical';
  /** @deprecated 改用 vpnPolicy。false -> 'os'，true -> 'prefer-physical' */
  bypassVpn?: boolean;
```

- [ ] **Step 3: 检查 rtc-client 是否依赖 Windows 旧默认行为**

Run:
```bash
grep -rn "bypassVpn\|vpnPolicy" ../rtc-client/src 2>/dev/null
```

若有命中，逐处确认新语义下行为是否仍符合预期，特别是 Windows——它的默认值从 `os` 变成了 `prefer-physical`。把结论写进汇报。

- [ ] **Step 4: 写 release note**

Run:
```bash
ls docs/*.md README.md
grep -n "## " README.md | head -20
```

在合适位置（优先仓库已有的变更记录文件；没有就在 `README.md` 加一节）写入：

```markdown
### vpnPolicy（新增）

新增 connector 配置项 `vpnPolicy`，三态取代已废弃的 `bypassVpn`：

| 取值 | 含义 |
|---|---|
| `os` | 跟随系统路由，允许 QUIC 流量走 VPN / utun 隧道 |
| `prefer-physical` | 优先物理网卡；启动时可回落隧道，运行中拒绝回落 |
| `force-physical` | 只走物理网卡，绝不回落，也不撤销网卡绑定 |

默认值：macOS / Windows / Linux 为 `prefer-physical`，iOS 为 `os`。

`force-physical` 用于服务端必须观测到客户端真实 IP 而非 VPN 出口 IP 的场景。
该模式下找不到物理网卡时，`connect` 立即返回 `BC_R_NO_PHYSICAL_INTERFACE`（64），
不等超时。

#### ⚠️ 破坏性变更

1. **Windows 默认行为变更。** 本版本之前 Windows 实际等价于 `os`
   （`GetBestInterfaceEx` 不区分接口类型），现对齐为 `prefer-physical`。
   TUN 全局代理下，未显式设置 `vpnPolicy` 的 Windows 客户端会从"走 VPN"
   变为"优先物理网卡"。需要旧行为请显式设置 `vpnPolicy: 'os'`。
2. **macOS 上 `bypassVpn: false` 的语义被修正。** 旧实现里该取值仍然是
   "物理优先、找不到才回落"，从未真正跟随过系统顺序。现按字面映射为
   `'os'`。需要旧行为请设置 `vpnPolicy: 'prefer-physical'`。

#### Linux 限制

只有 `SO_BINDTODEVICE` 能真正保证包从物理网卡出去，它需要 `CAP_NET_RAW`
（或 root）。没有该权限时回落到 `IP_UNICAST_IF`，而该选项在 kernel <
6.0.16 / 6.1.2 / 6.2 上对已 connect 的 UDP socket 被静默忽略。这类主机在
全局 TUN 代理下，`force-physical` 只能保证失败可见，不能保证真实 IP 送达。
发生回落时会打 `_WARN_` 日志。
```

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "docs(vpn): 补 release note 与 rtc-client 类型声明

显著标注两处破坏性变更（Windows 默认值、macOS bypassVpn:false 语义）
与 Linux 无 CAP_NET_RAW 时的能力边界。"
```

---

## Self-Review

**Spec 覆盖核对**

| Spec 章节 | 覆盖任务 |
|---|---|
| §一 NAPI API 表面 | Task 8（解析）、Task 12（文档+校验） |
| §一 平台默认值表 | Task 1（`tt_vpn_policy_platform_default`） |
| §一 兼容映射 + 冲突告警 | Task 1（`tt_vpn_policy_resolve`）、Task 8（告警） |
| §一 两处行为变更记录 | Task 6（Windows 注释）、Task 14（release note） |
| §一 `TTNetworkMonitorOptions` | Task 4 |
| §一 `TTVpnPolicy` 枚举位置 | Task 1 + Task 4（`INetworkPathMonitor.h` include `VpnPolicy.h`） |
| §一 iOS `TTConfig` / Swift | Task 13 |
| §一 `SMPConnector::Config` | Task 8 |
| §二 语义矩阵 | Task 5/6/7（monitor 三列）、Task 9（两列 unpin）、Task 10（无物理网卡列） |
| §3.1 macOS/iOS 合并 | Task 5 |
| §3.1 `_ex` 查询函数 | Task 4（声明+占位）、Task 5/6/7（各平台实现） |
| §3.2 Windows physical-first | Task 6 |
| §3.3(a) Linux 判定受 policy 控制 | Task 3 + Task 7 |
| §3.3(b) Linux `SO_BINDTODEVICE` | Task 11 |
| §3.4 UDPSender 抑制 unpin | Task 9 |
| §四 错误码与失败路径 | Task 2（错误码）、Task 10（失败路径+诊断日志） |
| §五 单元测试 | Task 1（policy 矩阵）、Task 3（Linux 判定） |
| §五 手工验证矩阵 | 不在本计划内——需真机与 VPN 环境，实现完成后单独执行 |
| §六 跨项目影响 | Task 14 |

**说明两处与 spec 的有意偏离：**

1. Spec §一写"`TTVpnPolicy` 同样定义在 `INetworkPathMonitor.h`"。本计划改为定义在新建的 `src/cpp/VpnPolicy.h`，由 `INetworkPathMonitor.h` include 它。理由：`UDPSender` 与 `SMPConnector` 都需要这个枚举但不需要 monitor 的 C API，而且纯逻辑独立成文件才能单测。对外契约不变——include `INetworkPathMonitor.h` 依然能拿到 `TTVpnPolicy`。
2. Spec §五写"Linux 物理网卡判定：`/sys/class/net` 可 mock"。本计划把它实现为显式的根路径注入参数（`tt_linux_iface_is_physical_at`）而非 mock 框架，与仓库既有的无框架 standalone 测试约定一致。

**未覆盖的 spec 要求：** 无。

**类型一致性核对**

- `TTVpnPolicy` 在 Task 1 定义，Task 4/5/6/7/8/9/13 消费，名称与四个取值全程一致
- `tt_vpn_policy_resolve` 的 5 参数签名在 Task 1 定义，Task 4（Apple/Win/Linux 三处）与 Task 8 调用，参数顺序一致
- `tt_netmon_query_default_ifindex_ex(int)` 在 Task 4 声明并给三平台占位实现，Task 5/6/7 各自替换为真实实现，Task 10 调用
- `tt_linux_iface_is_physical` 在 Task 3 定义，Task 7 消费
- `BC_R_NO_PHYSICAL_INTERFACE` 在 Task 2 定义，Task 10 使用，Task 12/13/14 文档引用，值统一为 64
- `SetVpnPolicy` 在 Task 9 同时定义于 `UDPSender` 与 `UDPSenderGroup`，命名一致
- `SMPConnector::Config::vpn_policy` 在 Task 8 定义，Task 9（`connector->config_.vpn_policy`）与 Task 10（`connector_->config_.vpn_policy`）消费——**注意这两处的 `connector` / `connector_` 写法不同**，各任务步骤内已提示现场核对

**任务依赖顺序**

Task 1 → 2 → 3 是三个互不依赖的基础单元，可并行。Task 4 依赖 1。Task 5/6/7 依赖 4（且 7 依赖 3）。Task 8 依赖 1+4。Task 9 依赖 1+8。Task 10 依赖 2+4+8。Task 11 依赖 9。Task 12 依赖 8。Task 13 依赖 1+8。Task 14 依赖全部。

按 1→2→3→4→5→6→7→8→9→10→11→12→13→14 顺序执行即可满足所有依赖。
