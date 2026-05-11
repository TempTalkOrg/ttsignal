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

    /// Off-switch for the bridge's built-in auto-restart on path
    /// changes. `false` (default) leaves AppleNetworkMonitor wired up
    /// to SMPConnection::Restart, so cellular ↔ wifi handoffs migrate
    /// the QUIC connection without app code lifting a finger. Set to
    /// `true` for server / long-lived deployments (mirrors the
    /// NAPI `config.disableAutoRestart`) where you'd rather opt out
    /// of NWPathMonitor jitter altogether and trigger restarts
    /// yourself via `TTSignalConnection.restart(interface:)`.
    public var disableAutoRestart: Bool        = false

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
        c.disableAutoRestart       = disableAutoRestart ? 1 : 0

        return withUnsafePointer(to: &c) { body($0) }
    }
}
