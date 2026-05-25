/// ////////////////////////////////////////////////////////////////////////////
// file : IConnectionHandler.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

public interface IConnectionHandler {
    public void onConnectResult(Connection conn, int error, String message);

    public void onStreamCreated(Connection conn, Stream stream);

    public void onStreamClosed(Connection conn, Stream stream);

	public default void onStreamDataAcked(Connection conn, Stream stream, long ackDelayTime, int ackedBytes, int inFlightBytes) {}

	public default void onStreamDataSent(Connection conn, Stream stream, int transId, int size) {}

    public void onRecvCmd(Connection conn, long timestamp, int transId, Stream stream, byte[] data);

    public void onRecvData(Connection conn, long timestamp, int transId, Stream stream, byte[] data);

    public void onRestart(Connection conn, int result, String address);

    public void onClosed(Connection conn, String reason);

    public void onException(Connection conn, String errMsg);
}
