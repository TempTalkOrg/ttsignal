package org.difft.android.smp;

import java.nio.charset.Charset;
import java.util.Date;

public class Stream {
    private Connection connection;
    private int streamId;
    private Object userObject;
    private static Charset utf8 = Charset.forName("UTF-8");

    public Stream(Connection connection, int streamId) {
        this.connection = connection;
        this.streamId = streamId;
    }

    public boolean isClosed() {
        return connection == null;
    }

    public int id() {
        return streamId;
    }

    public int sendCmd(int transId, byte[] data) {
        if (isClosed()) {
            return -1;
        }
        return connection.sendPacket(Const.PTYPE_CMD, (new Date()).getTime(), transId, streamId, data);
    }

    public int sendData(byte[] data) {
        if (isClosed()) {
            return -1;
        }
        return connection.sendPacket(Const.PTYPE_DATA, (new Date()).getTime(), 0, streamId, data);
    }

    public int sendText(String text) {
        if (isClosed()) {
            return -1;
        }
        return connection.sendPacket(Const.PTYPE_DATA, (new Date()).getTime(), 0, streamId, text.getBytes(utf8));
    }

    public Object setUserObject(Object obj) {
        Object old = userObject;
        userObject = obj;
        return old;
    }

    public Object getUserObject() {
        return userObject;
    }

    public void close() {
        if (isClosed()) {
            return;
        }
        connection.closeStream(this.streamId);
        this.connection = null;
        this.streamId = 0;
    }
}
