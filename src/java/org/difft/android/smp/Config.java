/// ////////////////////////////////////////////////////////////////////////////
// file : Config.java
// author : antoniozhou
/// ////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;


public class Config {
    public interface LogHandler {
        public void log(int level, String msg);
    }

    // Connector use config properties
    public String hostname = "localhost";
    // Server use config properties
    public int port = 8003;
    public int backlog = 1000;
    public boolean reusePort = false;
    public boolean ssl = false;
    public String privateKeyFile = "";
    public String certificateFile = "";
    // Server/Connector both use the same config properties
    public int taskThreads = 16;
    public int timerThreads = 4;
    // connection idle time out in milliseconds
    public int idleTimeOut = 20000;
    public String alpn = "ttsignal";
    public int maxConnections = 1000;
    public int congestCtrl = 'B';
    // ping on switch
    public boolean pingOn = false;
    // ping interval, in milliseconds
    public int pingInterval = 10000;
    // active connection id limit
    public int activeConnectionIdLimit = 1000;
    // device type, 1 : phone, 2 : PC
    public int deviceType = 0;
    // cid tag
    public String cidTag = "";
    public String logFile = "";
    public LogHandler logHandler = null;
    // Log level, error : E, info : I, debug : D, trace : T, warn : W
    public int logLevel = 0;
    // number of senders
    public int numOfSenders = 1;
    // Logical hostname for TLS SNI and certificate hostname verification (IP direct scenario)
    public String serverHost = "";
    // Self-signed root CA certificate in PEM format for custom certificate chain verification
    public String caCertPem = "";

    // -------- Outbound proxy (RFC 9298 CONNECT-UDP / MASQUE) --------
    // When configured, every connection created from this connector tunnels its
    // QUIC traffic through a MASQUE proxy instead of dialing the target directly.
    // proxyUrl is the primary input; proxyHost/proxyPort/proxySni override the
    // values parsed from it. Leave all empty/0 for a direct connection.
    // Accepted proxyUrl forms: "masque://host:port", "https://host:port",
    // "h3://host:port", or a bare "host:port" (defaults to MASQUE). Port
    // defaults to 443; IPv6 must be bracketed ("[2001:db8::1]:443").
    public String proxyUrl = "";
    // Proxy host/IP. Setting it alone (without proxyUrl) enables the MASQUE proxy.
    public String proxyHost = "";
    // Proxy port (default 443 when enabled via proxyUrl/proxyHost; 0 = unset).
    public int proxyPort = 0;
    // Outer TLS SNI presented to the proxy (defaults to proxyHost).
    public String proxySni = "";
    // Self-signed root CA (PEM) used to verify the outer hop to the proxy.
    // Empty = use the system trust store for the proxy's TLS certificate.
    public String proxyCaCertPem = "";
    // Base64 SHA-256 SPKI pin for the proxy's leaf certificate. When set, the
    // outer CONNECT-UDP hop is pinned to this public key (defense in depth on
    // top of normal chain verification). Empty = no pinning.
    public String spkiPin = "";

    public Config() {
        // Default constructor
    }
}
