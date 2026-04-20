/// ////////////////////////////////////////////////////////////////////////////
// file : Connection.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

import java.util.HashMap;

public class Connection {

    private long connectionHandle = 0;
    private long readyToDeleteHandle = 0;
    private IConnectionHandler handler;
    private HashMap<Integer, Stream> streams = new HashMap<Integer, Stream>();

    private native long initialize(long handle, Connection self, IHandler handler);

    private native int connect(long handle, String host, String props, int timeoutInMs);

    private native int sendPacket(long handle, long packet);

    private native void closeStream(long handle, int streamId);

    private native void restart(long handle, long networkHandle);

    private native void close(long handle);

    private native void destroy(long handle);

    static {
        System.loadLibrary("signal");
    }

    private Object userObject;

    public interface IHandler {
        public void onConnectResult(Connection conn, int error, String message);

        public void onStreamCreated(Connection conn, int streamId);

        public void onStreamClosed(Connection conn, int streamId);

        public void onStreamDataAcked(Connection conn, int streamId, long ackDelayTime, int ackedBytes, int inflightBytes);

        public void onStreamDataSent(Connection conn, int streamId, int transId, int size);

        public void onRecvCmd(Connection conn, long timestamp, int transId, int streamId, byte[] data);

        public void onRecvData(Connection conn, long timestamp, int transId, int streamId, byte[] data);

        public void onRestart(Connection conn, int result, String address);

        public void onClosed(Connection conn, String reason);

        public void onException(Connection conn, String errMsg);
    }

    public Connection(long handle, IConnectionHandler handler) {
        this.connectionHandle = initialize(handle, this, new IHandler() {
            @Override
            public void onConnectResult(Connection conn, int error, String message) {
                // 连接成功时的处理逻辑
                handler.onConnectResult(conn, error, message);
            }

            @Override
            public void onStreamCreated(Connection conn, int streamId) {
                // 新建流时的处理逻辑
                if (streams.containsKey(streamId)) {
                    return;
                }
                Stream stream = new Stream(conn, streamId);
                streams.put(streamId, stream);
                handler.onStreamCreated(conn, stream);
            }

            @Override
            public void onStreamClosed(Connection conn, int streamId) {
                // 流关闭时的处理逻辑
                System.out.println("raw QUIC stream closed, stream id : " + streamId);
                Stream stream = streams.remove(streamId);
                if (stream != null) {
                    handler.onStreamClosed(conn, stream);
                }
            }


            @Override
            public void onStreamDataAcked(Connection conn, int streamId, long ackDelayTime, int ackedBytes, int inflightBytes) {
                // 流包确认时的处理逻辑
                Stream stream = streams.get(streamId);
                if (stream != null) {
                    handler.onStreamDataAcked(conn, stream, ackDelayTime, ackedBytes, inflightBytes);
                }
            }

            @Override
            public void onStreamDataSent(Connection conn, int streamId, int transId, int size) {
                // 流数据发送时的处理逻辑
                Stream stream = streams.get(streamId);
                if (stream != null) {
                    handler.onStreamDataSent(conn, stream, transId, size);
                }
            }

            @Override
            public void onRecvCmd(Connection conn, long timestamp, int transId, int streamId, byte[] buffer) {
                // 收到命令时的处理逻辑
                Stream stream = streams.get(streamId);
                if (stream != null) {
                    handler.onRecvCmd(conn, timestamp, transId, stream, buffer);
                }
            }

            @Override
            public void onRecvData(Connection conn, long timestamp, int transId, int streamId, byte[] buffer) {
                // 收到数据时的处理逻辑
                Stream stream = streams.get(streamId);
                if (stream != null) {
                    handler.onRecvData(conn, timestamp, transId, stream, buffer);
                }
            }

            @Override
            public void onRestart(Connection conn, int result, String address) {
                handler.onRestart(conn, result, address);
            }

            @Override
            public void onClosed(Connection conn, String reason) {
                readyToDeleteHandle = connectionHandle;
                connectionHandle = 0;
                handler.onClosed(conn, reason);
            }

            @Override
            public void onException(Connection conn, String errorMsg) {
                handler.onException(conn, errorMsg);
            }
        });
        this.handler = handler;
    }

    public int connect(String host, String props, int timeoutInMs) {
        if (isClosed()) {
            return -1;
        }
        return connect(this.connectionHandle, host, props, timeoutInMs);
    }

    public boolean isClosed() {
        return connectionHandle == 0;
    }

    public int sendPacket(Packet packet) {
        if (isClosed()) {
            return -1;
        }
        packet.buildOnce();
        return sendPacket(this.connectionHandle, packet.getHandle());
    }

    public int sendPacket(byte type, long timestamp, int transId, int streamId, byte[] data) {
        if (isClosed()) {
            return -1;
        }
        Packet packet = new Packet();
        packet.type = type;
        packet.timestamp = timestamp;
        packet.transId = transId;
        packet.streamId = streamId;
        packet.payload = data;
        packet.buildOnce();
        return sendPacket(this.connectionHandle, packet.getHandle());
    }

    public int sendCmd(long timestamp, int transId, int streamId, byte[] data) {
        if (isClosed()) {
            return -1;
        }
        return sendPacket(Const.PTYPE_CMD, timestamp, transId, streamId, data);
    }

    public int sendData(long timestamp, int transId, int streamId, byte[] data) {
        if (isClosed()) {
            return -1;
        }
        return sendPacket(Const.PTYPE_DATA, timestamp, transId, streamId, data);
    }

    public void closeStream(int streamId) {
        if (isClosed()) {
            return;
        }
        closeStream(this.connectionHandle, streamId);
    }

    public Object setUserObject(Object obj) {
        Object old = userObject;
        userObject = obj;
        return old;
    }

    public Object getUserObject() {
        return userObject;
    }

    public void restart(long networkHandle) {
        if (isClosed()) {
            return;
        }
        restart(this.connectionHandle, networkHandle);
    }

    public void restart(){
        restart(0);
    }

    public void close() {
        if (isClosed()) {
            return;
        }
        close(this.connectionHandle);
        this.readyToDeleteHandle = this.connectionHandle;
        this.connectionHandle = 0;
    }

    protected void finalize() throws Throwable {
        if (readyToDeleteHandle != 0) {
            System.out.println("Connection.finalize called(pid:" + Thread.currentThread().getId() + ")");
            destroy(readyToDeleteHandle);
            readyToDeleteHandle = 0;
        }
    }
}
