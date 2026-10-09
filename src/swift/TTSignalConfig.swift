///////////////////////////////////////////////////////////////////////////////
// file : TTSignalConfig.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/Config.java. Held by
// TTSignalConnector and forwarded down to the C bridge via a transient
// TTConfig POD.
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

/// VPN / 虚拟网卡选择策略。取代已废弃的 `TTSignalConfig.bypassVpn`。
///
/// rawValue 与 C 侧 `TTConfig.vpnPolicy` / `TTVpnPolicy` 一一对应，
/// 是跨 ABI 契约的一部分，不要改动。
///
/// 定义在顶层而非嵌套进 `TTSignalConfig`，与 binding 里其它 `TTSignal*`
/// 公共类型保持一致的命名风格。
public enum TTSignalVPNPolicy: Int32 {
    /// 跟随系统路由，允许 QUIC 流量走 VPN / utun 隧道，不安装任何网卡绑定。
    /// iOS 的默认值——装了 per-app VPN 的用户通常就是希望流量走 VPN。
    case os = 0
    /// 优先物理网卡（wifi / wired / cellular）。启动时找不到物理网卡可回落
    /// 隧道，运行中拒绝回落（保持当前 socket）。macOS / Windows / Linux 的
    /// 默认值。
    case preferPhysical = 1
    /// 只走物理网卡，任何阶段都不回落，也不撤销已安装的网卡绑定。找不到
    /// 物理网卡时 `connect` 立即失败并回调
    /// `BC_R_NO_PHYSICAL_INTERFACE`（64）。用于服务端必须观测到客户端真实
    /// IP 而非 VPN 出口 IP 的场景。
    case forcePhysical = 2
}

public struct TTSignalConfig {

    /// Levels match TTSignalLog.Level / Const.LOG_*. Values are the same
    /// integers used by the C side (1 = DEBUG, 5 = FATAL).
    public enum LogLevel: Int32 {
        case debug = 1
        case info  = 2
        case warn  = 3
        case error = 4
        case fatal = 5
    }

    /// Congestion control selector. The C side reads this as a single
    /// ASCII byte (matches Const.CC_BBR / CC_BBR2 / CC_CUBIC / CC_RENO).
    public enum CongestionControl: Int32 {
        case bbr   = 98   // 'b'
        case bbr2  = 66   // 'B'
        case cubic = 99   // 'c'
        case reno  = 114  // 'r'
    }

    // -------- Connector side --------
    public var hostname: String                = "localhost"

    // -------- Server side --------
    public var port: Int32                     = 8003
    public var backlog: Int32                  = 1000
    public var reusePort: Bool                 = false
    public var ssl: Bool                       = false
    public var privateKeyFile: String          = ""
    public var certificateFile: String         = ""

    // -------- Common --------
    public var taskThreads: Int32              = 16
    public var timerThreads: Int32             = 4
    public var idleTimeOut: Int32              = 20000
    public var alpn: String                    = "ttsignal"
    public var maxConnections: Int32           = 1000
    public var congestCtrl: CongestionControl  = .bbr2
    public var pingOn: Bool                    = false
    public var pingInterval: Int32             = 10000
    public var activeConnectionIdLimit: Int32  = 1000
    public var deviceType: Int32               = 0
    public var cidTag: String                  = ""
    public var logFile: String                 = ""
    public var logLevel: LogLevel              = .info
    public var numOfSenders: Int32             = 1
    public var serverHost: String              = ""
    public var caCertPem: String               = ""

    // -------- Outbound proxy (RFC 9298 CONNECT-UDP / MASQUE) --------
    /// When set, every connection created from the owning
    /// `TTSignalConnector` tunnels its QUIC traffic through a MASQUE
    /// proxy instead of dialing the target directly. `proxyUrl` is the
    /// primary input; `proxyHost` / `proxyPort` / `proxySni` override the
    /// values parsed from it. Leave all empty/0 for a direct connection.
    ///
    /// Accepted `proxyUrl` forms: `masque://host:port`,
    /// `https://host:port`, `h3://host:port`, or a bare `host:port`
    /// (defaults to MASQUE). Port defaults to 443; IPv6 must be bracketed
    /// (`[2001:db8::1]:443`).
    public var proxyUrl: String                = ""
    /// Proxy host/IP. Setting it alone (without `proxyUrl`) enables MASQUE.
    public var proxyHost: String               = ""
    /// Proxy port (0 = unset; defaults to 443 when the proxy is enabled).
    public var proxyPort: Int32                = 0
    /// Outer TLS SNI presented to the proxy (defaults to `proxyHost`).
    public var proxySni: String                = ""
    /// Self-signed root CA (PEM) used to verify the outer hop to the proxy.
    /// Empty = use the system trust store for the proxy's TLS certificate.
    public var proxyCaCertPem: String          = ""
    /// Base64 SHA-256 SPKI pin for the proxy's leaf certificate. When set,
    /// the outer CONNECT-UDP hop is pinned to this public key (defense in
    /// depth on top of normal chain verification). Empty = no pinning.
    public var spkiPin: String                 = ""

    /// Off-switch for the bridge's built-in auto-restart on path
    /// changes. `false` (default) leaves AppleNetworkMonitor wired up
    /// to SMPConnection::Restart, so cellular ↔ wifi handoffs migrate
    /// the QUIC connection without app code lifting a finger. Set to
    /// `true` for server / long-lived deployments (mirrors the
    /// NAPI `config.disableAutoRestart`) where you'd rather opt out
    /// of NWPathMonitor jitter altogether and trigger restarts
    /// yourself via `TTSignalConnection.restart(interface:)`.
    public var disableAutoRestart: Bool        = false

    /// VPN / 虚拟网卡选择策略。`nil` 表示用平台默认值（iOS 上是 `.os`，
    /// macOS / Windows / Linux 上是 `.preferPhysical`）。
    ///
    /// 与已废弃的 `bypassVpn` 同时设置时本属性胜出，原生层会打一条警告
    /// 日志。
    ///
    /// 用 `.forcePhysical` 时，如果当前没有可用物理网卡，`connect` 会
    /// 立即失败并通过 `TTSignalHandler.onConnectResult` 回调
    /// `BC_R_NO_PHYSICAL_INTERFACE`（64），不会等到超时。
    public var vpnPolicy: TTSignalVPNPolicy?   = nil

    /// 【已废弃】改用 `vpnPolicy`。
    ///
    /// 兼容映射：`false` → `.os`，`true` → `.preferPhysical`。
    ///
    /// 历史说明：这个开关此前只在 macOS 生效，iOS 上完全是空操作
    /// （`ResolveActiveIfIndex` 直接忽略它，取系统顺序第一块网卡）。
    /// 现在两个平台共用同一份实现，差别只体现在默认值上。
    @available(*, deprecated, message: "改用 vpnPolicy。false 等价于 .os，true 等价于 .preferPhysical")
    public var bypassVpn: Bool? {
        get { _bypassVpn }
        set { _bypassVpn = newValue }
    }

    /// `bypassVpn` 的实际存储。做成独立的非废弃属性，是为了让 binding 内部
    /// （withCConfig）能读它而不触发自身的废弃警告；调用方走公开的
    /// `bypassVpn` 时仍会正常收到废弃提示。
    internal var _bypassVpn: Bool?             = nil

    public init() {}

    /// Build a TTConfig POD with C-string-backed fields. The closure runs
    /// while the underlying Swift String storage is still alive — the
    /// pointer values become invalid after this returns. Always pass it
    /// straight into the C bridge (which copies into BCFObject internally).
    func withCConfig<R>(_ body: (UnsafePointer<TTConfig>) -> R) -> R {
        // Capture each Swift String's UTF-8 storage. CString lifetimes
        // extend until the surrounding closure returns (Swift retains the
        // wrappers in `_strings`).
        var strings: [ContiguousArray<CChar>] = []
        func cstr(_ s: String) -> UnsafePointer<CChar> {
            let arr = ContiguousArray(s.utf8CString)
            strings.append(arr)
            return strings.last!.withUnsafeBufferPointer { $0.baseAddress! }
        }

        var c = TTConfig()
        c.hostname                 = cstr(hostname)
        c.port                     = port
        c.backlog                  = backlog
        c.reusePort                = reusePort ? 1 : 0
        c.ssl                      = ssl ? 1 : 0
        c.privateKeyFile           = cstr(privateKeyFile)
        c.certificateFile          = cstr(certificateFile)
        c.taskThreads              = taskThreads
        c.timerThreads             = timerThreads
        c.idleTimeOut              = idleTimeOut
        c.alpn                     = cstr(alpn)
        c.maxConnections           = maxConnections
        c.congestCtrl              = congestCtrl.rawValue
        c.pingOn                   = pingOn ? 1 : 0
        c.pingInterval             = pingInterval
        c.activeConnectionIdLimit  = activeConnectionIdLimit
        c.deviceType               = deviceType
        c.cidTag                   = cstr(cidTag)
        c.logFile                  = cstr(logFile)
        c.logLevel                 = logLevel.rawValue
        c.numOfSenders             = numOfSenders
        c.serverHost               = cstr(serverHost)
        c.caCertPem                = cstr(caCertPem)
        c.proxyUrl                 = cstr(proxyUrl)
        c.proxyHost                = cstr(proxyHost)
        c.proxyPort                = proxyPort
        c.proxySni                 = cstr(proxySni)
        c.proxyCaCertPem           = cstr(proxyCaCertPem)
        c.spkiPin                  = cstr(spkiPin)
        c.disableAutoRestart       = disableAutoRestart ? 1 : 0
        // -1 sentinel = "use platform default" (see TTConfig.bypassVpn
        // doc in ios_bridge.h). Only flip to 0/1 when the app has
        // actually expressed a preference.
        //
        // bypassVpn 已废弃但仍然透传：原生层在两者都给出时以 vpnPolicy 为准
        // 并打 WARN，只给 bypassVpn 时走兼容映射。读内部存储 _bypassVpn 而非
        // 公开属性，避免 binding 自身产生废弃警告。
        c.bypassVpn                = _bypassVpn.map { $0 ? Int32(1) : Int32(0) } ?? Int32(-1)
        // 同样用 -1 表示未设置，让原生层回落到 bypassVpn 兼容映射 / 平台默认。
        c.vpnPolicy                = vpnPolicy?.rawValue ?? Int32(-1)

        return withUnsafePointer(to: &c) { body($0) }
    }
}
