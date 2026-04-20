# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

ttsignal 是基于 xquic 引擎的高性能 QUIC 信令库，支持 10K+ 并发连接。提供 JNI (Android/Java) 和 NAPI (Node.js) 两种绑定。在 RTC 基础设施中作为 WebSocket 的替代传输层，承载 LiveKit protobuf 信令和日志拉取等 RPC 命令。

## Build Commands

```bash
# Node.js NAPI addon (macOS arm64)
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Debug -DBUILD_NODE_ADDON=ON && make -j$(nproc)
# 产物: node_modules/ttsignal/Debug/ttsignal.darwin.arm64.node

# Linux x64
cd build && cmake ../src -DCMAKE_BUILD_TYPE=Release -DTARGET_PLATFORM_LINUX=ON && make -j$(nproc)
# 产物: node_modules/ttsignal/Release/ttsignal.linux.x64.node

# Android JNI
android/scripts/build-so.sh
```

## Architecture

### Source Structure

```
src/
  CMakeLists.txt          — 顶层构建，BUILD_NODE_ADDON / BUILD_JNI 开关
  cpp/
    Interface.cpp/h       — 核心 QUIC 逻辑入口
    Runtime.cpp/h         — 事件循环和线程管理
    SMPConnector.cpp/h    — 客户端 Connector (主动发起连接)
    SMPServer.cpp/h       — 服务端 Server (监听连接)
    SMPParser.cpp/h       — SMP 协议帧解析
    SMPacket.cpp/h        — 数据包序列化
    UDPSender.cpp/h       — UDP 发送器
    Utils.cpp/h           — 工具函数，每次构建自动 touch 以嵌入时间戳
    napi/                 — Node.js N-API 绑定
    jni/                  — Android JNI 绑定
    http-parser/          — HTTP 头解析 (WebTransport 握手)
  js/
    index.js              — Node.js 胶水层，包装原生类为 EventEmitter
    package.json          — npm 模块定义
  java/                   — Java/Android 接口
deps/
  jquic/                  — xquic QUIC 引擎 (核心传输实现)
  boringssl/              — TLS 1.3 加密
  node-addon-api/         — N-API C++ 封装
  node-v23.6.0/           — Node.js 头文件
js/
  client.js               — Node.js 客户端示例 (WebTransport/LiveKit 信令)
  server.js               — Node.js 服务端示例
  getLogFile.js           — 通过 ttsignal 拉取服务端日志文件
  upload-livekit-bin.js   — 上传 livekit-ai 二进制到服务器
```

### Key Concepts

**ALPN 始终是 `'ttsignal'`**：
- 无论 Connector 还是 Connection 级别，实际 QUIC ALPN 协商值始终为 `'ttsignal'`
- 代码中 `createConnection({ alpn: 'h3' })` 的 `alpn` 参数不代表实际 ALPN，**不要误以为 ALPN 是 `'h3'`**——这是反复验证过的纠错经验

**核心 API (Node.js)**：
- `ttsignal.createConnector(config)` → 创建 Connector，管理 QUIC engine
- `connector.createConnection(config)` → 创建 Connection，连接到远端
- `conn.connect(url, headers, timeout, callback)` → 发起连接
- `conn.on('streamCreated', stream => ...)` → 接收服务端创建的流
- `stream.sendData(buffer)` → 发送二进制数据
- `stream.getFile({ path })` → RPC: 获取远端文件（用于日志拉取）
- `stream.on('data', (data, fin) => ...)` → 接收数据

**拥塞控制**：支持 BBR2 (默认)、BBR、CUBIC、Reno、COPA、Unlimited。

**SMP 协议**：自研帧协议，支持 Command (0x01)、Message (0x02)、User Control (0x03) 三种帧类型，实现流上的 RPC 语义。

## Cross-Project Ecosystem

ttsignal 是 AI 驱动 RTC 基础设施闭环中的**传输层基石**，被其他 3 个项目共同依赖：

```
rtc-client ──QUIC信令+WebRTC推拉流──→ livekit-ai (SFU)
    │                                      │
    │ 集成 ttsignal SDK 信令通道             │ 产生日志 → OpenSearch
    ↓                                      ↓
ttsignal (本项目) ─────────────────→ rtc-dashboard (运维/日志/部署)
```

**在闭环中的角色**：
1. rtc-client 使用 ttsignal 作为 QUIC 信令通道连接 livekit-ai，传输 LiveKit protobuf 二进制
2. rtc-client 使用 ttsignal 的 `getFile` RPC 从 livekit-ai 中继拉取 rtc-dashboard 的服务端日志
3. livekit-ai 的 `pkg/routing/quicrouter.go` 接受 ttsignal ALPN 连接
4. rtc-dashboard 的 `pkg/routing` 也可接受 ttsignal 连接用于 RPC 命令

### 关联项目与影响矩阵

| 关联项目 | 本地路径 | 本项目中的集成点 | 联动修改场景 |
|---------|---------|---------------|------------|
| **rtc-client** | `../rtc-client` | rtc-client 的 `src/signaling/client.ts` 和 `src/metrics/log-collector.ts` 调用本项目的 Node.js API (createConnector/createConnection/sendData/getFile) | 修改 NAPI 接口签名、事件名、Stream API、SMP 帧格式时，需同步更新 rtc-client 的 TypeScript 类型声明 (`ttsignal.d.ts`) 和调用代码 |
| **livekit-ai** | `../livekit-ai` | livekit-ai 的 `pkg/routing/quicrouter.go` 作为 ttsignal 的服务端对等体，接受 ALPN `"ttsignal"` 连接，解析 SMP 帧 | 修改 SMP 帧格式、ALPN 协商流程、连接参数时，需同步更新 livekit-ai 的 Go 侧 QUIC 路由层 |
| **rtc-dashboard** | `../rtc-dashboard` | rtc-dashboard 的 `pkg/routing` 也实现了 ttsignal 连接接收；`js/upload-livekit-bin.js` 使用本项目上传部署包 | 修改 RPC 命令格式 (TTCmd)、文件传输协议时，需同步更新 rtc-dashboard 的命令路由 |

### 关键协议约定（跨项目共享）

- **ALPN 规则**: ALPN 始终是 `'ttsignal'`，无论 Connector 还是 Connection 级别。代码中 `createConnection({ alpn: 'h3' })` 的 `alpn` 参数不代表实际 QUIC ALPN 协商值，**不要误以为 ALPN 是 `'h3'`**——这是反复验证过的纠错经验。
- **SMP 帧协议**: Command/Message/UserControl 三类帧，rtc-client 和 livekit-ai 都依赖此格式。变更帧结构是破坏性修改。
- **getFile RPC**: 用于日志拉取的关键路径。path 参数格式为 `/api/stats/room/<roomId>`，由 livekit-ai TTCmd 中继到 rtc-dashboard。
- **Node.js 加载**: `src/js/index.js` 通过 `process.ttsBuildType` ('debug'/'release') 选择加载哪个 .node 文件。rtc-client 在 import 前设置此变量。
