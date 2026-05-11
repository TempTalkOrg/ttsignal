//
//  QUICClientDemo.swift
//  QUICTest (Plan C — TTSignal.xcframework + xquic)
//
//  Demo wiring:
//  - TTSignalConnector / TTSignalConnection / TTSignalStream replace the old
//    QUICClient (Plan A, NWConnection + Apple QUIC).
//  - The bridge's AppleNetworkMonitor watches NWPathMonitor internally and
//    fires SMPConnection::Restart automatically on cellular ↔ wifi switches —
//    the app does NOT need to listen for path changes itself. The migration
//    result is delivered via onRestart(...) and surfaced as `restartCount`
//    + `lastRestartAddress` for the UI.
//  - Periodic 5s heartbeat so we can visually confirm RX/TX throughout a
//    network change.
//

import Combine
import Foundation

// MARK: - SharedConnector

/// Process-wide singleton for `TTSignalConnector`. The connector wraps the
/// xquic engine, runtime worker threads, the shared `AppleNetworkMonitor`
/// and the per-connector log appender — all of which are app-global
/// resources that should be created once and reused for every Connection.
///
/// Recreating the connector on every Connect tap is wasteful (extra
/// runtime threads, repeated `NWPathMonitor` start) and, more
/// importantly, races with the in-flight tear-down of the previous
/// connector: the new connector's path monitor immediately fires a
/// `setsockopt(IP_BOUND_IF)` mid-handshake on the freshly created
/// socket, and the server side often still holds state for the
/// previous connection (same `cidTag`), causing the new connection's
/// init RPC to be silently dropped and the demo's 15s timer to fire.
///
/// Holding the connector for the lifetime of the app side-steps both
/// problems: only one path monitor exists, only one xquic engine
/// exists, and per-connection state is the only thing that comes and
/// goes.
@MainActor
enum SharedConnector {
    private static var instance: TTSignalConnector?

    /// Log level the current `instance` was built with. The connector's
    /// log filter is fixed at construction time (see SMPConnector::Init →
    /// AddExternalLogAppender), so any change requires tearing the
    /// connector down and creating a new one. We remember the value here
    /// so the UI can detect a mismatch and trigger a rebuild.
    private(set) static var currentLogLevel: TTSignalConfig.LogLevel = .warn

    /// Lazily create the connector on first use, or rebuild it if the
    /// caller asks for a different `logLevel` than the live instance was
    /// built with. Returns `nil` if init fails — the caller surfaces
    /// this through the UI.
    static func obtain(logLevel: TTSignalConfig.LogLevel) -> TTSignalConnector? {
        if let instance, currentLogLevel == logLevel { return instance }
        // Either first-time creation, or the caller wants a different
        // log level → drop the existing connector first. We use the
        // *synchronous* close so the engine's worker threads have
        // finished destroying themselves before deinit / next-build
        // runs `tt_connector_destroy` (which deletes the SMPConnector).
        // The plain async close otherwise races destroy and crashes
        // any in-flight worker tasks. 3s is generous for the iOS
        // demo — bump it if you ever see TIMEDOUT in the log sink.
        instance?.closeSync(timeoutMs: 3000)
        instance = nil
        var config = TTSignalConfig()
        config.alpn = "ttsignal"
        config.idleTimeOut = 30000
        config.pingOn = true
        config.pingInterval = 10000
        config.taskThreads = 1
        config.timerThreads = 1
        config.logLevel = logLevel
        guard let connector = TTSignalConnector(config: config) else {
            return nil
        }
        instance = connector
        currentLogLevel = logLevel
        return connector
    }

    /// Drop the live connector if the requested log level differs from
    /// the one it was built with. Returns `true` when the instance was
    /// torn down, so callers can also drop any per-connection handler
    /// they were holding. Called by the UI when the picker changes,
    /// before the next `obtain(logLevel:)` rebuilds.
    @discardableResult
    static func resetIfLogLevelChanged(_ newLevel: TTSignalConfig.LogLevel) -> Bool {
        guard instance != nil, currentLogLevel != newLevel else { return false }
        // Synchronous teardown — see the matching note in obtain(...).
        // We need OnClosed to fire before clearing `instance`, otherwise
        // tt_connector_destroy in the wrapper's deinit can land while
        // worker threads are still pumping engine state.
        instance?.closeSync(timeoutMs: 3000)
        instance = nil
        currentLogLevel = newLevel
        return true
    }
}

// MARK: - ConnectionStats

/// Observable state for ContentView. Every write happens on MainActor.
///
/// The stats object is shared across the lifetime of the ContentView, but
/// at any given moment it belongs to exactly **one** `ApplicationHandler`
/// — the "active" connection. When the user kicks off a new Connect while
/// the previous Connection is still draining its async callbacks (most
/// notably `onClosed`), those late callbacks would otherwise overwrite
/// the new connection's CONNECTED state with CLOSED. The `owner` gate
/// below filters them out so a stale handler can't clobber the UI.
@MainActor
final class ConnectionStats: ObservableObject {
    @Published var state: String = "INIT"
    @Published var lastRestartAddress: String = "—"
    @Published var sentCount: Int = 0
    @Published var receivedCount: Int = 0

    /// How many times the bridge auto-restarted the QUIC socket because of
    /// a path change. > 0 means at least one cellular ↔ wifi transition
    /// completed without forcing the upper layer to reconnect.
    @Published var restartCount: Int = 0

    @Published var lastError: String = ""

    /// The handler that currently owns these stats. Held weakly to avoid
    /// pinning a torn-down handler alive. Writes coming from any other
    /// handler are dropped on the floor by `mutate(from:_:)`.
    private weak var owner: AnyObject?

    /// Reset all values *and* unbind the current owner. Called from the
    /// UI before a new connect, so that any in-flight callback from the
    /// previous handler will find that it's no longer the owner and
    /// silently drop its mutation.
    func reset() {
        owner = nil
        state = "INIT"
        lastRestartAddress = "—"
        sentCount = 0
        receivedCount = 0
        restartCount = 0
        lastError = ""
    }

    /// Bind these stats to a freshly-created handler. From this point on,
    /// only callbacks coming from `owner` may mutate the published
    /// values via `mutate(from:_:)`.
    func bind(to owner: AnyObject) {
        self.owner = owner
    }

    /// Apply `work` only if `caller` is still the active owner. This is
    /// the gate that prevents a previous connection's late `onClosed`
    /// from clobbering the new connection's UI state.
    func mutate(from caller: AnyObject, _ work: (ConnectionStats) -> Void) {
        guard owner === caller else { return }
        work(self)
    }
}

// MARK: - ApplicationHandler

/// TTSignalHandler implementation that bridges native callbacks to
/// ConnectionStats + NotificationCenter logs. Callbacks fire on internal
/// worker threads so every UI write is hopped to MainActor explicitly.
final class ApplicationHandler: NSObject, TTSignalHandler, Loggable {
    let connection: TTSignalConnection
    let stats: ConnectionStats

    private var sendTimer: DispatchSourceTimer?
    private let sendInterval: TimeInterval = 5.0

    /// Server-pushed stream. Captured on the first onStreamCreated so the
    /// "Send Test" button has somewhere to write into.
    private var primaryStream: TTSignalStream?
    private let streamLock = NSLock()

    init(connection: TTSignalConnection, stats: ConnectionStats) {
        self.connection = connection
        self.stats = stats
        super.init()
    }

    /// Close the per-connection resources only. The shared
    /// `TTSignalConnector` is owned by `SharedConnector` and survives
    /// across reconnects, so it is intentionally NOT closed here.
    func close() {
        stopSendingTimer()
        connection.close()
    }

    /// Hop to MainActor before mutating Published values, and route the
    /// write through the stats' owner gate so that a callback fired by a
    /// previously-detached handler can't overwrite the active
    /// connection's state.
    private func mutateStats(_ work: @escaping @MainActor (ConnectionStats) -> Void) {
        let stats = stats
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            MainActor.assumeIsolated {
                stats.mutate(from: self) { work($0) }
            }
        }
    }

    // MARK: Heartbeat — write a timestamp once a connection is up so we can
    // see RX/TX continue across migrations.

    private func startSendingTimer() {
        stopSendingTimer()
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now() + sendInterval, repeating: sendInterval)
        timer.setEventHandler { [weak self] in self?.sendCurrentTimestamp() }
        sendTimer = timer
        timer.resume()
    }

    private func stopSendingTimer() {
        sendTimer?.cancel()
        sendTimer = nil
    }

    private func sendCurrentTimestamp() {
        let ts = Date().timeIntervalSince1970
        let text = "\(ts)"
        sendOnPrimaryStream(text: text)
    }

    /// Used by UI's "Send Test" button.
    @MainActor
    func sendManualTest() {
        let payload = "manual-\(Date().timeIntervalSince1970)"
        sendOnPrimaryStream(text: payload)
    }

    private func sendOnPrimaryStream(text: String) {
        streamLock.lock(); let s = primaryStream; streamLock.unlock()
        guard let stream = s else {
            log("[Demo] no stream yet — drop \(text)", .warning)
            return
        }
        let rc = stream.sendText(text)
        if rc != 0 {
            log("[Demo] sendText rc=\(rc)", .warning)
        }
    }

    // MARK: TTSignalHandler

    func onConnectResult(_ connection: TTSignalConnection, error: Int32, message: String?) {
        log("[Demo] onConnectResult err=\(error) msg=\(message ?? "")")
        if error == 0 {
            mutateStats { $0.state = "CONNECTED" }
            startSendingTimer()
        } else {
            mutateStats {
                $0.state = "FAILED"
                $0.lastError = "connect error \(error) \(message ?? "")"
            }
        }
    }

    func onStreamCreated(_ connection: TTSignalConnection, stream: TTSignalStream) {
        log("[Demo] streamCreated id=\(stream.id)")
        streamLock.lock()
        if primaryStream == nil { primaryStream = stream }
        streamLock.unlock()
    }

    func onStreamClosed(_ connection: TTSignalConnection, stream: TTSignalStream) {
        log("[Demo] streamClosed id=\(stream.id)")
        streamLock.lock()
        if primaryStream === stream { primaryStream = nil }
        streamLock.unlock()
    }

    func onStreamDataSent(_: TTSignalConnection,
                          stream _: TTSignalStream,
                          transId _: Int32,
                          size: Int32)
    {
        mutateStats { $0.sentCount += 1 }
    }

    func onRecvCmd(_: TTSignalConnection,
                   timestamp _: Int64,
                   transId _: Int32,
                   stream _: TTSignalStream,
                   data: Data)
    {
        let preview = String(data: data, encoding: .utf8) ?? "<binary \(data.count)B>"
        log("[Demo] recvCmd \(data.count)B: \(preview)")
        mutateStats { $0.receivedCount += 1 }
    }

    func onRecvData(_: TTSignalConnection,
                    timestamp _: Int64,
                    transId _: Int32,
                    stream _: TTSignalStream,
                    data: Data)
    {
        let preview = String(data: data, encoding: .utf8) ?? "<binary \(data.count)B>"
        log("[Demo] recvData \(data.count)B: \(preview)")
        mutateStats { $0.receivedCount += 1 }
    }

    func onRestart(_: TTSignalConnection, result: Int32, address: String?) {
        // The bridge's AppleNetworkMonitor drives the restart entirely on
        // its own; we just surface the outcome so the UI shows that the
        // migration completed without any upper-layer reconnect.
        let addr = address ?? "?"
        log("[Demo] onRestart result=\(result) addr=\(addr)")
        mutateStats {
            $0.restartCount += 1
            $0.lastRestartAddress = addr
        }
    }

    func onClosed(_: TTSignalConnection, reason: String?) {
        log("[Demo] onClosed reason=\(reason ?? "")")
        stopSendingTimer()
        mutateStats { $0.state = "CLOSED" }
    }

    func onException(_: TTSignalConnection, errMsg: String) {
        log("[Demo] onException \(errMsg)", .error)
        mutateStats {
            $0.state = "FAILED"
            $0.lastError = errMsg
        }
    }
}

// MARK: - Helpers

/// One-shot helper called from ContentView. Reuses the shared
/// `TTSignalConnector` and spins up a fresh Connection; returns the
/// resulting handler so the UI can hold on to it (and tear it down on
/// Disconnect). The connector itself is never closed here.
@MainActor
func connectQUICServer(url: String,
                       args: [String: Any],
                       logLevel: TTSignalConfig.LogLevel,
                       stats: ConnectionStats) -> ApplicationHandler?
{
    guard let connector = SharedConnector.obtain(logLevel: logLevel) else {
        stats.state = "FAILED"
        stats.lastError = "TTSignalConnector init returned nil"
        return nil
    }

    // The handler is created lazily because TTSignalConnection is created
    // first (it needs to be passed in). We jump through a placeholder so
    // the closure captures the right instance.
    final class HandlerHolder {
        var handler: ApplicationHandler?
    }
    let holder = HandlerHolder()

    final class Trampoline: TTSignalHandler {
        let holder: HandlerHolder
        init(_ h: HandlerHolder) { self.holder = h }
        func onConnectResult(_ c: TTSignalConnection, error: Int32, message: String?) {
            holder.handler?.onConnectResult(c, error: error, message: message)
        }
        func onStreamCreated(_ c: TTSignalConnection, stream: TTSignalStream) {
            holder.handler?.onStreamCreated(c, stream: stream)
        }
        func onStreamClosed(_ c: TTSignalConnection, stream: TTSignalStream) {
            holder.handler?.onStreamClosed(c, stream: stream)
        }
        func onStreamDataSent(_ c: TTSignalConnection, stream: TTSignalStream, transId: Int32, size: Int32) {
            holder.handler?.onStreamDataSent(c, stream: stream, transId: transId, size: size)
        }
        func onRecvCmd(_ c: TTSignalConnection, timestamp: Int64, transId: Int32, stream: TTSignalStream, data: Data) {
            holder.handler?.onRecvCmd(c, timestamp: timestamp, transId: transId, stream: stream, data: data)
        }
        func onRecvData(_ c: TTSignalConnection, timestamp: Int64, transId: Int32, stream: TTSignalStream, data: Data) {
            holder.handler?.onRecvData(c, timestamp: timestamp, transId: transId, stream: stream, data: data)
        }
        func onRestart(_ c: TTSignalConnection, result: Int32, address: String?) {
            holder.handler?.onRestart(c, result: result, address: address)
        }
        func onClosed(_ c: TTSignalConnection, reason: String?) {
            holder.handler?.onClosed(c, reason: reason)
        }
        func onException(_ c: TTSignalConnection, errMsg: String) {
            holder.handler?.onException(c, errMsg: errMsg)
        }
    }
    let trampoline = Trampoline(holder)

    var connConfig = TTSignalConfig()
    connConfig.alpn = "ttsignal"
    if let parsed = URL(string: url), let host = parsed.host {
        connConfig.serverHost = host
    } else {
        connConfig.serverHost = "tlivekit9tcew3gy.test.chative.im"
    }
    connConfig.cidTag = "12345678900"
    connConfig.deviceType = 1
    guard let connection = connector.createConnection(config: connConfig, handler: trampoline) else {
        // Do NOT close the shared connector — it stays alive for the
        // next attempt.
        stats.state = "FAILED"
        stats.lastError = "createConnection returned nil"
        return nil
    }

    let appHandler = ApplicationHandler(connection: connection, stats: stats)
    holder.handler = appHandler
    // Take ownership of the stats so subsequent callbacks from any
    // previously-closed handler are filtered out by mutate(from:).
    stats.bind(to: appHandler)

    // Build props JSON from caller-supplied dict (e.g. Authorization).
    let propsJson: String
    if !args.isEmpty,
       let data = try? JSONSerialization.data(withJSONObject: args,
                                              options: [.sortedKeys]),
       let s = String(data: data, encoding: .utf8)
    {
        propsJson = s
    } else {
        propsJson = "{}"
    }

    stats.state = "CONNECTING"
    let rc = connection.connect(url: url, propsJson: propsJson, timeoutMs: 15000)
    if rc != 0 {
        stats.state = "FAILED"
        stats.lastError = "connect() returned \(rc)"
    }
    return appHandler
}
