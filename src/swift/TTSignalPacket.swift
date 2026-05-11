///////////////////////////////////////////////////////////////////////////////
// file : TTSignalPacket.swift
// author : anto
//
// Swift mirror of src/java/org/difft/android/smp/Packet.java. SMP packet
// types match Const.PTYPE_*.
///////////////////////////////////////////////////////////////////////////////

import Foundation
import TTSignalC

public final class TTSignalPacket {

    /// SMP frame type — must match the Const.PTYPE_* table on every binding.
    public enum PacketType: UInt8 {
        case cmd          = 1
        case data         = 2
        case userControl  = 3
        case ping         = 4
        case pong         = 5
    }

    fileprivate let handle: TTPacketRef

    /// type, transId, streamId map to SMPHeader fields. timestamp == 0
    /// triggers bc_time_now() on the C side (matches the JNI default).
    public init(type: PacketType,
                timestamp: Int64 = 0,
                transId: Int32 = 0,
                streamId: Int32 = 0,
                payload: Data)
    {
        let h: TTPacketRef? = payload.withUnsafeBytes { rawBuf -> TTPacketRef? in
            let bytes = rawBuf.bindMemory(to: UInt8.self).baseAddress
            return tt_packet_create(type.rawValue,
                                    timestamp,
                                    transId,
                                    streamId,
                                    bytes,
                                    payload.count)
        }
        // Safe to force-unwrap — tt_packet_create only returns nil on OOM,
        // and the build will already be in trouble before that.
        precondition(h != nil, "tt_packet_create returned nil")
        self.handle = h!
    }

    deinit {
        tt_packet_destroy(handle)
    }

    // Internal — used by TTSignalConnection / Stream to feed the bridge.
    var rawHandle: TTPacketRef { handle }
}
