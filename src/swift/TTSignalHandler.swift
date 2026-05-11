///////////////////////////////////////////////////////////////////////////////
// file : TTSignalHandler.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/IConnectionHandler.java.
// All callbacks are dispatched on internal worker threads — implementations
// must hop to MainActor / DispatchQueue.main themselves before touching UI.
///////////////////////////////////////////////////////////////////////////////

import Foundation

public protocol TTSignalHandler: AnyObject {
    /// Result of TTSignalConnection.connect(...). `error == 0` (BC_R_SUCCESS)
    /// means the QUIC handshake + SMP handshake both completed.
    func onConnectResult(_ connection: TTSignalConnection,
                         error: Int32,
                         message: String?)

    /// A new server-initiated stream landed on this connection.
    func onStreamCreated(_ connection: TTSignalConnection,
                         stream: TTSignalStream)

    /// A stream was closed (either side initiated).
    func onStreamClosed(_ connection: TTSignalConnection,
                        stream: TTSignalStream)

    /// QUIC ack accounting for traffic on a stream. Default no-op so simple
    /// callers don't have to implement it.
    func onStreamDataAcked(_ connection: TTSignalConnection,
                           stream: TTSignalStream,
                           ackDelayUs: Int64,
                           ackedBytes: Int32,
                           inflightBytes: Int32)

    /// Local data was handed off to the QUIC engine (not yet acked).
    func onStreamDataSent(_ connection: TTSignalConnection,
                          stream: TTSignalStream,
                          transId: Int32,
                          size: Int32)

    /// SMP CMD frame (PTYPE_CMD). Use for short text/json control payloads.
    func onRecvCmd(_ connection: TTSignalConnection,
                   timestamp: Int64,
                   transId: Int32,
                   stream: TTSignalStream,
                   data: Data)

    /// SMP DATA frame (PTYPE_DATA). Use for binary payloads (LiveKit
    /// signaling protobuf rides this channel).
    func onRecvData(_ connection: TTSignalConnection,
                    timestamp: Int64,
                    transId: Int32,
                    stream: TTSignalStream,
                    data: Data)

    /// QUIC migration finished (or failed). `address` is the new local
    /// 5-tuple address as a string (e.g. "192.168.1.5:54321"). On iOS this
    /// fires automatically as the result of NWPathMonitor flipping
    /// interfaces — no upper-layer call needed.
    func onRestart(_ connection: TTSignalConnection,
                   result: Int32,
                   address: String?)

    /// Connection has shut down. Once this fires the handle is invalid.
    func onClosed(_ connection: TTSignalConnection,
                  reason: String?)

    /// An exception (BCException) bubbled up from the engine.
    func onException(_ connection: TTSignalConnection,
                     errMsg: String)
}

public extension TTSignalHandler {
    func onStreamDataAcked(_ connection: TTSignalConnection,
                           stream: TTSignalStream,
                           ackDelayUs: Int64,
                           ackedBytes: Int32,
                           inflightBytes: Int32) {}

    func onStreamDataSent(_ connection: TTSignalConnection,
                          stream: TTSignalStream,
                          transId: Int32,
                          size: Int32) {}
}
