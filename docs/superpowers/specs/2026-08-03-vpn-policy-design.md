# vpnPolicy：三态网卡选择策略

日期：2026-08-03
状态：设计已确认，待实现

## 背景与问题

开启 VPN 后，虚拟网卡（macOS/iOS 的 `utun*`、Linux 的 `tun*`/`tailscale*`、Windows 的 wintun/TAP）会抢占默认路由。ttsignal 的 QUIC 流量随之从 VPN 出口发出，服务端（livekit-ai `pkg/routing/quicrouter.go`）拿到的 client IP 全部是 VPN 出口 IP，无法获取客户端真实 IP。

### 现状：`bypassVpn` 只解决了一半

代码里已有 `bypassVpn` 开关（NAPI `config.bypassVpn` / iOS `TTConfig.bypassVpn` / Swift `TTSignalConfig.bypassVpn`），默认 `true`。它只影响**路径监视器挑哪块网卡**，不影响**socket 最终绑在哪块网卡上**：

1. `AppleNetworkMonitor.mm` 的 `ResolveActiveIfIndex` 在 macOS 上确实遍历 path、优先选 wifi/wired/cellular，跳过 utun/ipsec/ppp，把物理网卡 ifIndex 交给 `setsockopt(IP_BOUND_IF)`。这一步是对的。
2. **但 `UDPSender::Connect`（`UDPSender.cpp:314-352`）会把它撤销**：连接前做一次 `tt_route_lookup_ifindex(peer)` 路由表校验，发现"内核认为该走 utun，而我们钉的是 en0"时判定为 mismatch，主动调 `_TryClearInterfaceBinding()` → `setsockopt(IP_BOUND_IF, 0)`，流量重新回到 VPN。

这段 unpin 逻辑不是 bug。它防的是 Clash/Surge/mihomo TUN 模式下的黑洞：硬钉物理网卡后，若对端只能经 VPN 到达（典型如 fake-IP 段 `198.18.0.0/15`），包被静默丢弃且 `send()` 仍返回成功，连接表现为无法诊断的超时。所以"强制物理网卡"和"连得上"在 TUN 场景下是一组真实取舍——这正是需要一个显式开关的原因。

### 各平台缺口

| 平台 | monitor 选网卡 | Connect 时的 unpin |
|---|---|---|
| **macOS** | 已 physical-first，受 `bypassVpn` 控制 | 路由表 mismatch 就主动 unpin，撤掉物理钉（主缺口） |
| **Windows** | `GetBestInterfaceEx(0.0.0.0)` 完全不看接口类型，TUN 抢了默认路由就选 TUN（`WinIpChangeMonitor.cpp:95`） | 同样主动 unpin（`UDPSender.cpp:370`） |
| **Linux** | `IsVirtualIface` 名字黑名单**无条件**硬编码跳过虚拟网卡，反而不受 `bypassVpn` 控制（`LinuxNetlinkMonitor.cpp:80,205`） | 不主动 unpin，只告警；但被动失败路径仍会 clear |
| **iOS** | `bypassVpn` 完全是空操作——`ResolveActiveIfIndex` 的 iOS 分支 `(void)physicalOnly`（`AppleNetworkMonitor.mm:194`），取 OS 顺序第一个接口 | `tt_route_lookup_ifindex` 在 iOS 是返回 0 的桩，不会 unpin |

Linux 另有一个坑：只有 tun 存在时 `QueryDefaultIfIndex` 返回 0 → 不触发 restart → socket 根本不绑定 → 内核照样把包送进 VPN。即 Linux 现状是"选网卡时跳过了 VPN，但没真正绑定物理网卡"。

## 目标与非目标

**目标**：给业务一个显式开关，声明"这条信令必须走物理网卡，服务端必须看到真实 IP"，并在做不到时**失败可见**。

**非目标**：
- 不做自动降级/回落重试。严格模式失败后由业务决定是否改用宽松策略重连。
- 不改变任何平台的现有默认行为。
- 不做 Connection 级覆盖，开关只在 Connector 级。
- 不动 Android（`src/java` 没有 `bypassVpn`，走独立的 Java `NetworkCallback` 路径）。

## 设计

### 一、API 表面

**NAPI（`createConnector` config）**

```js
vpnPolicy: 'os' | 'prefer-physical' | 'force-physical'
bypassVpn: Boolean   // 保留，deprecated
```

默认值 **platform-dependent**：

| 平台 | 默认 `vpnPolicy` | 与改动前的关系 |
|---|---|---|
| macOS | `prefer-physical` | **不变**，等价于今天 `bypassVpn: true`（默认）的行为 |
| Linux | `prefer-physical` | **不变**，`IsVirtualIface` 今天就是无条件过滤虚拟网卡、找不到物理网卡则返回 0 |
| Windows | `prefer-physical` | **行为变更**，见下 |
| iOS | `os` | **不变**，等价于今天 iOS 的行为（`bypassVpn` 在 iOS 是空操作）。iOS 用户安装 per-app VPN 通常就是**希望**流量走 VPN |

兼容映射：只给 `bypassVpn` 时 `false → 'os'`、`true → 'prefer-physical'`；两者都给时 `vpnPolicy` 胜出并打一条 `_WARN_` 日志。

#### 两处必须记录的行为变更

规划实现时核对代码发现，"默认行为在所有平台均不变"并不成立，有两处例外：

**(1) Windows 默认值变更。** Windows 今天的实际默认等于 `os`——`WinIpChangeMonitor.cpp:246` 写明 `bypassVpn intentionally ignored`，`QueryBestIfIndex` 只有一条 `GetBestInterfaceEx(0.0.0.0)` 路径，不做任何接口类型过滤，TUN 抢了默认路由就选 TUN。本设计将 Windows 默认定为 `prefer-physical`，与 macOS / Linux 对齐，因此**所有未显式设置 `vpnPolicy` 的 Windows 调用方，在 TUN 全局代理下会从"走 VPN"变成"优先物理网卡、运行中拒绝回落 VPN"**。这是有意为之的取舍：桌面三平台默认值统一比保留 Windows 的历史特例更可预期。需在 release note 中显著标注。

**(2) macOS 上 `bypassVpn: false` 的语义被修正。** `AppleNetworkMonitor.mm` 的 macOS 分支**无论如何都先挑物理网卡**，`physicalOnly` 只决定"找不到物理网卡时回落到 utun 还是返回 0"。所以旧代码里两个取值都从未产生过"跟随 OS 顺序"的行为：

| 旧 `bypassVpn` | macOS 实际行为 | 对应新 policy |
|---|---|---|
| `false` | 物理优先，找不到才回落 VPN | 接近 `prefer-physical` |
| `true`（默认） | 物理优先，找不到就返回 0 保持现 socket | `prefer-physical`（live 语义） |

按本设计的 `false → os` 映射，显式写了 `bypassVpn: false` 的 macOS 调用方会从"物理优先"变成"真正跟随 OS 顺序"。这个映射是有意保留的：`false` 的字面意思就是"不要绕开 VPN"，旧实现根本没兑现，属于顺带修正。

**C 层 `TTNetworkMonitorOptions`**（`src/cpp/INetworkPathMonitor.h`）

末尾追加 `int vpnPolicy;`，遵守该结构体已有的"只追加不重排"ABI 约定；`bypassVpn` 原地保留。取值 `-1` unset / `0` os / `1` prefer-physical / `2` force-physical。各平台实现在 `vpnPolicy == -1` 时从 `bypassVpn` 推导，从而让不认识新字段的旧调用方行为完全不变。

枚举类型 `TTVpnPolicy` 同样定义在 `INetworkPathMonitor.h`（该头文件已是三平台 monitor 与 `SMPConnector` 的共同契约）。C++ 侧用它，C ABI 边界（`TTNetworkMonitorOptions` / `TTConfig`）仍走 `int` 以避免枚举宽度问题。

**iOS bridge `TTConfig`**（`src/cpp/apple/ios_bridge.h`）

追加 `int32_t vpnPolicy;`，同样的 `-1/0/1/2` 编码；现有 tri-state `bypassVpn` 保留。选 `-1` 作 unset 哨兵（而非 `0`）与该文件里 `bypassVpn` 的既有约定一致，让 C-struct 零初始化的调用方仍拿到安全默认。

**Swift**（`src/swift/TTSignalConfig.swift`）

```swift
public enum TTSignalVPNPolicy { case os, preferPhysical, forcePhysical }
public var vpnPolicy: TTSignalVPNPolicy? = nil   // nil = 平台默认（iOS 上即 .os）

@available(*, deprecated, message: "Use vpnPolicy instead")
public var bypassVpn: Bool? = nil
```

**SMPConnector::Config**（`src/cpp/SMPConnector.h`）

新增 `TTVpnPolicy vpn_policy;` 字段；`bool bypassVpn` 保留供兼容映射期间读取。`Config::Init`（`SMPConnector.cpp:2671` 附近）解析 `vpnPolicy` 字符串键，沿用现有 `pConfig->Get(...)` + `IS_BCF_STRING` 模式。

### 二、语义矩阵

| policy | monitor 选网卡 | `Connect()` 主动 unpin | `_OnConnectDone` 被动 unpin | 无物理网卡时 |
|---|---|---|---|---|
| `os` | OS 顺序第一个（含 VPN） | 允许 | 允许 | N/A |
| `prefer-physical` | 物理优先；启动查询可回落虚拟，运行中拒绝回落（返回 0，保持现 socket 不动） | 允许 | 允许 | 回落 VPN |
| `force-physical` | 只接受物理，任何阶段都不回落 | **禁止** | **禁止** | connect 失败 |

`force-physical` 的两条"禁止"是修复本问题的核心：`UDPSender.cpp:314-352` 的主动 unpin 与 `UDPSender.cpp:1325` `_OnConnectDone` 的被动 unpin，都需要在该模式下短路返回。

运行中物理网卡消失（拔网线、关 Wi-Fi，只剩 VPN）的行为已经是对的，沿用不改：`ResolveActiveIfIndex` 返回 0 → update handler 直接 `return` → 保持当前 socket，不迁移到 VPN。

### 三、各平台改动

#### 3.1 macOS / iOS

`ResolveActiveIfIndex`（`AppleNetworkMonitor.mm:107`）的 `bool physicalOnly` 参数换成 policy 枚举，**同时删掉 `#if TARGET_OS_OSX` 分支，两平台合并为一份实现**。

现在 iOS 分支的 `(void)physicalOnly` + "第一个接口胜出"，与 macOS 的 physical-first 是两套重复代码；合并后 iOS 自动获得同等能力，差异只体现在默认值上（见上表）。物理判定沿用 `nw_interface_get_type()` 白名单（wifi / cellular / wired），utun / ipsec / ppp 一律是 `nw_interface_type_other`。

启动查询 `tt_netmon_query_default_ifindex` 也需感知 policy：`prefer-physical` 下保持"允许回落虚拟"以便 VPN-only 机器能 bootstrap；`force-physical` 下不回落，返回 0。

该函数是 `INetworkPathMonitor.h:116` 声明、三平台各自实现（`AppleNetworkMonitor.mm:527` / `LinuxNetlinkMonitor.cpp:444` / `WinIpChangeMonitor.cpp:299`）的**已导出符号**，且当前**全仓无内部调用方**（仅出现在 iOS 导出符号表中）。因此不改它的签名，而是新增 `int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)`，原函数保留为委托调用、传平台默认 policy 的薄包装。

#### 3.2 Windows

`WinIpChangeMonitor.cpp:95` 的 `GetBestInterfaceEx(0.0.0.0)` 仅在 `os` 下保留。`prefer` / `force` 下改为：

1. 先取 `GetBestInterfaceEx` 的结果，若它已是物理网卡则直接采用（快路径，覆盖绝大多数无 VPN 场景）；
2. 否则 `GetIfTable2` 枚举，筛选 `MIB_IF_ROW2.InterfaceAndOperStatusFlags.HardwareInterface == 1` 且 `OperStatus == IfOperStatusUp` 且 `Type` 不在 `IF_TYPE_TUNNEL` / `IF_TYPE_PPP` / `IF_TYPE_SOFTWARE_LOOPBACK` 中，取 metric 最小者。

判定必须用 `HardwareInterface` 位而非名字或 `Type`：TAP-Windows 和 wintun 都上报为 `IF_TYPE_ETHERNET_CSMACD`，仅看 `Type` 会把它们误判为物理网卡。

#### 3.3 Linux

两处改动：

**(a) 网卡筛选受 policy 控制。** `IsVirtualIface`（`LinuxNetlinkMonitor.cpp:80`）现在是无条件生效的名字黑名单。改为：`os` 下不过滤（今天没有这个能力）；`prefer` / `force` 下过滤，且判定主依据换成 `/sys/class/net/<name>/device` 是否存在（真实硬件设备才有这个符号链接），现有名字黑名单降级为兜底（覆盖 `/sys` 不可读的容器场景）。

**(b) `force-physical` 必须换绑定手段。** `UDPSender.cpp` 自己的注释已经写明：`IP_UNICAST_IF` 在 kernel < 6.0.16 / 6.1.2 / 6.2 上，对已 `connect()` 的 UDP socket 被静默忽略（路由在 connect 时被缓存，缓存绕过 `fib_lookup`）。所以 Linux 严格模式按此顺序尝试：

1. `SO_BINDTODEVICE` —— 唯一硬保证，需 `CAP_NET_RAW` 或 root；
2. 失败（`EPERM`）则回落 `IP_UNICAST_IF` + `bind()` 到物理网卡源 IP，并打 `_WARN_` 说明该模式在 TUN 全局代理下可能不生效；
3. 两者都失败 → 按"无物理网卡"处理，connect 失败。

**必须诚实记录的限制**：只有 `SO_BINDTODEVICE` 能真正保证包从物理网卡出去。仅绑源 IP 不够——路由仍走 tun 时，包带着物理网卡的源 IP 进入 VPN，会被 NAT 改写或直接丢弃。因此**无 `CAP_NET_RAW` 权限的 Linux 进程在 TUN 全局模式下，`force-physical` 只能保证"失败可见"，不能保证"拿到真实 IP"**。这个限制需要同步写进 NAPI 的 JSDoc。

#### 3.4 UDPSender

policy 经 Connector 级 `BCFObject` 配置下传给每条连接的 `UDPSender`（沿用 `UDPSender::Config::Init(BCFObject*)` 现有模式，`UDPSender.h:84`）。`force-physical` 下：

- `Connect()` 的路由表 mismatch 检查（`UDPSender.cpp:314`）跳过 `_TryClearInterfaceBinding` 调用，改为打 `_WARN_` 记录 mismatch 事实但保持钉住；
- `_OnConnectDone`（`UDPSender.cpp:1303`，unpin 调用在 `1328`）收到 `BC_R_NETUNREACH` / `BC_R_HOSTUNREACH` / `BC_R_ADDRNOTAVAIL` 时同样不 unpin，直接把错误上抛。

### 四、错误码与失败路径

新增 `BC_R_NO_PHYSICAL_INTERFACE`，沿用现有 `BCRESULT` 体系（具体数值在实现时取未占用值）。

`SMPConnection::Connect` 在 `force-physical` 且当前无可用物理 ifIndex 时，直接回调 `onConnectResult(error = BC_R_NO_PHYSICAL_INTERFACE)`——不发包、不等 connect timeout。业务在一次回调内即可决策：降级到 `prefer-physical` 重连，或提示用户关闭 VPN。

失败时的日志需打出当时枚举到的**全部接口及其类型判定结果**，用于区分"真的没有物理网卡"和"判定函数误判"。这是三平台判定逻辑各不相同后最主要的现场排查手段。

### 五、测试

**单元测试**
- policy 解析 + `bypassVpn` 兼容映射的 3×3 组合（含两者同时给出时 `vpnPolicy` 胜出并告警）
- Linux 物理网卡判定：`/sys/class/net` 路径可注入，覆盖真实硬件 / tun / 容器内 `/sys` 不可读三种情形

**手工验证矩阵**（无法自动化，需真机）

4 平台 × {无 VPN、split-tunnel VPN、Clash TUN 全局} × 3 policy。

断言点是**服务端观测到的 client IP**，观测位置为 livekit-ai `pkg/routing/quicrouter.go` 记录的 remote addr。

重点用例：
- macOS + Clash TUN 全局 + `force-physical` → 服务端看到真实 IP（本次要修的主场景）
- macOS + split-tunnel VPN + 服务端仅内网可达 + `force-physical` → connect 返回 `BC_R_NO_PHYSICAL_INTERFACE` 或超时，**不静默回落**
- macOS / Linux / iOS + 不传 `vpnPolicy` → 行为与改动前一致（回归保护）
- **Windows + 不传 `vpnPolicy` + TUN 全局 → 预期与改动前不同**，需确认新行为（优先物理网卡）确实生效且连接可用，这是本次唯一有意引入的默认行为变更
- Linux 非 root + TUN 全局 + `force-physical` → 记录实际表现，验证告警日志确实出现

## 兼容性与跨项目影响

- **默认行为在 macOS / Linux / iOS 不变**；**Windows 默认从 `os` 变为 `prefer-physical`**，显式设置了 `bypassVpn: false` 的 macOS 调用方语义也被修正——两处均见 §一"两处必须记录的行为变更"，需写入 release note
- `rtc-client` 的 `ttsignal.d.ts` 需补 `vpnPolicy` 类型声明；同时确认 rtc-client 是否在 Windows 上依赖"默认走 VPN"的旧行为
- `livekit-ai` / `rtc-dashboard` 服务端不使用此开关，无需改动
- `livekit-client-swift` 未来接入时直接使用 `TTSignalVPNPolicy`

## 已确认的决策

1. **严格模式语义**：`force-physical` 下钉死物理网卡，连不上就失败，不做混合回落。理由是开关由业务显式打开，语义越简单越可预期；混合回落会让"有时拿到真实 IP、有时拿不到"变成不可复现的线上问题，比直接失败更难排查。
2. **开关形态**：复用 `bypassVpn` 升级为三态 `vpnPolicy`，而非新增独立 bool。避免出现 `bypassVpn: false` + `strictPhysical: true` 这类自相矛盾的组合。
3. **无物理网卡时**：立即失败并返回专属错误码，不挂起等待。等待只是把同一个决策推迟几秒，却要在 connector 与 connection 之间引入新状态机。
4. **iOS 默认值**：`os`，与其他桌面平台的 `prefer-physical` 不同。宁可接受 platform-dependent 默认值，也不在一次"加开关"的改动里静默改变 iOS 线上行为。
5. **iOS 本次一并补齐** physical-first 能力（合并 macOS/iOS 实现），供 livekit-client-swift 后续使用。
6. **Windows 默认值对齐桌面**：定为 `prefer-physical`，接受由此产生的默认行为变更。理由是桌面三平台默认值统一比保留 Windows 的历史特例更可预期；该变更需在 release note 中显著标注。
