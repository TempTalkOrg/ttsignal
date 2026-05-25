///////////////////////////////////////////////////////////////////////////////
// file : TTSignalConnector.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/Connector.java. Owns the
// shared QUIC engine and is the factory for TTSignalConnection objects.
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

public final class TTSignalConnector {

    private var handle: TTConnectorRef?
    public let config: TTSignalConfig

    public var isClosed: Bool { handle == nil }

    public init?(config: TTSignalConfig) {
        self.config = config
        let h = config.withCConfig { tt_connector_create($0) }
        guard let h else { return nil }
        self.handle = h
    }

    deinit {
        if let h = handle {
            tt_connector_destroy(h)
            handle = nil
        }
    }

    /// Spawn a new connection on the engine. handler is retained by the
    /// returned TTSignalConnection — keep that connection alive as long as
    /// you want to receive callbacks.
    public func createConnection(config: TTSignalConfig? = nil,
                                 handler: TTSignalHandler) -> TTSignalConnection?
    {
        guard handle != nil else { return nil }
        let conn = TTSignalConnection(connector: self,
                                      config: config ?? self.config,
                                      handler: handler)
        return conn.isClosed ? nil : conn
    }

    /// Returns the connector stats as a JSON object string (or nil if the
    /// connector is closed). Keys mirror SMPConnector::GetStats's map:
    ///   { "allocated_conn_size": N, "active_conn_size": N,
    ///     "freed_conn_size": N }
    public func getStats() -> String? {
        guard let h = handle else { return nil }
        var buf = [CChar](repeating: 0, count: 1024)
        let n = buf.withUnsafeMutableBufferPointer { bp -> Int in
            return tt_connector_get_stats(h, bp.baseAddress, bp.count)
        }
        guard n > 0 else { return nil }
        return String(cString: buf)
    }

    public func close() {
        guard let h = handle else { return }
        tt_connector_close(h)
        // We don't destroy here — destroy happens in deinit, mirroring
        // Connector.java's two-phase close + finalize.
    }

    /// Synchronous shutdown: kicks `tt_connector_close` and blocks the
    /// caller until the C++ engine has fired `IConnectorHandler::OnClosed`
    /// (i.e. xqc_engine_destroy + log appender removal are done) or
    /// `timeoutMs` elapses. Returns `true` on a clean close, `false` on
    /// timeout.
    ///
    /// Prefer this over `close()` whenever the next thing you do is
    /// drop the wrapper (so `deinit` calls `tt_connector_destroy`) or
    /// rebuild a new connector — for example, the QUICTest log-level
    /// switcher does both. The plain async `close()` otherwise races
    /// the destroy and can crash worker threads still touching the
    /// underlying SMPConnector.
    ///
    /// `timeoutMs <= 0` waits forever. MUST NOT be called from inside a
    /// `TTSignalHandler` callback (would deadlock the same worker pool
    /// that has to deliver the close notification).
    @discardableResult
    public func closeSync(timeoutMs: Int32 = 3000) -> Bool {
        guard let h = handle else { return true }
        // BC_R_SUCCESS == 0 (see deps/env/src/BC/Config.h).
        return tt_connector_close_sync(h, timeoutMs) == 0
    }

    // Internal — used by TTSignalConnection.init.
    var rawHandle: TTConnectorRef? { handle }
}
