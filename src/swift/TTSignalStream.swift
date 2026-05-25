///////////////////////////////////////////////////////////////////////////////
// file : TTSignalStream.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/Stream.java. Streams are
// owned by their parent TTSignalConnection — once the underlying QUIC
// stream closes the Connection clears the entry from its `streams` map.
///////////////////////////////////////////////////////////////////////////////

import Foundation

public final class TTSignalStream {
    private weak var connection: TTSignalConnection?
    public  let     id: Int32
    public  var     userObject: Any?

    init(connection: TTSignalConnection, id: Int32) {
        self.connection = connection
        self.id = id
    }

    public var isClosed: Bool { connection == nil }

    /// SMP CMD frame. Ignored if the stream / its connection is closed.
    @discardableResult
    public func sendCmd(transId: Int32, data: Data) -> Int32 {
        guard let conn = connection else { return -1 }
        return conn.sendPacket(type: .cmd,
                               timestamp: Int64(Date().timeIntervalSince1970 * 1000),
                               transId: transId,
                               streamId: id,
                               payload: data)
    }

    /// SMP DATA frame.
    @discardableResult
    public func sendData(_ data: Data) -> Int32 {
        guard let conn = connection else { return -1 }
        return conn.sendPacket(type: .data,
                               timestamp: Int64(Date().timeIntervalSince1970 * 1000),
                               transId: 0,
                               streamId: id,
                               payload: data)
    }

    /// Convenience for UTF-8 text payloads.
    @discardableResult
    public func sendText(_ text: String) -> Int32 {
        return sendData(Data(text.utf8))
    }

    public func close() {
        guard let conn = connection else { return }
        conn.closeStream(streamId: id)
        connection = nil
    }

    // Internal — Connection clears the back-pointer after onStreamClosed
    // so we don't try to send into a dead handle.
    func detach() { connection = nil }
}
