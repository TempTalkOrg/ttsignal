///////////////////////////////////////////////////////////////////////////////
// file : WSConnector_test.cpp
//
// WSConnector / WSParser 里不依赖网络的部分：URL 解析、握手请求组装、握手响应
// 校验、Sec-WebSocket-Key/Accept、以及 WS 帧的编解码（掩码、分片、长度三档、
// 各类协议违规）。不进任何构建目标，手动编译运行。
//
// 真实网络路径（wss、域名、握手失败、ping/pong、close 握手与 Close() 并发）在
// WSConnector_integration_test.cpp 里，那个才是主战场 —— 前几轮的教训是
// "没有真实路径测试覆盖的分支必然有 bug"。
//
// ---------------------------------------------------------------------------
// 怎么跑（macOS arm64；Linux 见文件末尾的替换说明）
// ---------------------------------------------------------------------------
//
//   cd /Users/antonio/chative/ttsignal
//
//   # llhttp 是纯 C，必须单独按 C 编（跟着 -std=c++17 走会因为 int->enum 的
//   # 隐式转换编不过）
//   for f in api http llhttp; do \
//       cc -c -g -fsanitize=address -I deps/llhttp/include \
//          deps/llhttp/src/$f.c -o /tmp/llhttp_$f.o || break; done
//
//   c++ -std=c++17 -g -Wall -fsanitize=address -fno-omit-frame-pointer \
//       -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include -I deps/llhttp/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//       src/cpp/SMPacket.cpp src/cpp/SMPParser.cpp \
//       src/cpp/LLHTTPParser.cpp src/cpp/HttpConnector.cpp \
//       src/cpp/WSParser.cpp src/cpp/WSConnector.cpp \
//       /tmp/llhttp_api.o /tmp/llhttp_http.o /tmp/llhttp_llhttp.o \
//       src/cpp/tests/WSConnector_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/wsconnector_ut
//   /tmp/wsconnector_ut
//
// （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp，
//   库路径换成 deps/*/lib/Linux/x86_64/Debug/*.a。）
//
// 本文件一行网络代码都没有，也不需要 Runtime::Initialize —— 上面那一大串源文件
// 只是为了满足链接器。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// 帧工具
///////////////////////////////////////////////////////////////////////////////

// 把 BCBuffer 拉平成 std::string（不动原 buffer 的游标）
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

// 手工拼一帧。BuildFrame 永远置 FIN，测分片必须自己拼。
static std::string MakeFrame(bool fin, uint8_t opcode,
                             const std::string& payload,
                             const uint8_t* mask /* 4 字节，NULL 表示不掩码 */,
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

// 长度字段直接填一个天文数字（payload 不必真的那么长），用来验证上限检查发生在
// 分配之前。
static std::string MakeHugeLenFrame(uint64_t declaredLen, bool msbSet)
{
    std::string out;
    out.push_back((char)(0x80 | 0x02));    // FIN + BINARY
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

///////////////////////////////////////////////////////////////////////////////
// TestParser —— 记录收到的帧
///////////////////////////////////////////////////////////////////////////////

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

    // 喂数据，返回是否抛了 BCException
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
        // 交付的长度必须与 header 一致 —— 分片重组时 header 会被改写
        CHECK_EQ_INT(header.payload_size, payload_len);
        frames.push_back(f);
    }
};

///////////////////////////////////////////////////////////////////////////////
// case 1：Sec-WebSocket-Key / Accept
///////////////////////////////////////////////////////////////////////////////

static void test_ws_keys()
{
    printf("[1] WSGenKey / WSAcceptKey\n");

    // RFC 6455 1.3 的官方示例向量
    CHECK_EQ_STR(WSParser::WSAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="),
                 "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    // 16 字节随机数的 base64 == 24 字节，末尾一个 '='
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
    // accept 值是 base64(SHA1(..))，20 字节 -> 28 字节
    CHECK_EQ_INT(WSParser::WSAcceptKey(k1).size(), 28);
}

///////////////////////////////////////////////////////////////////////////////
// case 2：ParseUrl
///////////////////////////////////////////////////////////////////////////////

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
    // 没有路径时 request-target 必须补成 "/"
    CHECK_EQ_STR(p.target, "/");

    CHECK(WSConnection::ParseUrl("ws://127.0.0.1:8080/a?b=1#frag", p, err));
    CHECK_EQ_INT(p.port, 8080);
    CHECK_EQ_STR(p.hostHeader, "127.0.0.1:8080");
    // fragment 不上线
    CHECK_EQ_STR(p.target, "/a?b=1");

    // 端口等于 scheme 默认值时 Host 头不带端口
    CHECK(WSConnection::ParseUrl("wss://a.b:443/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "a.b");
    CHECK(WSConnection::ParseUrl("ws://a.b:80/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "a.b");

    // IPv6：host 剥方括号，Host 头保留
    CHECK(WSConnection::ParseUrl("wss://[2001:db8::1]:9443/s", p, err));
    CHECK_EQ_STR(p.host, "2001:db8::1");
    CHECK_EQ_STR(p.hostHeader, "[2001:db8::1]:9443");
    CHECK_EQ_INT(p.port, 9443);

    // 拒绝：非 ws/wss、缺 scheme、缺 host、userinfo、非法端口、裸 IPv6
    CHECK(!WSConnection::ParseUrl("http://example.com/", p, err));
    CHECK(Contains(err, "只支持 ws / wss"));
    CHECK(!WSConnection::ParseUrl("example.com/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://", p, err));
    CHECK(!WSConnection::ParseUrl("ws://u:pw@example.com/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://example.com:0/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://example.com:99999/", p, err));
    CHECK(!WSConnection::ParseUrl("ws://2001:db8::1/", p, err));
    // 路径里的空白必须先做 percent-encoding
    CHECK(!WSConnection::ParseUrl("ws://example.com/a b", p, err));
}

///////////////////////////////////////////////////////////////////////////////
// case 3：BuildHandshakeRequest
///////////////////////////////////////////////////////////////////////////////

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
    // 必须以空行结束，且不带报文体
    CHECK(out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0);

    // 业务头透传；保留头被忽略；User-Agent 可覆盖
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

    // 注入：头值里的 CRLF、头名里的非 token
    HttpHeaderMap bad;
    bad["X-Evil"] = "a\r\nX-Injected: 1";
    CHECK(!WSConnection::BuildHandshakeRequest(p, "k", bad, out, err));
    CHECK(Contains(err, "CR/LF"));
    HttpHeaderMap bad2;
    bad2["X Evil"] = "v";
    CHECK(!WSConnection::BuildHandshakeRequest(p, "k", bad2, out, err));
    CHECK(Contains(err, "token"));
    // 空 key
    HttpHeaderMap none;
    CHECK(!WSConnection::BuildHandshakeRequest(p, "", none, out, err));
}

///////////////////////////////////////////////////////////////////////////////
// case 4：CheckHandshakeResponse
///////////////////////////////////////////////////////////////////////////////

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

    // Upgrade 的取值大小写无关
    h["upgrade"] = "WebSocket";
    CHECK(WSConnection::CheckHandshakeResponse(101, h, key, err));

    // "Connection: keep-alive, Upgrade" 是合法的 —— jmp 原版的全等比较会误判
    h["connection"] = "keep-alive, Upgrade";
    CHECK(WSConnection::CheckHandshakeResponse(101, h, key, err));

    // 非 101
    CHECK(!WSConnection::CheckHandshakeResponse(200, h, key, err));
    CHECK(Contains(err, "101"));
    CHECK(!WSConnection::CheckHandshakeResponse(403, h, key, err));

    // 缺 accept
    HttpHeaderMap h2 = h;
    h2.erase("sec-websocket-accept");
    CHECK(!WSConnection::CheckHandshakeResponse(101, h2, key, err));
    CHECK(Contains(err, "Sec-WebSocket-Accept"));

    // accept 值错
    HttpHeaderMap h3 = h;
    h3["sec-websocket-accept"] = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h3, key, err));
    CHECK(Contains(err, "不匹配"));

    // accept 值大小写敏感（base64）：把正确值转小写必须判失败
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

    // Connection 里没有 upgrade
    HttpHeaderMap h5 = h;
    h5["connection"] = "keep-alive";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h5, key, err));
    CHECK(Contains(err, "upgrade"));

    // Upgrade 不是 websocket
    HttpHeaderMap h6 = h;
    h6["upgrade"] = "h2c";
    CHECK(!WSConnection::CheckHandshakeResponse(101, h6, key, err));
}

///////////////////////////////////////////////////////////////////////////////
// case 5：帧编码（BuildFrame / PackPacket）
///////////////////////////////////////////////////////////////////////////////

static void test_build_frame()
{
    printf("[5] BuildFrame / PackPacket\n");

    // 短帧、不掩码
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
    // 掩码帧：掩码位置 1，payload 与明文不同，异或回去必须一致
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
        CHECK_EQ_INT((uint8_t)bytes[1], 0x85);   // MASK 位 + 长度 5
        CHECK(bytes.substr(6) != "hello");
        std::string plain;
        for (size_t i = 0; i < 5; i++)
        {
            plain.push_back((char)(bytes[6 + i] ^ bytes[2 + (i % 4)]));
        }
        CHECK_EQ_STR(plain, "hello");
    }
    // 126 / 127 三档长度
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
    // CalculateFrameHeaderSize
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 10), 2);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(true, 10), 6);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 200), 4);
    CHECK_EQ_INT(WSParser::CalculateFrameHeaderSize(false, 70000), 10);
    // PackPacket：SMPacket 的 origion_data 封成一帧
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
        // 空指针不该崩
        CHECK(WSParser::PackPacket(SMPacketPtr(), WS_OP_BINARY, false) == NULL);
    }
}

///////////////////////////////////////////////////////////////////////////////
// case 6：帧解码 happy path（含掩码、增量喂料、零长度）
///////////////////////////////////////////////////////////////////////////////

static void test_parse_frames_ok()
{
    printf("[6] 帧解码：掩码 / 增量 / 零长度 / 三档长度\n");

    const uint8_t mask[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

    // 不掩码的文本帧（服务端发给客户端的正常形态）
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_TEXT, "hello", NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_TEXT);
        CHECK_EQ_STR(p.frames[0].payload, "hello");
    }
    // 掩码帧必须被正确解掩
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_BINARY, "masked-payload", mask)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, "masked-payload");
    }
    // 一次喂两帧（粘包）
    {
        TestParser p;
        std::string two = MakeFrame(true, WS_OP_TEXT, "a", NULL)
                        + MakeFrame(true, WS_OP_TEXT, "bb", NULL);
        CHECK(!p.Feed(two));
        CHECK_EQ_INT(p.frames.size(), 2);
        CHECK_EQ_STR(p.frames[0].payload, "a");
        CHECK_EQ_STR(p.frames[1].payload, "bb");
    }
    // 逐字节喂（拆包）：三档长度都要过
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
    // 零长度 payload：空文本、空 ping、空 close、空 pong。
    // 原版会取空 vector 的 &v[0]（UB），这一条就是钉它的。
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
    // 控制帧带 payload（ping 的 payload 要能原样拿到，pong 才能回显）
    {
        TestParser p;
        std::string ping(125, 'p');
        CHECK(!p.Feed(MakeFrame(true, WS_OP_PING, ping, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_STR(p.frames[0].payload, ping);
    }
    // WriteToWS：编出来的帧自己能解回去（返回值必须是 SUCCESS —— 原版永远返回
    // BC_R_FAILURE）
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
        // 控制帧不走 WriteToWS
        BufferPtr b2(new BCBuffer);
        CHECK(p.WriteToWS(WS_OP_PING, true, b2, NULL) == BC_R_INVALIDARG);
    }
}

///////////////////////////////////////////////////////////////////////////////
// case 7：分片重组
//
// 原版只在 FIN 帧上派发，非 FIN 帧的 payload 被整段丢弃 —— 这一组用例就是钉它。
///////////////////////////////////////////////////////////////////////////////

static void test_fragments()
{
    printf("[7] 分片重组\n");

    const uint8_t mask[4] = { 0x01, 0x02, 0x03, 0x04 };

    // TEXT(!fin) + CONT(!fin) + CONT(fin) -> 一次交付，内容是三段拼起来
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
    // 掩码 + 逐字节喂 + 分片
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_BINARY,   "aaa", mask)
                      + MakeFrame(true,  WS_OP_CONTINUE, "bbb", mask);
        CHECK(!p.FeedByteByByte(s));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_BINARY);
        CHECK_EQ_STR(p.frames[0].payload, "aaabbb");
    }
    // 控制帧允许插在分片消息中间，且不打断重组
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
    // 连续两条分片消息，第二条不受第一条影响
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
    // 中间帧为空 payload 也要正确
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

///////////////////////////////////////////////////////////////////////////////
// case 8：协议违规必须抛异常（而不是解出垃圾或 OOM）
///////////////////////////////////////////////////////////////////////////////

static void test_protocol_violations()
{
    printf("[8] 协议违规\n");

    // RSV 位非零（没协商任何扩展）
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_TEXT, "x", NULL, 0x1)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    // 保留 opcode（0x3..0x7、0xB..0xF）
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, 0x3, "x", NULL)));
    }
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, 0xB, "x", NULL)));
    }
    // 控制帧被分片
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(false, WS_OP_PING, "x", NULL)));
    }
    // 控制帧 payload 超过 125 字节
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_PING, std::string(126, 'p'), NULL)));
    }
    // continuation 但没有待续的消息
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_CONTINUE, "x", NULL)));
    }
    // 分片还没结束又来一个新的数据帧
    {
        TestParser p;
        std::string s = MakeFrame(false, WS_OP_TEXT, "a", NULL)
                      + MakeFrame(true,  WS_OP_TEXT, "b", NULL);
        CHECK(p.Feed(s));
    }
    // 单帧超过 maxFrameBytes：**长度字段填天文数字，payload 并不真的发**。
    // 上限检查必须发生在 resize() 之前，否则这一条会当场 OOM。
    {
        TestParser p;
        p.SetMaxFrameBytes(1024);
        CHECK_EQ_INT(p.MaxFrameBytes(), 1024);
        CHECK(p.Feed(MakeHugeLenFrame(0x7FFFFFFFFFFFull, false)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    // 64 位长度的最高位必须为 0（RFC 6455 5.2）
    {
        TestParser p;
        CHECK(p.Feed(MakeHugeLenFrame(16, true)));
    }
    // 16 位长度档也要受 maxFrameBytes 约束
    {
        TestParser p;
        p.SetMaxFrameBytes(100);
        CHECK(p.Feed(MakeFrame(true, WS_OP_BINARY, std::string(300, 'z'), NULL)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    // 分片重组之后的总长同样受约束 —— 否则对端把大消息切成合法小帧就能绕过
    {
        TestParser p;
        p.SetMaxFrameBytes(150);
        std::string s = MakeFrame(false, WS_OP_BINARY,   std::string(100, 'a'), NULL)
                      + MakeFrame(true,  WS_OP_CONTINUE, std::string(100, 'b'), NULL);
        CHECK(p.Feed(s));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    // 边界：正好等于上限要能过
    {
        TestParser p;
        p.SetMaxFrameBytes(100);
        CHECK(!p.Feed(MakeFrame(true, WS_OP_BINARY, std::string(100, 'z'), NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
    }
    // Cleanup 之后解析器要能从干净状态重新开始
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

///////////////////////////////////////////////////////////////////////////////
// case 9：close 帧的 payload 长度边界
//
// RFC 6455 5.5.1：close 帧的 body 要么**为空**，要么**至少 2 字节**（2 字节
// 关闭码 + 可选的 UTF-8 原因）。长度为 1 是畸形帧。
//
// ⚠️ 这一条是 review 抓出来的漏检：原实现只校验了控制帧 <= 125 字节，长度为 1
// 的 close 帧会被正常交付，上层 `if (payload_len >= 2)` 取不到码就默认成
// kWSCloseNormal(1000) —— 一个畸形帧被报成了"干净的正常关闭"。
///////////////////////////////////////////////////////////////////////////////

static void test_close_frame_lengths()
{
    printf("[9] close 帧长度边界（0 / 1 / >=2）\n");

    // 长度 0：合法，无关闭码
    {
        TestParser p;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, "", NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].opcode, WS_OP_CLOSE);
        CHECK(p.frames[0].payload.empty());
    }
    // 长度 1：**非法**，必须判协议违规而不是当成"没带关闭码"
    {
        TestParser p;
        CHECK(p.Feed(MakeFrame(true, WS_OP_CLOSE, "\x03", NULL)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
    // 长度 2：合法，关闭码 1000
    {
        TestParser p;
        std::string body;
        body.push_back((char)0x03);
        body.push_back((char)0xE8);         // 1000
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), 2);
        const uint8_t* q = (const uint8_t*)p.frames[0].payload.data();
        CHECK_EQ_INT((q[0] << 8) | q[1], 1000);
    }
    // 长度 2 + 原因文本：合法
    {
        TestParser p;
        std::string body;
        body.push_back((char)0x03);
        body.push_back((char)0xEA);         // 1002
        body += "protocol error";
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), body.size());
        const uint8_t* q = (const uint8_t*)p.frames[0].payload.data();
        CHECK_EQ_INT((q[0] << 8) | q[1], 1002);
    }
    // 边界：125 字节（控制帧上限）合法，126 非法（已由 case 8 覆盖 >125）
    {
        TestParser p;
        std::string body(125, 'x');
        body[0] = (char)0x03;
        body[1] = (char)0xE8;
        CHECK(!p.Feed(MakeFrame(true, WS_OP_CLOSE, body, NULL)));
        CHECK_EQ_INT(p.frames.size(), 1);
        CHECK_EQ_INT(p.frames[0].payload.size(), 125);
    }
    // 掩码的畸形 close 帧同样要判掉（长度检查发生在解掩之前的帧头阶段）
    {
        TestParser p;
        const uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };
        CHECK(p.Feed(MakeFrame(true, WS_OP_CLOSE, "\x03", mask)));
        CHECK_EQ_INT(p.frames.size(), 0);
    }
}

///////////////////////////////////////////////////////////////////////////////
// case 10：maxFrameBytes 的上限 clamp
//
// _RequireData 的形参是 uint32_t，而 payload_size 是 size_t。业务把
// maxFrameBytes 配到 > 4 GiB 的话，强转会静默截断，require_data_size_ 与真实
// payload_size 不一致，解析状态机行为不可预测。所以两道口子都要夹：
//   * WSParser::SetMaxFrameBytes（不变量的持有者，最后一道防线）
//   * WSConnection::Config::Init（业务配置入口，越界要告警）
///////////////////////////////////////////////////////////////////////////////

static void test_max_frame_bytes_clamp()
{
    printf("[10] maxFrameBytes 上限 clamp\n");

    // SetMaxFrameBytes 自己夹
    {
        TestParser p;
        p.SetMaxFrameBytes((size_t)5 * 1024 * 1024 * 1024);   // 5 GiB
        CHECK_EQ_INT(p.MaxFrameBytes(), (size_t)UINT32_MAX);
        // 正常取值不受影响
        p.SetMaxFrameBytes(64 * 1024);
        CHECK_EQ_INT(p.MaxFrameBytes(), 64 * 1024);
        // 0 表示"不改"
        p.SetMaxFrameBytes(0);
        CHECK_EQ_INT(p.MaxFrameBytes(), 64 * 1024);
        // 正好等于上限
        p.SetMaxFrameBytes((size_t)UINT32_MAX);
        CHECK_EQ_INT(p.MaxFrameBytes(), (size_t)UINT32_MAX);
    }
    // 配置入口夹，并记下越界原值供告警
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
    // 合法取值不该被记成越界
    {
        WSConnection::Config cfg;
        BCFObject            o;
        o.PutInt("maxFrameBytes", 1024 * 1024);
        cfg.Init(&o);
        CHECK_EQ_INT(cfg.maxFrameBytes, 1024 * 1024);
        CHECK_EQ_INT(cfg.bad_max_frame_bytes, 0);
    }
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
