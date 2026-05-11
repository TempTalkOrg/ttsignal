///////////////////////////////////////////////////////////////////////////////
// file : TTSignalConnection.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/Connection.java. Plus an
// extra `restart(interface:)` overload that takes an NWInterface so callers
// can drive an explicit migration if they want to (typically they shouldn't
// — the bridge's AppleNetworkMonitor handles it automatically).
///////////////////////////////////////////////////////////////////////////////

import Foundation
import Network
import TTSignalC

public final class TTSignalConnection {

    /// Thread-safe map of streamId → TTSignalStream. C callbacks fire on
    /// arbitrary worker threads, so all access is funnelled through an
    /// internal queue.
    private var streams: [Int32: TTSignalStream] = [:]
    private let streamsLock = NSLock()

    private  weak var connector: TTSignalConnector?
    fileprivate let   handler: TTSignalHandler
    private  var      handle: TTConnectionRef?
    private  var      vtableBox: VTableBox?
    public   var      userObject: Any?

    /// Convenience: do we still hold a live native handle?
    public var isClosed: Bool { handle == nil }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    init(connector: TTSignalConnector,
         config: TTSignalConfig,
         handler: TTSignalHandler)
    {
        self.connector = connector
        self.handler   = handler

        let box = VTableBox()
        self.vtableBox = box

        // Set up vtable callback closures while `box.owner` is still nil —
        // the closures capture `box` weakly via the userdata pointer trick
        // (Unmanaged).
        var vt = TTHandlerVTable()
        vt.on_connect_result    = vtable_on_connect_result
        vt.on_stream_created    = vtable_on_stream_created
        vt.on_stream_closed     = vtable_on_stream_closed
        vt.on_stream_data_acked = vtable_on_stream_data_acked
        vt.on_stream_data_sent  = vtable_on_stream_data_sent
        vt.on_recv_cmd          = vtable_on_recv_cmd
        vt.on_recv_data         = vtable_on_recv_data
        vt.on_restart           = vtable_on_restart
        vt.on_closed            = vtable_on_closed
        vt.on_exception         = vtable_on_exception
        box.vtable = vt

        // Wire box.owner = self before the bridge can fire any callback.
        // Connector is required to actually open the connection.
        guard let cref = connector.rawHandle else {
            self.handle = nil
            return
        }

        let userdata = Unmanaged.passUnretained(box).toOpaque()
        self.handle = config.withCConfig { cfg -> TTConnectionRef? in
            withUnsafePointer(to: &box.vtable) { vtPtr in
                tt_connector_create_connection(cref, cfg, vtPtr, userdata)
            }
        }
        box.owner = self
    }

    deinit {
        // Final teardown if the user forgot to call close().
        if let h = handle {
            tt_connection_destroy(h)
            handle = nil
        }
    }

    // -------------------------------------------------------------------------
    // Public API — mirrors Connection.java method-for-method
    // -------------------------------------------------------------------------

    /// Returns BC_R_SUCCESS (0) on synchronous success — the actual
    /// connection result arrives asynchronously via onConnectResult().
    /// `propsJson` is a JSON object string forwarded verbatim to the
    /// server in the SMP handshake (matches Connection.connect's `props`).
    @discardableResult
    public func connect(url: String,
                        propsJson: String = "{}",
                        timeoutMs: Int32 = 0) -> Int32
    {
        guard let h = handle else { return -1 }
        return url.withCString { urlPtr in
            propsJson.withCString { propPtr in
                tt_connection_connect(h, urlPtr, propPtr, timeoutMs)
            }
        }
    }

    @discardableResult
    public func sendPacket(_ packet: TTSignalPacket) -> Int32 {
        guard let h = handle else { return -1 }
        return tt_connection_send_packet(h, packet.rawHandle)
    }

    /// Build + send a one-shot packet. Used by Stream.sendCmd / sendData.
    @discardableResult
    public func sendPacket(type: TTSignalPacket.PacketType,
                           timestamp: Int64,
                           transId: Int32,
                           streamId: Int32,
                           payload: Data) -> Int32
    {
        let pkt = TTSignalPacket(type: type,
                                 timestamp: timestamp,
                                 transId: transId,
                                 streamId: streamId,
                                 payload: payload)
        return sendPacket(pkt)
    }

    public func closeStream(streamId: Int32) {
        guard let h = handle else { return }
        tt_connection_close_stream(h, streamId)
    }

    /// Force a QUIC migration. Pass an NWInterface to bind to a specific
    /// interface (the bridge calls if_nametoindex on its name and feeds the
    /// ifIndex through SMPConnection::Restart -> UDPSender::Restart with
    /// IP_BOUND_IF). Pass nil to keep the existing interface (matches
    /// Android's restart()).
    ///
    /// Application code typically does NOT need to call this — the bridge's
    /// internal NWPathMonitor invokes restart automatically on path changes.
    public func restart(interface: NWInterface? = nil) {
        guard let h = handle else { return }
        var ifIndex: Int64 = 0
        if let iface = interface {
            // NWInterface doesn't directly expose its system index, but
            // its `name` is the BSD interface name (en0 / pdp_ip0 / etc.)
            // which if_nametoindex resolves to the same numeric index used
            // by the kernel for setsockopt(IP_BOUND_IF).
            ifIndex = iface.name.withCString { Int64(if_nametoindex($0)) }
        }
        tt_connection_restart(h, ifIndex)
    }

    public func close() {
        guard let h = handle else { return }
        tt_connection_close(h)
        // We don't destroy here — destroy happens in deinit, after
        // onClosed has fired and the engine has actually torn down. This
        // matches Java's two-phase Close + finalize.
    }

    // -------------------------------------------------------------------------
    // Stream registry — mirrors Connection.java's `streams` HashMap
    // -------------------------------------------------------------------------

    fileprivate func upsertStream(id: Int32) -> TTSignalStream {
        streamsLock.lock(); defer { streamsLock.unlock() }
        if let existing = streams[id] { return existing }
        let s = TTSignalStream(connection: self, id: id)
        streams[id] = s
        return s
    }

    fileprivate func removeStream(id: Int32) -> TTSignalStream? {
        streamsLock.lock(); defer { streamsLock.unlock() }
        let s = streams.removeValue(forKey: id)
        s?.detach()
        return s
    }

    fileprivate func lookupStream(id: Int32) -> TTSignalStream? {
        streamsLock.lock(); defer { streamsLock.unlock() }
        return streams[id]
    }

    fileprivate func clearHandle() {
        handle = nil
    }
}

// =============================================================================
// VTable trampolines — module-level so they're plain C function pointers.
// =============================================================================

/// Holds the vtable + a strong-ish reference to the owning connection so the
/// callback userdata pointer stays valid for the engine's lifetime. We use
/// an unowned reference to avoid a retain cycle (the connection holds the
/// box, the box's userdata is passed back through C — opaque to C, but if
/// we used a strong ref the cycle would never break).
final class VTableBox {
    var vtable: TTHandlerVTable = TTHandlerVTable()
    weak var owner: TTSignalConnection? = nil
}

private func unbox(_ userdata: UnsafeMutableRawPointer?) -> TTSignalConnection? {
    guard let userdata = userdata else { return nil }
    let box = Unmanaged<VTableBox>.fromOpaque(userdata).takeUnretainedValue()
    return box.owner
}

private func vtable_on_connect_result(userdata: UnsafeMutableRawPointer?,
                                      error: Int32,
                                      message: UnsafePointer<CChar>?)
{
    guard let conn = unbox(userdata) else { return }
    let msg = message.flatMap { String(cString: $0) }
    conn.handler.onConnectResult(conn, error: error, message: msg)
}

private func vtable_on_stream_created(userdata: UnsafeMutableRawPointer?,
                                      streamId: Int32)
{
    guard let conn = unbox(userdata) else { return }
    let stream = conn.upsertStream(id: streamId)
    conn.handler.onStreamCreated(conn, stream: stream)
}

private func vtable_on_stream_closed(userdata: UnsafeMutableRawPointer?,
                                     streamId: Int32)
{
    guard let conn = unbox(userdata) else { return }
    if let stream = conn.removeStream(id: streamId) {
        conn.handler.onStreamClosed(conn, stream: stream)
    }
}

private func vtable_on_stream_data_acked(userdata: UnsafeMutableRawPointer?,
                                         streamId: Int32,
                                         ackDelayUs: Int64,
                                         ackedBytes: Int32,
                                         inflightBytes: Int32)
{
    guard let conn = unbox(userdata) else { return }
    guard let stream = conn.lookupStream(id: streamId) else { return }
    conn.handler.onStreamDataAcked(conn,
                                   stream: stream,
                                   ackDelayUs: ackDelayUs,
                                   ackedBytes: ackedBytes,
                                   inflightBytes: inflightBytes)
}

private func vtable_on_stream_data_sent(userdata: UnsafeMutableRawPointer?,
                                        streamId: Int32,
                                        transId: Int32,
                                        size: Int32)
{
    guard let conn = unbox(userdata) else { return }
    guard let stream = conn.lookupStream(id: streamId) else { return }
    conn.handler.onStreamDataSent(conn,
                                  stream: stream,
                                  transId: transId,
                                  size: size)
}

private func vtable_on_recv_cmd(userdata: UnsafeMutableRawPointer?,
                                timestamp: Int64,
                                transId: Int32,
                                streamId: Int32,
                                data: UnsafePointer<UInt8>?,
                                len: Int)
{
    guard let conn = unbox(userdata) else { return }
    guard let stream = conn.lookupStream(id: streamId) else { return }
    let payload: Data = (data != nil && len > 0)
        ? Data(bytes: UnsafeRawPointer(data!), count: len)
        : Data()
    conn.handler.onRecvCmd(conn,
                           timestamp: timestamp,
                           transId: transId,
                           stream: stream,
                           data: payload)
}

private func vtable_on_recv_data(userdata: UnsafeMutableRawPointer?,
                                 timestamp: Int64,
                                 transId: Int32,
                                 streamId: Int32,
                                 data: UnsafePointer<UInt8>?,
                                 len: Int)
{
    guard let conn = unbox(userdata) else { return }
    guard let stream = conn.lookupStream(id: streamId) else { return }
    let payload: Data = (data != nil && len > 0)
        ? Data(bytes: UnsafeRawPointer(data!), count: len)
        : Data()
    conn.handler.onRecvData(conn,
                            timestamp: timestamp,
                            transId: transId,
                            stream: stream,
                            data: payload)
}

private func vtable_on_restart(userdata: UnsafeMutableRawPointer?,
                               result: Int32,
                               localAddr: UnsafePointer<CChar>?)
{
    guard let conn = unbox(userdata) else { return }
    let addr = localAddr.flatMap { String(cString: $0) }
    conn.handler.onRestart(conn, result: result, address: addr)
}

private func vtable_on_closed(userdata: UnsafeMutableRawPointer?,
                              reason: UnsafePointer<CChar>?)
{
    guard let conn = unbox(userdata) else { return }
    let r = reason.flatMap { String(cString: $0) }
    conn.clearHandle()
    conn.handler.onClosed(conn, reason: r)
}

private func vtable_on_exception(userdata: UnsafeMutableRawPointer?,
                                 errMsg: UnsafePointer<CChar>?)
{
    guard let conn = unbox(userdata) else { return }
    let msg = errMsg.flatMap { String(cString: $0) } ?? ""
    conn.handler.onException(conn, errMsg: msg)
}
