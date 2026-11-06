



#include "WSConnector.h"
#include "WSParser.h"

#include <BC/BCException.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace WS;

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                         \
    do {                                                                       \
        std::string _a(actual);                                                \
        std::string _e(expected);                                              \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: 期望 \"%s\"，实际 \"%s\"\n",                 \
                   __FILE__, __LINE__, _e.c_str(), _a.c_str());                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        long long _a = (long long)(actual);                                    \
        long long _e = (long long)(expected);                                  \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: 期望 %lld，实际 %lld\n",                     \
                   __FILE__, __LINE__, _e, _a);                                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static bool Contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}






static std::string Flatten(BufferPtr buf)
{
    std::string out;
    BCBuffer    clone;
    buf->RefClone(&clone);
    uint32_t n = 0;
    void*    p = NULL;
    while ((p = clone.ReadBlock(INFINITE, n)) != NULL && n > 0)
    {
        out.append((const char*)p, n);
    }
    return out;
}


static std::string MakeFrame(bool fin, uint8_t opcode,
                             const std::string& payload,
                             const uint8_t* mask ,
                             uint8_t rsv = 0,
                             bool forceLen64 = false)
{
    std::string out;
    out.push_back((char)((fin ? 0x80 : 0x00) | (rsv << 4) | (opcode & 0x0F)));

    const size_t len     = payload.size();
    const uint8_t maskBit = mask ? 0x80 : 0x00;
    if (!forceLen64 && len < 126)
    {
        out.push_back((char)(maskBit | (uint8_t)len));
    }
    else if (!forceLen64 && len < 65536)
    {
        out.push_back((char)(maskBit | 126));
        out.push_back((char)((len >> 8) & 0xFF));
        out.push_back((char)(len & 0xFF));
    }
    else
    {
        out.push_back((char)(maskBit | 127));
        for (int i = 7; i >= 0; i--)
        {
            out.push_back((char)((len >> (i * 8)) & 0xFF));
        }
    }
    if (mask)
    {
        out.append((const char*)mask, 4);
        for (size_t i = 0; i < len; i++)
        {
            out.push_back((char)(payload[i] ^ mask[i % 4]));
        }
    }
    else
    {
        out.append(payload);
    }
    return out;
}



static std::string MakeHugeLenFrame(uint64_t declaredLen, bool msbSet)
{
    std::string out;
    out.push_back((char)(0x80 | 0x02));    
    out.push_back((char)127);
    uint64_t v = declaredLen;
    if (msbSet)
    {
        v |= (uint64_t)1 << 63;
    }
    for (int i = 7; i >= 0; i--)
    {
        out.push_back((char)((v >> (i * 8)) & 0xFF));
    }
    return out;
}





struct RecvFrame
{
    uint8_t     opcode = 0;
    std::string payload;
};

class TestParser : public WSParser
{
public:
    std::vector<RecvFrame> frames;
    std::vector<std::string> writes;

    
    bool Feed(const std::string& bytes)
    {
        try
        {
            ParseWSFrame(bytes.data(), bytes.size());
        }
        catch (BCException&)
        {
            return true;
        }
        catch (...)
        {
            return true;
        }
        return false;
    }

    bool FeedByteByByte(const std::string& bytes)
    {
        for (size_t i = 0; i < bytes.size(); i++)
        {
            try
            {
                ParseWSFrame(bytes.data() + i, 1);
            }
            catch (...)
            {
                return true;
            }
        }
        return false;
    }

protected:
    int OnWSWrite(std::shared_ptr<BCBuffer> data, void* user_data) override
    {
        (void)user_data;
        writes.push_back(Flatten(data));
        return (int)data->RemainingLength();
    }

    void OnRecvWSFrame(const WSFrameHeader& header, const uint8_t* payload,
                       size_t payload_len) override
    {
        RecvFrame f;
        f.opcode = header.opcode;
        if (payload && payload_len > 0)
        {
            f.payload.assign((const char*)payload, payload_len);
        }
        
        CHECK_EQ_INT(header.payload_size, payload_len);
        frames.push_back(f);
    }
};





static void test_ws_keys()
{
    printf("[1] WSGenKey / WSAcceptKey\n");

    
    CHECK_EQ_STR(WSParser::WSAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="),
                 "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    
    std::string k1 = WSParser::WSGenKey();
    std::string k2 = WSParser::WSGenKey();
    CHECK_EQ_INT(k1.size(), 24);
    CHECK(k1[23] == '=');
    CHECK(k1 != k2);
    for (size_t i = 0; i < k1.size(); i++)
    {
        char c = k1[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        CHECK(ok);
    }
    
    CHECK_EQ_INT(WSParser::WSAcceptKey(k1).size(), 28);
}





static void test_parse_url()
{
    printf("[2] ParseUrl\n");

    WSUrlParts  p;
    std::string err;

    CHECK(WSConnection::ParseUrl("ws://example.com/chat", p, err));
    CHECK_EQ_STR(p.scheme, "ws");
    CHECK_EQ_STR(p.host, "example.com");
    CHECK_EQ_STR(p.hostHeader, "example.com");
    CHECK_EQ_INT(p.port, 80);
    CHECK_EQ_STR(p.target, "/chat");
    CHECK(!p.tls);

    CHECK(WSConnection::ParseUrl("WSS://Example.COM", p, err));
    CHECK_EQ_STR(p.scheme, "wss");
    CHECK_EQ_INT(p.port, 443);
    CHECK(p.tls);
    
    CHECK_EQ_STR(p.target, "/");

    CHECK(WSConnection::ParseUrl("ws://127.0.0.1:8080/a?b=1#frag", p, err));
    CHECK_EQ_INT(p.port, 8080);
    CHECK_EQ_STR(p.hostHeader, "127.0.0.1:8080");
    
    CHECK_EQ_STR(p.target, "/a?b=1");

    
    CHECK(WSConnection::ParseUrl("wss://a.b:443/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "a.b");
    CHECK(WSConnection::ParseUrl("ws://a.b:80/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "a.b");

    
    CHECK(WSConnection::ParseUrl("wss://[2001:db8::1]:9443/s", p, err));
    CHECK_EQ_STR(p.host, "2001:db8::1");
    CHECK_EQ_STR(p.hostHeader, "[2001:db8::1]:9443");
    CHECK_EQ_INT(p.port, 9443);

    
    CHECK(!WSConnection::ParseUrl("http://example.com/", p, err));
    CHECK(Contains(err, "只支持 ws / wss"));
    CHECK(!WSConnection::ParseUrl("example.com/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://", p, err));
    CHECK(!WSConnection::ParseUrl("ws://u:pw@example.com/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://example.com:0/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://example.com:99999/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://2001:db8::1/", p, err));
    
    CHECK(!WSConnection::ParseUrl("ws://example.com/a b", p, err));
}





static void test_build_handshake()
{
    printf("[3] BuildHandshakeRequest\n");

    WSUrlParts  p;
    std::string err, out;
    CHECK(WSConnection::ParseUrl("ws://example.com:8080/chat?x=1", p, err));

    HttpHeaderMap extra;
    CHECK(WSConnection::BuildHandshakeRequest(p, "abc123==", extra, out, err));
    CHECK(Contains(out, "GET /chat?x=1 HTTP/1.1\r\n"));
    CHECK(Contains(out, "Host: example.com:8080\r\n"));
    CHECK(Contains(out, "Upgrade: websocket\r\n"));
    CHECK(Contains(out, "Connection: Upgrade\r\n"));
    CHECK(Contains(out, "Sec-WebSocket-Key: abc123==\r\n"));
    CHECK(Contains(out, "Sec-WebSocket-Version: 13\r\n"));
    CHECK(Contains(out, "User-Agent: ttsignal/1.0\r\n"));
    
    CHECK(out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0);

    
    extra["X-Token"]           = "t1";
    extra["User-Agent"]        = "biz/2.0";
    extra["Host"]              = "evil.com";
    extra["Sec-WebSocket-Key"] = "hijacked";
    extra["Connection"]        = "close";
    CHECK(WSConnection::BuildHandshakeRequest(p, "abc123==", extra, out, err));
    CHECK(Contains(out, "X-Token: t1\r\n"));
    CHECK(Contains(out, "User-Agent: biz/2.0\r\n"));
    CHECK(!Contains(out, "ttsignal/1.0"));
    CHECK(!Contains(out, "evil.com"));
    CHECK(!Contains(out, "hijacked"));
    CHECK(!Contains(out, "Connection: close"));
    CHECK(Contains(out, "Host: example.com:8080\r\n"));

    
    HttpHeaderMap bad;
    bad["X-Evil"] = "a\r\nX-Injected: 1";
    CHECK(!WSConnection::BuildHandshakeRequest(p, "k", bad, out, err));
    CHECK(Contains(err, "CR/LF"));
    HttpHeaderMap bad2;
    bad2["X Evil"] = "v";
    CHECK(!WSConnection::BuildHandshakeRequest(p, "k", bad2, out, err));
    CHECK(Contains(err, "token"));
    
    HttpHeaderMap none;
    CHECK(!WSConnection::BuildHandshakeRequest(p, "", none, out, err));
}





static void test_check_handshake()
{
    printf("[4] CheckHandshakeResponse\n");

    const std::string key    = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    std::string       err;

    HttpHeaderMap h;
    h["upgrade"]              = "websocket";
    h["connection"]           = "Upgrade";
    h["sec-websocket-accept"] = accept;
    CHECK(WSConnection::CheckHandshakeResponse(101, h, key, err));

    
    h["upgrade"] = "WebSocket";
    CHECK(WSConnection::CheckHandshakeResponse(101, h, key, err));

    
    h["connection"] = "keep-alive, Upgrade";
    CHECK(WSConnection::CheckHandshakeResponse(101, h, key, err));

    
    CHECK(!WSConnection::CheckHandshakeResponse(200, h, key, err));
    CHECK(Contains(err, "101"));
    CHECK(!WSConnection::CheckHandshakeResponse(403, h, key, err));

    
    HttpHeaderMap h2 = h;
    h2.erase("sec-websocket-accept");
    CHECK(!WSConnection::CheckHandshakeResponse(101, h2, key, err));
    CHECK(Contains(err, "Sec-WebSocket-Accept"));

    
    HttpHeaderMap h3 = h;
    h3["sec-websocket-accept"] = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h3, key, err));
    CHECK(Contains(err, "不匹配"));

    
    HttpHeaderMap h4 = h;
    std::string   lowered = accept;
    for (size_t i = 0; i < lowered.size(); i++)
    {
        if (lowered[i] >= 'A' && lowered[i] <= 'Z')
        {
            lowered[i] = (char)(lowered[i] - 'A' + 'a');
        }
    }
    h4["sec-websocket-accept"] = lowered;
    CHECK(!WSConnection::CheckHandshakeResponse(101, h4, key, err));

    
    HttpHeaderMap h5 = h;
    h5["connection"] = "keep-alive";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h5, key, err));
    CHECK(Contains(err, "upgrade"));

    
    HttpHeaderMap h6 = h;
    h6["upgrade"] = "h2c";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h6, key, err));
}





static void test_build_frame()
{
    printf("[5] BuildFrame / PackPacket\n");

    
    {
        BufferPtr payload(new BCBuffer);
        payload->Write("hello", 5);
        WSFrameHeader h;
        h.opcode = WS_OP_TEXT;
        h.is_final = true;
        h.has_mask = false;
        BufferPtr frame = WSParser::BuildFrame(h, payload.get());
        std::string bytes = Flatten(frame);
        CHECK_EQ_INT(bytes.size(), 7);
        CHECK_EQ_INT((uint8_t)bytes[0], 0x81);
        CHECK_EQ_INT((uint8_t)bytes[1], 5);
        CHECK_EQ_STR(bytes.substr(2), "hello");
    }
    
    {
        BufferPtr payload(new BCBuffer);
        payload->Write("hello", 5);
        WSFrameHeader h;
        h.opcode = WS_OP_BINARY;
        h.is_final = true;
        h.has_mask = true;
        h.mask[0] = 0x11; h.mask[1] = 0x22; h.mask[2] = 0x33; h.mask[3] = 0x44;
        BufferPtr frame = WSParser::BuildFrame(h, payload.get());
        std::string bytes = Flatten(frame);
        CHECK_EQ_INT(bytes.size(), 2 + 4 + 5);
        CHECK_EQ_INT((uint8_t)bytes[0], 0x82);
        CHECK_EQ_INT((uint8_t)bytes[1], 0x85);   
        CHECK(bytes.substr(6) != "hello");
        std::string plain;
        for (size_t i = 0; i < 5; i++)
        {
            plain.push_back((char)(bytes[6 + i] ^ bytes[2 + (i % 4)]));
        }
        CHECK_EQ_STR(plain, "hello");
    }
    
    {
        WSFrameHeader h;
        h.opcode = WS_OP_BINARY;
        h.is_final = true;
        h.has_mask = false;

        std::string mid(200, 'a');
        BufferPtr b1(new BCBuffer);
        b1->Write(mid.data(), (uint32_t)mid.size());
        std::string f1 = Flatten(WSParser::BuildFrame(h, b1.get()));
        CHECK_EQ_INT((uint8_t)f1[1], 126);
        CHECK_EQ_INT(f1.size(), 4 + 200);

        std::string big(70000, 'b');
        BufferPtr b2(new BCBuffer);
        b2->Write(big.data(), (uint32_t)big.size());
        std::string f2 = Flatten(WSParser::BuildFrame(h, b2.get()));
        CHECK_EQ_INT((uint8_t)f2[1], 127);
        CHECK_EQ_INT(f2.size(), 10 + 70000);
    }
    
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 10), 2);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(true, 10), 6);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 200), 4);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 70000), 10);
    
    {
        SMPacketPtr pkt(new SMPacket);
        BufferPtr   data(new BCBuffer);
        data->Write("pkt", 3);
        pkt->Create(data);
        BufferPtr frame = WSParser::PackPacket(pkt, WS_OP_BINARY, false);
        CHECK(frame != NULL);
        std::string bytes = Flatten(frame);
        CHECK_EQ_INT((uint8_t)bytes[0], 0x82);
        CHECK_EQ_STR(bytes.substr(2), "pkt");
        
        CHECK(WSParser::PackPacket(SMPacketPtr(), WS_OP_BINARY, false) == NULL);
    }
}





static void test_parse_frames_ok()
{
    printf("[6] 帧解码：掩码 / 增量 / 零长度 / 三档长度\n");

    const uint8_t mask[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

    
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_TEXT, "hello", NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_TEXT);
        CHECK_EQ_STR(p.frames[0].payload, "hello");
    }
    
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_BINARY, "masked-payload", mask)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, "masked-payload");
    }
    
    {
        TestParser p;
        std::string two = MakeFrame(true, WS_OP_TEXT, "a", NULL)
                        + MakeFrame(true, WS_OP_TEXT, "bb", NULL);
        CHECK(!p.Feed(two));
        CHECK_EQ_INT(p.frames.size(), 2);
        CHECK_EQ_STR(p.frames[0].payload, "a");
        CHECK_EQ_STR(p.frames[1].payload, "bb");
    }
    
    {
        TestParser p;
        std::string s1(200, 'x');
        std::string s2(70000, 'y');
        CHECK(!p.FeedByteByByte(MakeFrame(true, WS_OP_TEXT, "short", mask)));
        CHECK(!p.FeedByteByByte(MakeFrame(true, WS_OP_BINARY, s1, NULL)));
        CHECK(!p.Feed(MakeFrame(true, WS_OP_BINARY, s2, mask)));
        CHECK_EQ_INT(p.frames.size(), 3);
        CHECK_EQ_STR(p.frames[0].payload, "short");
        CHECK_EQ_INT(p.frames[1].payload.size(), 200);
        CHECK_EQ_INT(p.frames[2].payload.size(), 70000);
        CHECK_EQ_STR(p.frames[2].payload, s2);
    }
    
    
    {
        TestParser p;
        std::string s = MakeFrame(true, WS_OP_TEXT, "", NULL)
                      + MakeFrame(true, WS_OP_PING, "", NULL)
                      + MakeFrame(true, WS_OP_PONG, "", NULL)
                      + MakeFrame(true, WS_OP_CLOSE, "", NULL);
        CHECK(!p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 4);
        for (size_t i = 0; i < p.frames.size(); i++)
        {
            CHECK(p.frames[i].payload.empty());
        }
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_TEXT);
        CHECK_EQ_INT(p.frames[3].opcode, WS_OP_CLOSE);
    }
    
    {
        TestParser p;
        std::string ping(125, 'p');
        CHECK(!p.Feed(MakeFrame(true, WS_OP_PING, ping, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, ping);
    }
    
    
    {
        TestParser p;
        BufferPtr payload(new BCBuffer);
        payload->Write("roundtrip", 9);
        CHECK(p.WriteToWS(WS_OP_TEXT, true, payload, NULL) == BC_R_SUCCESS);
        CHECK_EQ_INT(p.writes.size(), 1);
        TestParser q;
        CHECK(!q.Feed(p.writes[0]));
        CHECK_EQ_INT(q.frames.size(), 1);
        CHECK_EQ_STR(q.frames[0].payload, "roundtrip");
        
        BufferPtr b2(new BCBuffer);
        CHECK(p.WriteToWS(WS_OP_PING, true, b2, NULL) == BC_R_INVALIDARG);
    }
}







static void test_fragments()
{
    printf("[7] 分片重组\n");

    const uint8_t mask[4] = { 0x01, 0x02, 0x03, 0x04 };

    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT,     "Hel",  NULL)
                      + MakeFrame(false, WS_OP_CONTINUE, "lo, ", NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, "world", NULL);
        CHECK(!p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_TEXT);
        CHECK_EQ_STR(p.frames[0].payload, "Hello, world");
    }
    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_BINARY,   "aaa", mask)
                      + MakeFrame(true,  WS_OP_CONTINUE, "bbb", mask);
        CHECK(!p.FeedByteByByte(s));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_BINARY);
        CHECK_EQ_STR(p.frames[0].payload, "aaabbb");
    }
    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT,     "part1", NULL)
                      + MakeFrame(true,  WS_OP_PING,     "hb",    NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, "part2", NULL);
        CHECK(!p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 2);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_PING);
        CHECK_EQ_STR(p.frames[0].payload, "hb");
        CHECK_EQ_INT(p.frames[1].opcode, WS_OP_TEXT);
        CHECK_EQ_STR(p.frames[1].payload, "part1part2");
    }
    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT,     "A", NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, "B", NULL)
                      + MakeFrame(false, WS_OP_BINARY,   "C", NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, "D", NULL);
        CHECK(!p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 2);
        CHECK_EQ_STR(p.frames[0].payload, "AB");
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_TEXT);
        CHECK_EQ_STR(p.frames[1].payload, "CD");
        CHECK_EQ_INT(p.frames[1].opcode, WS_OP_BINARY);
    }
    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT,     "x", NULL)
                      + MakeFrame(false, WS_OP_CONTINUE, "",  NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, "y", NULL);
        CHECK(!p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, "xy");
    }
}





static void test_protocol_violations()
{
    printf("[8] 协议违规\n");

    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_TEXT, "x", NULL, 0x1)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, 0x3, "x", NULL)));
    }
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, 0xB, "x", NULL)));
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(false, WS_OP_PING, "x", NULL)));
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_PING, std::string(126, 'p'), NULL)));
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_CONTINUE, "x", NULL)));
    }
    
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT, "a", NULL)
                      + MakeFrame(true,  WS_OP_TEXT, "b", NULL);
        CHECK(p.Feed(s));
    }
    
    
    {
        TestParser p;
        p.SetMaxFrameBytes(1024);
        CHECK_EQ_INT(p.MaxFrameBytes(), 1024);
        CHECK(p.Feed(MakeHugeLenFrame(0x7FFFFFFFFFFFull, false)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeHugeLenFrame(16, true)));
    }
    
    {
        TestParser p;
        p.SetMaxFrameBytes(100);
        CHECK(p.Feed(MakeFrame(true, WS_OP_BINARY, std::string(300, 'z'), NULL)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    
    {
        TestParser p;
        p.SetMaxFrameBytes(150);
        std::string s = MakeFrame(false, WS_OP_BINARY,   std::string(100, 'a'), NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, std::string(100, 'b'), NULL);
        CHECK(p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    
    {
        TestParser p;
        p.SetMaxFrameBytes(100);
        CHECK(!p.Feed(MakeFrame(true, WS_OP_BINARY, std::string(100, 'z'), NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
    }
    
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(false, WS_OP_TEXT, "orphan", NULL)));
        CHECK_EQ_INT(p.frames.size(), 0);
        p.Cleanup();
        CHECK(!p.Feed(MakeFrame(true, WS_OP_TEXT, "fresh", NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, "fresh");
    }
}












static void test_close_frame_lengths()
{
    printf("[9] close 帧长度边界（0 / 1 / >=2）\n");

    
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, "", NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_CLOSE);
        CHECK(p.frames[0].payload.empty());
    }
    
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_CLOSE, "\x03", NULL)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    
    {
        TestParser p;
        std::string body;
        body.push_back((char)0x03);
        body.push_back((char)0xE8);         
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), 2);
        const uint8_t* q = (const uint8_t*)p.frames[0].payload.data();
        CHECK_EQ_INT((q[0] << 8) | q[1], 1000);
    }
    
    {
        TestParser p;
        std::string body;
        body.push_back((char)0x03);
        body.push_back((char)0xEA);         
        body += "protocol error";
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), body.size());
        const uint8_t* q = (const uint8_t*)p.frames[0].payload.data();
        CHECK_EQ_INT((q[0] << 8) | q[1], 1002);
    }
    
    {
        TestParser p;
        std::string body(125, 'x');
        body[0] = (char)0x03;
        body[1] = (char)0xE8;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), 125);
    }
    
    {
        TestParser p;
        const uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };
        CHECK(p.Feed(MakeFrame(true, WS_OP_CLOSE, "\x03", mask)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
}











static void test_max_frame_bytes_clamp()
{
    printf("[10] maxFrameBytes 上限 clamp\n");

    
    {
        TestParser p;
        p.SetMaxFrameBytes((size_t)5 * 1024 * 1024 * 1024);   
        CHECK_EQ_INT(p.MaxFrameBytes(), (size_t)UINT32_MAX);
        
        p.SetMaxFrameBytes(64 * 1024);
        CHECK_EQ_INT(p.MaxFrameBytes(), 64 * 1024);
        
        p.SetMaxFrameBytes(0);
        CHECK_EQ_INT(p.MaxFrameBytes(), 64 * 1024);
        
        p.SetMaxFrameBytes((size_t)UINT32_MAX);
        CHECK_EQ_INT(p.MaxFrameBytes(), (size_t)UINT32_MAX);
    }
    
    {
        WSConnection::Config cfg;
        CHECK_EQ_INT(cfg.maxFrameBytes, WS_DEFAULT_MAX_FRAME_BYTES);
        CHECK_EQ_INT(cfg.bad_max_frame_bytes, 0);

        BCFObject o;
        o.PutInt("maxFrameBytes", (uint64_t)5 * 1024 * 1024 * 1024);
        cfg.Init(&o);
        CHECK_EQ_INT(cfg.maxFrameBytes, (size_t)UINT32_MAX);
        CHECK_EQ_INT(cfg.bad_max_frame_bytes, (uint64_t)5 * 1024 * 1024 * 1024);
    }
    
    {
        WSConnection::Config cfg;
        BCFObject            o;
        o.PutInt("maxFrameBytes", 1024 * 1024);
        cfg.Init(&o);
        CHECK_EQ_INT(cfg.maxFrameBytes, 1024 * 1024);
        CHECK_EQ_INT(cfg.bad_max_frame_bytes, 0);
    }
}





int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_ws_keys();
    test_parse_url();
    test_build_handshake();
    test_check_handshake();
    test_build_frame();
    test_parse_frames_ok();
    test_fragments();
    test_protocol_violations();
    test_close_frame_lengths();
    test_max_frame_bytes_clamp();

    if (g_failures == 0)
    {
        printf("WSConnector_test: ALL PASSED\n");
        return 0;
    }
    printf("WSConnector_test: %d FAILURE(S)\n", g_failures);
    return 1;
}




