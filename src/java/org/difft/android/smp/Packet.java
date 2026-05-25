/// ////////////////////////////////////////////////////////////////////////////
// file : Packet.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

public class Packet {
    private long handle = 0;
    private boolean builded = false;

    private native long initialize();

    private native void build(long handle);

    private native void destroy(long handle);

    static {
        System.loadLibrary("signal");
    }

    public Packet() {
        handle = initialize();
    }

    public void build() {
        if (handle != 0) {
            build(handle);
            builded = true;
        }
    }

    public void buildOnce() {
        if (handle != 0 && !builded) {
            build(handle);
            builded = true;
        }
    }

    public boolean isBuilded() {
        return builded;
    }

    public long getHandle() {
        return handle;
    }

    protected void finalize() throws Throwable {
        if (handle != 0) {
            destroy(handle);
            handle = 0;
        }
    }

    // packet type
    public byte type = 0;
    // packet timestamp
    public long timestamp = 0;
    // packet transaction id
    public int transId = 0;
    // packet stream id
    public int streamId = 0;
    // packet payload
    public byte[] payload = null;
    // message retry intervals array（milliseconds）
    public int[] retry_intervals = null;
}
