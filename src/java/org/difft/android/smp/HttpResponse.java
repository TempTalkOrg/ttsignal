package org.difft.android.smp;

import java.util.Map;

public final class HttpResponse {
    public final int status;
    public final String reason;
    public final Map<String, String> headers;
    public final byte[] body;
    public final String peerIp;
    public final long boundIfIndex;
    public final String pinMethod;

    public HttpResponse(
            int status,
            String reason,
            Map<String, String> headers,
            byte[] body,
            String peerIp,
            long boundIfIndex,
            String pinMethod) {
        this.status = status;
        this.reason = reason;
        this.headers = headers;
        this.body = body;
        this.peerIp = peerIp;
        this.boundIfIndex = boundIfIndex;
        this.pinMethod = pinMethod;
    }
}
