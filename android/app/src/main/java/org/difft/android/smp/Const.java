///////////////////////////////////////////////////////////////////////////////
// file : Const.java
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

package org.difft.android.smp;

public class Const {
    public static byte PTYPE_CMD            = 1;
    public static byte PTYPE_DATA           = 2;
    public static byte PTYPE_USER_CONTROL   = 3;
    public static byte PTYPE_PING           = 4;
    public static byte PTYPE_PONG           = 5;
    public static int LOG_DEBUG             = 1;
    public static int LOG_INFO              = 2;
    public static int LOG_WARN              = 3;
    public static int LOG_ERROR             = 4;
    public static int LOG_FATAL             = 5;
    public static int CC_BBR                = 98;
    public static int CC_BBR2               = 66;
    public static int CC_CUBIC              = 99;
    public static int CC_RENO               = 114;
}
