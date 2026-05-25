/// ////////////////////////////////////////////////////////////////////////////
// file : Connector.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

import java.util.HashMap;

public class Connector {
    private static native long initialize(Connector self, Config config);

    private static native long createConnection(long handle, Config config);

    private static native HashMap<String, Integer> getStats(long handle);

    private static native void close(long handle);

    private static native void destroy(long handle);

    static {
        System.loadLibrary("signal");
    }

    protected long handle;
    private long readyToDeleteHandle = 0;

    public Connector(Config config) {
        handle = initialize(this, config);
    }

    public Connection createConnection(Config config, IConnectionHandler handler) {
        long newHandle = createConnection(handle, config);
        if (newHandle == 0)
            return null;
        return new Connection(newHandle, handler);
    }

    public HashMap<String, Integer> getStats() {
        if (isClosed()) {
            return null;
        }
        return getStats(handle);
    }

    public void close() {
        if (isClosed()) {
            return;
        }
        close(handle);
        readyToDeleteHandle = handle;
        handle = 0;
    }

    public boolean isClosed() {
        return handle == 0;
    }

    protected void finalize() {
        if (readyToDeleteHandle != 0) {
            System.out.println("Connector.finalize called(pid:" + Thread.currentThread().getId() + ")");
            destroy(readyToDeleteHandle);
            readyToDeleteHandle = 0;
        }
    }
}
