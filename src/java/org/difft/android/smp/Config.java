///////////////////////////////////////////////////////////////////////////////
// file : Config.java
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

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
    public String logFile = "";
    public LogHandler logHandler = null;
    // Log level, error : E, info : I, debug : D, trace : T, warn : W
    public int logLevel = 0;

    public Config() {
        // Default constructor
    }
}
