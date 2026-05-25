///////////////////////////////////////////////////////////////////////////////
// file : TTSignalLog.swift
// author : anto
//
// Swift access to the bridge's logger. By default every log line goes to
// OSLog under subsystem "org.difft.ttsignal" / category "core". Apps that
// want to surface logs in their own UI (e.g. QUICTest) or pipe them into
// their own logging stack install a sink via `setSink` — installing a
// sink also silences the OSLog backend, so the host app owns log
// delivery exclusively and Console.app doesn't see duplicates of every
// line. Removing the sink (`setSink(nil)`) restores the OSLog default.
// Mirrors the Config.LogHandler functional interface in src/java/.../Config.java.
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

public enum TTSignalLog {

    public typealias LogSink = (TTSignalConfig.LogLevel, String) -> Void

    /// Last installed sink. Held here so we can keep the closure alive for
    /// the lifetime of its registration.
    private static var sinkBox: SinkBox?

    /// Install a Swift log sink. Pass `nil` to remove and restore the
    /// default OSLog backend. While a sink is installed the bridge sends
    /// log lines **only** to the sink — the OSLog forwarder is silenced,
    /// so the host app owns log delivery and Console.app no longer sees
    /// duplicates of every line you're already rendering / persisting.
    /// The sink may fire from any thread — implementers are responsible
    /// for thread-hopping before touching UI.
    public static func setSink(_ sink: LogSink?) {
        if let sink {
            let box = SinkBox(sink: sink)
            sinkBox = box
            let ud = Unmanaged.passUnretained(box).toOpaque()
            tt_logger_set_callback(log_trampoline, ud)
        } else {
            tt_logger_set_callback(nil, nil)
            sinkBox = nil
        }
    }
}

private final class SinkBox {
    let sink: TTSignalLog.LogSink
    init(sink: @escaping TTSignalLog.LogSink) { self.sink = sink }
}

/// The integer the C bridge passes to `TTLogCallback` is already the
/// public TTSignal log level (Const.LOG_* / Utils.h LOG_LEVEL_*, 1..5),
/// because `SMPConnector::log_callback` runs `BCLogLevelToLogLevel()`
/// before invoking `IConnectorHandler::OnLog`. That maps 1:1 onto the
/// raw values of `TTSignalConfig.LogLevel`, so we just construct it
/// directly — no remapping needed. Anything outside 1..5 (shouldn't
/// happen in practice) falls back to `.info`.
private func log_trampoline(userdata: UnsafeMutableRawPointer?,
                            level: Int32,
                            msg: UnsafePointer<CChar>?)
{
    guard let userdata = userdata else { return }
    let box = Unmanaged<SinkBox>.fromOpaque(userdata).takeUnretainedValue()
    let lvl = TTSignalConfig.LogLevel(rawValue: level) ?? .info
    let s = msg.flatMap { String(cString: $0) } ?? ""
    box.sink(lvl, s)
}
