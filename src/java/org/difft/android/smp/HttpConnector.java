package org.difft.android.smp;

public final class HttpConnector implements AutoCloseable {
    private static native long initialize(HttpConfig config);

    private static native void request(long handle, HttpRequest request, HttpCallback callback);

    private static native void destroy(long handle);

    static {
        System.loadLibrary("signal");
    }

    private long handle;

    public HttpConnector(HttpConfig config) {
        handle = initialize(config);
        if (handle == 0) {
            throw new IllegalStateException("Failed to initialize TTSignal HTTP connector");
        }
    }

    public synchronized void request(HttpRequest request, HttpCallback callback) {
        if (handle == 0) {
            callback.onError(-1, "BC_R_SHUTTINGDOWN", "HTTP connector is closed");
            return;
        }
        request(handle, request, callback);
    }

    public synchronized boolean isClosed() {
        return handle == 0;
    }

    @Override
    public synchronized void close() {
        if (handle == 0) {
            return;
        }
        long closingHandle = handle;
        handle = 0;
        destroy(closingHandle);
    }
}
