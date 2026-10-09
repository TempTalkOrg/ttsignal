package org.difft.android.smp;

import java.util.HashMap;
import java.util.Map;

public class HttpRequest {
    public String method = "GET";
    public String url = "";
    public Map<String, String> headers = new HashMap<>();
    public byte[] body = null;
    public int timeoutMs = 10000;
    public String resolvedIp = "";
}
