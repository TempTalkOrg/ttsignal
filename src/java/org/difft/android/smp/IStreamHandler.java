/// ////////////////////////////////////////////////////////////////////////////
// file : IStreamHandler.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

public interface IStreamHandler {
    public void onRecvCmd(long timestamp, int transId, int streamId, byte[] data);

    public void onRecvData(long timestamp, int transId, int streamId, byte[] data);

    public void onClosed(String reason);
}

