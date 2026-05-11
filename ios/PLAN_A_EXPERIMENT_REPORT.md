# iOS QUIC Connection Migration 方案 A 实验报告

> **状态：已废弃（DEPRECATED）**
>
> Apple `NWProtocolQUIC` 与 LiveKit 服务端 19 字节 server-issued
> Connection ID 不兼容，方案 A 在真机切网时无法完成 RFC 9000 connection
> migration（详见本报告第 8 节）。**iOS QUIC migration 现以方案 C
> 为准**：见 [`ios/PLAN_C_DESIGN.md`](./PLAN_C_DESIGN.md)，新的 Swift
> binding 在 [`src/swift/`](../src/swift/)，C/ObjC++ 桥接在
> [`src/cpp/apple/`](../src/cpp/apple/)，xcframework 产物在
> [`build/ios-xcframework/TTSignal.xcframework`](../build/ios-xcframework/TTSignal.xcframework)。
>
> 老的 `ios/QUICClient.swift` 已删除。本报告保留作为方案选型记录。

## 1. 背景

Android 端的 ttsignal 已经实现了**网卡切换时不丢 QUIC 连接**的能力：监听 `ConnectivityManager.NetworkCallback`，在新网络 validated 后调用 `Connection.restart(networkHandle)`，底层重建 UDP socket 并通过 xquic 的 `jqc_conn_local_addr_changed` 完成 RFC 9000 connection migration。整个过程对上层（rtc-client / SignalClient）完全无感，**QUIC 会话、加密密钥、流状态、拥塞窗口全部保留**。

iOS 端目前用 Apple `Network.framework` 的 `NWConnection + NWProtocolQUIC`，整个 QUIC 引擎和底层 UDP socket 都在系统进程里（nehelper / nesessionmanager），用户态完全摸不到。问题：**iOS 能不能做到 Android 一样的"socket 重建但 QUIC 状态保留"？**

理论上有三条路：

| 方案 | 描述 | 上层感知 | 代价 |
|---|---|---|---|
| **A** | 不限制 interface + 加观察 handler，让系统自行迁移 | 完全无感（如果系统真做了） | 极低，几行代码 |
| **B** | `QUICClient` 内部自己 cancel + 重建 NWConnection，QUICClient 实例不变 | 业务层 QUICClient 不重建，但底层 QUIC 重握手 | 中等，需要重连一次 |
| **C** | 把 xquic 移植到 iOS，彻底放弃 NWProtocolQUIC | 完全无感 + 与 Android 对齐 | 大，需要新写一套 NAPI/Swift binding |

本报告记录方案 A 的实验过程与结论。

## 2. 方案 A 实现

核心改动在 [`ios/QUICClient.swift`](./QUICClient.swift)，三件事：

### 2.1 NWParameters 调整

```swift
parameters.serviceClass = .responsiveData
parameters.preferNoProxies = true
// 不指定 requiredInterfaceType / prohibitedInterfaceTypes
// 让系统在 wifi / cellular / wired 之间自由选择
```

### 2.2 注册三个观察 handler

```swift
private func installMigrationObservers(on stream: NWConnection) {
    stream.viabilityUpdateHandler = { [weak self] isViable in ... }
    stream.betterPathUpdateHandler = { [weak self] hasBetter in ... }
    stream.pathUpdateHandler = { [weak self] path in ... }
}
```

每个 handler 仅做：去重 → 写日志 → 通过 delegate 上报。**不主动 cancel/重建** NWConnection。

### 2.3 独立的 NWPathMonitor

```swift
private func startPathMonitor() {
    let monitor = NWPathMonitor()
    monitor.pathUpdateHandler = { [weak self] path in
        // 把当前 active interface (wifi/cellular/wired/...) 报给 delegate
    }
    monitor.start(queue: connectionQueue)
    pathMonitor = monitor
}
```

作为 `NWConnection.pathUpdateHandler` 的补充观察源，部分场景下 NWConnection 自己的 path 回调不会触发，但系统级 NWPath 变了。

### 2.4 协议扩展（向后兼容）

`WTMessageDelegate` 上加了三个**可选默认空实现**回调：
- `quicClient(_:didChangeViability:)`
- `quicClient(_:didDetectBetterPath:)`
- `quicClient(_:didChangePath:)`

老 delegate 完全无需改动。

完整 diff 见 git history。

## 3. 测试 app

为了验证方案 A 实际表现，搭了一个独立的 SwiftUI 测试工程 [`ios/QUICTest.xcodeproj`](./QUICTest.xcodeproj)：

```
ios/QUICTest/
├── QUICTestApp.swift          @main 入口
├── ContentView.swift          状态卡片 + Connect/Disconnect/Send 按钮 + 实时日志
├── QUICClientDemo.swift       ApplicationHandler + ConnectionStats (ObservableObject)
├── LiveKitStubs.swift         Loggable / LiveKitError stub（让同一份 QUICClient.swift 既能在 SDK 跑也能在测试 app 跑）
├── QUICClient.swift           symlink → ../QUICClient.swift
└── ...
```

UI 实时显示：
- `State` (CONNECTING/CONNECTED/FAILED/CLOSED)
- `Path` (wifi/cellular/wired/...)
- `Viable` / `Better` 标志
- `PathΔ` (path 变化次数)
- `Sent` / `Recv` 计数
- 滚动日志（最近 200 条）

## 4. 实验环境

- iPhone 真机（**必须真机**，模拟器没真实蜂窝接口）
- iOS 18.x
- 测试服务器：`tlivekit9tcew3gy.test.chative.im`
- 测试动作：起始走 cellular，连接建立后开 Wi-Fi 让系统自动迁移

## 5. 实验结果（关键日志）

```
[INFO]  QUICClient.init():332 Raw QUIC Client initialized...
[INFO]  [Migration] connection.path=cellular
[INFO]  [Demo] path=cellular
[INFO]  Raw QUIC Client连接正在准备中...
[INFO]  Raw QUIC Client隧道已建立，可以接收入站流和创建出站流。
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13ae03200:udp
[INFO]  [Migration] connection.path=wifi                    ← 系统第一次切到 wifi
[INFO]  [Demo] path=wifi
        quic_conn_setup_pmtud [C1.1.1:1] [-7ca6cf98] unable to query remote endpoint, assuming IPv6
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13ae03200:udp
[INFO]  [Migration] betterPathAvailable=true
[INFO]  [Demo] betterPath=true
        nw_protocol_implementation_lookup_path [C1.1.1:1] No path found for ef816fc0505e4d38       ← Apple QUIC 内部丢失 path 引用
        nw_protocol_implementation_lookup_path [C1.1.1:1] No path found for ef816fc0505e4d38
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13afbf840:udp ← 系统建了一个新 UDP 实例
[INFO]  [Migration] betterPathAvailable=false
[INFO]  [Demo] betterPath=false
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13afbf840:udp
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13afbf840:udp
[INFO]  [Migration] betterPathAvailable=true
[INFO]  [Demo] betterPath=true
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13afbf840:udp ← 重复 6+ 次
        nw_protocol_instance_set_output_handler Not calling remove_input_handler on 0x13afbf840:udp
        ...
        nw_read_request_report [C1] Receive failed with error "Socket is not connected"  ← POSIX 57
[INFO]  Raw QUIC Client stream closed by server (not draining)
[INFO]  Raw QUIC Client连接失败！错误类型: POSIXErrorCode(rawValue: 57): Socket is not connected
[ERROR] [Demo] failed: POSIXErrorCode(rawValue: 57): Socket is not connected
```

## 5.1 服务端侧实际行为（关键对照证据）

仅看客户端日志，初判很容易把锅扣给 "Apple NWProtocolQUIC 在 path flapping 时状态机损坏"。但同时拉到 livekit-ai 侧的 `quicrouter.go` 日志（`data/quic-server.4.28.log`）后，**结论需要修正**——服务端侧迁移本身是工作的，问题在更上游的 CID 兼容性。

服务端日志摘要（以第二次连接为例，时间窗 03:43:16 ~ 03:45:34，约 2 分 13 秒）：

| 时间 | 客户端地址 | DCID | 长度 | 事件 |
|---|---|---|---|---|
| 03:43:16.198 | **114.250.178.236:23487** | `365498064faa60f3` | 8B | 客户端首个 Initial |
| 03:43:16.296 | 114.250.178.236:23487 | `51554943001ec80a0712b9d42ed0da558d5ee2` | **19B** | 客户端用服务端发的 server-issued CID 续 Initial |
| 03:43:16.31 ~ 17.3 | 114.250.178.236:23487 | 同上 19B | — | Handshake → 1-RTT 数据 |
| **03:43:30.225** | **221.216.116.209:2662** | `51554943001ec80a0712b9af55d953c6058f09` | **19B** | **IP 切换 + DCID 切换**（NEW_CONNECTION_ID 给的另一个备用 CID） |
| 03:43:30 → 03:45:34 | 221.216.116.209:2662 | `...af55d953c6058f09` | 19B | 服务端持续在新 path 上 send/recv |

关键观察：

1. 两个 19 字节 DCID 共享前缀 `51554943001ec80a0712b9`（11 字节，前 4 字节 `51 55 49 43` 对应 ASCII **"QUIC"**）→ 这是 livekit-ai 实现的 **QUIC-LB 风格 CID 路由**：前缀做 router 标识，尾部 8 字节做 backend selector。
2. `d42ed0da558d5ee2` / `af55d953c6058f09` 是同一个 QUIC connection 通过 NEW_CONNECTION_ID 帧下发的两个备用 CID，分别对应两条 path。
3. 03:43:30 客户端从 `114.250.178.236:23487` → `221.216.116.209:2662`，**同时切换 DCID** ——教科书式的 RFC 9000 connection migration，**服务端完整接收并继续转发**。
4. 03:45:29 服务端确实报了一条 error，但跟 QUIC 层无关：

   ```json
   {"level":"error","caller":"service/quicserver.go:496",
    "msg":"Error creating QuicForwarder:",
    "error":"dial tcp 10.1.10.197:7983: connect: connection timed out"}
   ```

   是 LiveKit RPC forward 路径连后端业务（10.1.10.197:7983）超时——**应用层错误**。报错前后 QUIC 层都还在收发包。

> 结论：**服务端侧 connection migration 完全成功**，问题不在 LiveKit `quicrouter.go`，也不在迁移流程本身。

## 6. 问题诊断（基于客户端 + 服务端日志对照）

### 6.1 第一次 path 切换看起来"成功"，但很快崩溃

客户端 `connection.path=cellular` → `connection.path=wifi` 切换后短暂可用，但随即出现 `No path found for <CID>`、`Not calling remove_input_handler` 反复刷屏，最终 `POSIX 57 (ENOTCONN)` 整条连接死亡。

### 6.2 真正的根因：**Apple QUIC 与服务端 19 字节 server-issued CID 不兼容**

把客户端关键日志 line 110 拎出来：

```
quic_conn_process_inbound [C13.1.1:1]
  [-51554943001ec80a0712b9d42ed0da558d5ee2] unable to parse packet
```

这条 CID `51554943001ec80a0712b9d42ed0da558d5ee2` 与服务端 line 31 完全一致——**正是 livekit-ai quic-lb-router 发给客户端的 19 字节 server-issued CID**。"unable to parse packet" 意味着 Apple QUIC 收到包但解析失败。

QUIC 短头部 1-RTT 包**不携带 DCID 长度字段**——端点必须自己记住"我当前在这条 path 上的 DCID 长度是多少"。如果 Apple 的 NWProtocolQUIC 内部对短头部 CID 长度做了 hard-coded 假设（比如 8 字节），遇到 19 字节就会按错位置解 AEAD，整个包认不出来。

### 6.3 path 表查不到 CID 是同一个 bug 的连锁反应

```
nw_protocol_implementation_lookup_path [C1.1.1:1] No path found for ef816fc0505e4d38
                                                                  ^^^^^^^^^^^^^^^^
nw_protocol_implementation_lookup_path [C5.1.1:1] No path found for 401666e06fa1d880
```

`ef816fc0505e4d38` / `401666e06fa1d880` 都是 8 字节，对应**服务端 NEW_CONNECTION_ID 帧给的备用 CID 后 8 字节**（与服务端 DCID 后 8 字节 `d42ed0da558d5ee2` / `af55d953c6058f09` 是同源的 server-issued CID 池）。

含义：当 Apple QUIC 收到一个用某个备用 CID 发的包，要去 path 表里查"这个 CID 关联到哪条 path"——查不到。NEW_CONNECTION_ID 帧的存储或关联逻辑出了问题。

### 6.4 多个 UDP protocol instance 没清理（次生现象）

```
0x13ae03200:udp  ← 第一次 cellular path 上的 UDP 实例
0x13afbf840:udp  ← 系统在 wifi path 上 spawn 的新 UDP 实例
```

`0x13afbf840:udp` 重复 7+ 次 `Not calling remove_input_handler`——这是 Apple QUIC 栈内部 spawn 的 UDP 实例没被正确销毁，**不是我们代码的问题**（我们持有的 NWConnection 早就 cancel 干净，handler 也都置 nil 了），也无任何 API 可介入清理。这是 6.2/6.3 触发后内部状态机损坏的次生现象。

### 6.5 最终连接死亡

```
Receive failed with error "Socket is not connected"  (POSIX 57 = ENOTCONN)
```

底层 UDP socket 报 ENOTCONN，整条 NWConnection 转入 `.failed`，上层感知到信令断。

### 6.6 为什么 Android 没事

Android 用 xquic（自己实现的 QUIC），对任意长度（0–20 字节）的 DCID 都按 RFC 9000 处理，**不假设 8 字节**。所以同样的 livekit-ai 服务端、同样的 19 字节 server-issued CID，xquic 能正确解析，连接稳定。

## 7. 结论

**方案 A 在 iOS 上不可行**。但根因和最初判断不同：

| 错误归因 | 修正后归因 |
|---|---|
| ~~Apple NWProtocolQUIC 在 path flapping 时状态机有 bug~~ | **Apple NWProtocolQUIC 与 livekit-ai quic-lb-router 的 19 字节 server-issued CID 不兼容**，触发 path 表内部状态机损坏 |

证据链：

1. ❌ 客户端 `unable to parse packet` 的 CID 与服务端发的 19 字节 CID 一字不差
2. ❌ 客户端 `No path found for <8B CID>` 的 CID 与服务端 NEW_CONNECTION_ID 给的备用 CID 同源
3. ✅ 服务端侧 IP+DCID 切换被完整接收，迁移本身是工作的
4. ✅ Android xquic 用同一服务端连接稳定 → 不是服务端协议错，而是 Apple 实现侧的 CID 长度兼容性 bug
5. ❌ 4 次重连每次都按相同模式崩溃 → 确定性 bug，不是偶发 race

**附带影响**：这个 bug **不只在迁移时触发**——只要 Apple QUIC 内部任何 path 重选/重路由（系统底层很多场景会触发），都会查 path 表，都会触发 "No path found"。所以即使不主动迁移，长生命周期的连接也可能 idle 一段时间后崩溃。

## 8. 下一步建议

### 8.0 方案 0（30 分钟内可做的根因验证）

让 livekit-ai 临时把 `quicrouter.go` 给 iOS 客户端发的 server-issued CID 长度从 19B 缩短到 8B（牺牲或重构 LB 路由信息），iOS QUICTest 重跑同样的切网测试。预期：

- 如果 `unable to parse packet` / `No path found for <CID>` 全部消失 → 6.2 的根因假设成立，确认是 CID 长度兼容性问题
- 如果还是崩 → 说明 Apple NWProtocolQUIC 的 connection migration 实现确实有更深层的 bug，回退到方案 B/C

这是后续走方案 B 还是方案 C 之前**最便宜的判定动作**，建议先做。

需要 livekit-ai 配合：在 `pkg/routing/quicrouter.go` 的 server-issued CID 生成逻辑里，给 ALPN=`ttsignal` 且 client UA 是 iOS 的连接走 8B CID 兜底（或者全局开关临时切到 8B 跑一组对照实验）。

剩下的两条路：

### 8.1 方案 B（推荐短期方案）

`QUICClient` 内部自管 NWConnection 生命周期：
- 用 `NWPathMonitor` 监听 active interface 类型变化（wifi ↔ cellular）
- 检测到真实切换后，主动 `cancel()` 旧 NWConnection
- 用相同的 props 新建 NWConnection，重新 `connect` cmd
- `QUICClient` Swift 实例本身不重建，`delegate` 不变，只是底层 `stream` 被透明换掉
- 给上层（LiveKit SignalClient）暴露一个可选的 `restart()` 触发器（手动 / 自动）

代价：
- ✅ 上层（LiveKit `SignalClient` 等）**不需要重建 QUICClient 实例**
- ❌ QUIC 会话、加密、流状态会重建（**~500ms 量级的握手 + connect cmd**）
- ❌ 比 Android 的"无损迁移"差一截，但比断线重连流畅很多

预计代码量：QUICClient.swift 加 ~80 行。可在当前 QUICTest 工程里直接验证。

### 8.2 方案 C（长期方案 / 与 Android 对齐）

把 `deps/jquic`（xquic）编进 iOS framework，自己用 BSD socket 或 `NWConnection`(UDP only) 做 IO，复用 SDK 已有的 `SMPConnector` C++ 代码。

代价：
- ✅ 与 Android 完全对齐：所有逻辑（restart / migration / 拥塞控制策略 / 日志格式）一致
- ✅ 真正无损迁移
- ❌ 工作量大：要给 iOS 加一份 C++ ↔ Swift binding（类似 Android 的 JNI 那一层）
- ❌ 需要处理 iOS 后台限制、socket 权限、TLS（boringssl）等

预计代码量：weeks 级别。

### 8.3 选择建议

推荐执行顺序：

1. **先做方案 0（30 分钟）**：服务端临时缩 CID 到 8B，iOS 重跑实验，确认根因。
2. **方案 0 通过** → 服务端长期保留 8B server-issued CID 路径（或为 iOS 客户端单独走 8B 通道）+ iOS 仍然只用 NWProtocolQUIC，不需要方案 B/C。但要评估 8B CID 对 livekit-ai LB 路由信息的影响。
3. **方案 0 不通过 / LB 必须用 19B**：
   - 业务能接受切网时断线重连一次 → **方案 B**
   - 业务对"切网期间的连接保持"有强诉求 → **方案 B 兜底 + 方案 C 长期目标**
4. 不管走哪条，**方案 A（让系统自行迁移）在 Apple 修 NWProtocolQUIC 的 CID 长度兼容性之前都不要再考虑**。

## 9. 附录：相关文件

| 路径 | 说明 |
|---|---|
| [`ios/QUICClient.swift`](./QUICClient.swift) | 方案 A 改造完成，含 viability/betterPath/path 三个观察 handler 和 NWPathMonitor |
| [`ios/QUICTest.xcodeproj/`](./QUICTest.xcodeproj) | 独立测试工程，Xcode 16+ 直接打开 |
| [`ios/QUICTest/`](./QUICTest) | 测试 app 源码（SwiftUI） |
| [`ios/QUICTest/QUICClient.swift`](./QUICTest/QUICClient.swift) | symlink → `../QUICClient.swift`，与 SDK 共用同一份 |
| [`data/quic-client.4.28.log`](../data/quic-client.4.28.log) | 客户端实验原始日志（4 次重连） |
| [`data/quic-server.4.28.log`](../data/quic-server.4.28.log) | livekit-ai 服务端 quicrouter 对照日志 |
| [`src/cpp/SMPConnector.cpp`](../src/cpp/SMPConnector.cpp) `OnRestart` | Android 侧 restart 实现参考 |
| [`deps/jquic/include/xquic/xquic.h`](../deps/jquic/include/xquic/xquic.h) `jqc_conn_local_addr_changed` | xquic 的 connection migration API |
| `../livekit-ai/pkg/routing/quicrouter.go` | livekit-ai QUIC LB router 实现（19B server-issued CID 来源） |
| `../livekit-ai/pkg/service/quicserver.go:496` | `QuicForwarder` 创建逻辑（与本次根因无关，仅记录 03:45:29 的应用层错误来源） |
