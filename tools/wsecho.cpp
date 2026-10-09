///////////////////////////////////////////////////////////////////////////////
// file : tools/wsecho.cpp
//
// WebSocket 端到端验证工具。用法：
//
//   wsecho --url wss://echo.websocket.org/ \
//          [--text "hello"] [--count 1] [--header "K: V"]... \
//          [--vpn-policy os|prefer-physical|force-physical] \
//          [--dns 223.5.5.5] [--ca /path/ca.pem] [--insecure] \
//          [--spki-pin BASE64] [--max-frame-bytes N] \
//          [--ping] [--timeout-ms 10000] [--log-level debug|info|warn|error|none]
//
// 连上之后发 --count 条文本帧，等同样条数的回声，然后主动关闭。打印握手响应头、
// 收到的每条消息，以及 peerIp / boundIfIndex / pinMethod —— 后三项用来确认这次
// 连接到底有没有真的走物理网卡。
//
// 退出码：0 = 握手成功且收齐回声；1 = 参数错误；2 = 握手/传输失败或回声不齐。
///////////////////////////////////////////////////////////////////////////////

#include "WSConnector.h"
#include "Runtime.h"

#include <BC/BCFCodec.h>
#include <BC/BCLog.h>       // _ERROR_ / _WARN_ / _INFO_ / _DEBUG_

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace WS;

///////////////////////////////////////////////////////////////////////////////
// PrintSanitized —— 把远端来的字节安全地打到终端
//
// ⚠️ 握手响应头与文本帧都是**未净化的原始网络字节**，里面可以有 ESC，也就是可以
// 往本地终端注入 ANSI 转义序列（改颜色、清屏、移动光标，伪造出一份看起来完全
// 不同的输出）。内嵌 NUL 还会让 printf("%s") 提前截断。
// 规则与 tools/httpget.cpp 的同名函数一致。
///////////////////////////////////////////////////////////////////////////////

static void PrintSanitized(FILE* out, const char* data, size_t size,
                           bool allowNewline)
{
    for (size_t i = 0; i < size; i++)
    {
        unsigned char c = (unsigned char)data[i];
        if (c == '\t' || (allowNewline && c == '\n'))
        {
            fputc((int)c, out);
        }
        else if (c >= 0x20 && c != 0x7f)
        {
            // 含 >= 0x80：UTF-8 原样输出
            fputc((int)c, out);
        }
        else
        {
            fprintf(out, "<%02X>", (unsigned)c);
        }
    }
}

static void PrintSanitized(FILE* out, const std::string& s, bool allowNewline)
{
    PrintSanitized(out, s.data(), s.size(), allowNewline);
}

///////////////////////////////////////////////////////////////////////////////
// Echoer —— 一条连接的回调
///////////////////////////////////////////////////////////////////////////////

class Echoer : public IWSConnectionHandler
{
public:
    Echoer(const std::string& text, int count, bool ping)
        : text_(text), want_(count), ping_(ping)
    {
    }

    void SetConn(WSConnPtr c) { conn_ = c; }

    void OnConnectResult(BCRESULT result, const HttpHeaderMap& headers) override
    {
        if (result != BC_R_SUCCESS)
        {
            fprintf(stderr, "ERROR OnConnectResult result=%u（握手失败）\n",
                    (unsigned)result);
            std::unique_lock<std::mutex> lk(m_);
            done_ = true;
            cv_.notify_all();
            return;
        }
        printf("OnConnectResult result=0\n");
        printf("--- handshake headers ---\n");
        for (HttpHeaderMap::const_iterator it = headers.begin();
             it != headers.end(); ++it)
        {
            PrintSanitized(stdout, it->first, false);
            printf(": ");
            PrintSanitized(stdout, it->second, false);
            printf("\n");
        }
        if (conn_)
        {
            printf("peerIp: %s\n", conn_->PeerIp().c_str());
            printf("boundIfIndex: %u\n", conn_->BoundIfIndex());
            printf("pinMethod: %s\n",
                   conn_->PinMethod().empty() ? "(未绑定)"
                                              : conn_->PinMethod().c_str());
        }
        // 在回调里直接发送 —— 契约明确允许（回调在库的锁之外派发）
        if (conn_ && ping_)
        {
            conn_->SendPing();
        }
        for (int i = 0; i < want_ && conn_; i++)
        {
            char        buf[64];
            std::string msg = text_;
            snprintf(buf, sizeof(buf), " #%d", i + 1);
            msg += buf;
            {
                std::unique_lock<std::mutex> lk(m_);
                pending_.insert(msg);
            }
            BCRESULT r = conn_->SendText(msg);
            if (r != BC_R_SUCCESS)
            {
                fprintf(stderr, "ERROR SendText 失败 result=%u\n", (unsigned)r);
            }
        }
    }

    void OnRecvText(LPCSTR text) override
    {
        printf("OnRecvText: ");
        PrintSanitized(stdout, text ? text : "", strlen(text ? text : ""), false);
        printf("\n");

        // ⚠️ 只认"我们自己发出去的那几条"回来了才算收齐回声。
        // 公共回声服务（echo.websocket.org）连上就会先推一条自己的欢迎语
        // （"Request served by ..."），按条数计数会把它算进去，于是少收一条
        // 真正的回声也会显示成功。
        std::unique_lock<std::mutex> lk(m_);
        std::set<std::string>::iterator it =
            pending_.find(std::string(text ? text : ""));
        if (it != pending_.end())
        {
            pending_.erase(it);
            got_++;
        }
        if (got_ >= want_)
        {
            all_echoed_ = true;
            cv_.notify_all();
        }
    }

    void OnRecvData(LPCVOID data, size_t size) override
    {
        printf("OnRecvData: %zu 字节\n", size);
        (void)data;
    }

    void OnClosed(LPCSTR reason) override
    {
        printf("OnClosed: ");
        PrintSanitized(stdout, reason ? reason : "", strlen(reason ? reason : ""),
                       false);
        printf("\n");
        std::unique_lock<std::mutex> lk(m_);
        done_ = true;
        cv_.notify_all();
    }

    void OnException(BCException& e) override
    {
        fprintf(stderr, "ERROR OnException: ");
        PrintSanitized(stderr, e.GetMsg().c_str(), strlen(e.GetMsg().c_str()),
                       false);
        fprintf(stderr, "\n");
    }

    bool WaitEchoed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return all_echoed_ || done_; });
    }
    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return done_; });
    }
    bool AllEchoed()
    {
        std::unique_lock<std::mutex> lk(m_);
        return all_echoed_;
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    WSConnPtr               conn_;
    std::string             text_;
    int                     want_ = 1;
    bool                    ping_ = false;
    int                     got_  = 0;
    // 已发出、还没收到回声的消息
    std::set<std::string>   pending_;
    bool                    all_echoed_ = false;
    bool                    done_ = false;
};

///////////////////////////////////////////////////////////////////////////////
// CtorHandler —— 把库里的日志打到 stderr，别和消息内容混在 stdout 上
///////////////////////////////////////////////////////////////////////////////

class CtorHandler : public IWSConnectorHandler
{
public:
    void OnLog(int level, LPCSTR msg) override
    {
        fprintf(stderr, "[L%d] %s\n", level, msg ? msg : "");
    }

    void OnClosed() override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_ = true;
        cv_.notify_all();
    }

    void OnException(BCException& e) override
    {
        fprintf(stderr, "ERROR connector exception: %s\n", e.GetMsg().c_str());
    }

    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    closed_ = false;
};

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

static void Usage()
{
    fprintf(stderr,
            "用法: wsecho --url <ws://|wss:// URL> [选项]\n"
            "  --text TEXT           要发的文本，默认 \"hello from ttsignal\"\n"
            "  --count N             发几条，默认 1\n"
            "  --header \"K: V\"       追加握手请求头，可重复\n"
            "  --ping                握手成功后额外发一个 ping\n"
            "  --vpn-policy P        os | prefer-physical | force-physical\n"
            "  --dns IP[,IP...]      绕过 VPN 的 DNS 服务器\n"
            "  --ca FILE             CA 证书 PEM 文件；不给则用系统信任库\n"
            "  --spki-pin BASE64     只比对 SPKI pin\n"
            "  --insecure            跳过一切证书校验（仅调试）\n"
            "  --max-frame-bytes N   收包侧单帧上限，默认 8388608\n"
            "  --timeout-ms N        DNS+连接+TLS+握手 总超时，默认 10000\n"
            "  --log-level L         debug|info|warn|error|none，默认 warn\n");
}

static bool ReadFileAll(const char* path, std::string& out)
{
    FILE* fp = fopen(path, "rb");
    if (!fp)
    {
        return false;
    }
    char   buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        out.append(buf, n);
    }
    fclose(fp);
    return true;
}

// "debug" -> 4，"none" -> 0（BC 的级别数字越小越严重，0 表示什么都不打）
static int ParseLogLevel(const char* s)
{
    if (!s)             return _WARN_;
    if (!strcmp(s, "none"))  return 0;
    if (!strcmp(s, "error")) return _ERROR_;
    if (!strcmp(s, "warn"))  return _WARN_;
    if (!strcmp(s, "info"))  return _INFO_;
    if (!strcmp(s, "debug")) return _DEBUG_;
    return _WARN_;
}

static bool SplitHeader(const char* raw, std::string& key, std::string& value)
{
    const char* colon = strchr(raw, ':');
    if (!colon || colon == raw)
    {
        return false;
    }
    key.assign(raw, (size_t)(colon - raw));
    const char* v = colon + 1;
    while (*v == ' ' || *v == '\t')
    {
        v++;
    }
    value.assign(v);
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    // 不缓冲：库的日志走 stderr（无缓冲），stdout 一缓冲两边顺序就对不上了。
    setvbuf(stdout, NULL, _IONBF, 0);

    const char* url            = NULL;
    const char* text           = "hello from ttsignal";
    const char* vpnPolicy      = NULL;
    const char* dnsServers     = NULL;
    const char* caFile         = NULL;
    const char* spkiPin        = NULL;
    const char* logLevelText   = "warn";
    bool        insecure       = false;
    bool        ping           = false;
    int         count          = 1;
    uint32_t    timeoutMs      = 10000;
    uint64_t    maxFrameBytes  = 0;
    std::vector<std::pair<std::string, std::string> > headers;

    for (int i = 1; i < argc; i++)
    {
        const char* a    = argv[i];
        bool        need = (i + 1 < argc);

#define TAKE(name, dst)                                                        \
        if (!strcmp(a, name)) {                                                \
            if (!need) { fprintf(stderr, "%s 缺少参数\n", name); Usage(); return 1; } \
            dst = argv[++i];                                                   \
            continue;                                                          \
        }

        TAKE("--url",        url)
        TAKE("--text",       text)
        TAKE("--vpn-policy", vpnPolicy)
        TAKE("--dns",        dnsServers)
        TAKE("--ca",         caFile)
        TAKE("--spki-pin",   spkiPin)
        TAKE("--log-level",  logLevelText)
#undef TAKE

        if (!strcmp(a, "--header"))
        {
            if (!need) { fprintf(stderr, "--header 缺少参数\n"); return 1; }
            std::string k, v;
            if (!SplitHeader(argv[++i], k, v))
            {
                fprintf(stderr, "--header 格式错误，期望 \"Key: Value\"\n");
                return 1;
            }
            headers.push_back(std::make_pair(k, v));
            continue;
        }
        if (!strcmp(a, "--count"))
        {
            if (!need) { fprintf(stderr, "--count 缺少参数\n"); return 1; }
            count = (int)strtol(argv[++i], NULL, 10);
            if (count < 1) { fprintf(stderr, "--count 必须 >= 1\n"); return 1; }
            continue;
        }
        if (!strcmp(a, "--timeout-ms"))
        {
            if (!need) { fprintf(stderr, "--timeout-ms 缺少参数\n"); return 1; }
            timeoutMs = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--max-frame-bytes"))
        {
            if (!need) { fprintf(stderr, "--max-frame-bytes 缺少参数\n"); return 1; }
            maxFrameBytes = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--insecure")) { insecure = true; continue; }
        if (!strcmp(a, "--ping"))     { ping     = true; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { Usage(); return 0; }

        fprintf(stderr, "未知参数：%s\n", a);
        Usage();
        return 1;
    }

    if (!url || !*url)
    {
        fprintf(stderr, "必须提供 --url\n");
        Usage();
        return 1;
    }

    std::string caPem;
    if (caFile && !ReadFileAll(caFile, caPem))
    {
        fprintf(stderr, "读不到 CA 文件：%s\n", caFile);
        return 1;
    }

    int exitCode = 2;
    {
        BCFObject cfg;
        cfg.PutInt("workerThreads", 1);
        cfg.PutInt("taskThreads", 4);
        cfg.PutInt("timerThreads", 2);
        cfg.PutInt("logLevel", (uint64_t)ParseLogLevel(logLevelText));
        cfg.PutInt("connectTimeoutMs", timeoutMs);
        if (vpnPolicy)      cfg.PutString("vpnPolicy", vpnPolicy);
        if (dnsServers)     cfg.PutString("dnsServers", dnsServers);
        if (!caPem.empty()) cfg.PutString("caCerts", caPem.c_str());
        if (spkiPin)        cfg.PutString("spkiPin", spkiPin);
        if (insecure)       cfg.PutBool("insecureSkipVerify", true);
        if (maxFrameBytes)  cfg.PutInt("maxFrameBytes", maxFrameBytes);

        CtorHandler ctorHandler;
        WSConnector connector;
        // WSConnector::Create 内部会 Runtime::Initialize（同一个 cfg）
        BCRESULT r = connector.Create(&cfg, &ctorHandler);
        if (r != BC_R_SUCCESS)
        {
            fprintf(stderr, "WSConnector::Create 失败 result=%u\n", (unsigned)r);
            Runtime::Destroy();
            return 2;
        }

        Echoer    echoer(text, count, ping);
        WSConnPtr conn = connector.CreateConnection(NULL, &echoer);
        if (!conn)
        {
            fprintf(stderr, "WSConnector::CreateConnection 失败\n");
            connector.Close();
            ctorHandler.WaitClosed(3000);
            Runtime::Destroy();
            return 2;
        }
        echoer.SetConn(conn);
        for (size_t i = 0; i < headers.size(); i++)
        {
            BCRESULT hr = conn->SetRequestHeader(headers[i].first,
                                                 headers[i].second);
            if (hr != BC_R_SUCCESS)
            {
                fprintf(stderr, "请求头 \"%s\" 被拒 result=%u\n",
                        headers[i].first.c_str(), (unsigned)hr);
                connector.Close();
                ctorHandler.WaitClosed(3000);
                Runtime::Destroy();
                return 1;
            }
        }

        r = conn->Connect(url, timeoutMs);
        if (r != BC_R_SUCCESS)
        {
            fprintf(stderr, "Connect 失败 result=%u\n", (unsigned)r);
            connector.Close();
            ctorHandler.WaitClosed(3000);
            Runtime::Destroy();
            return 2;
        }

        // 库内部已经有握手超时，这里再多留余量兜底
        const int waitMs = (int)timeoutMs + 10000;
        if (!echoer.WaitEchoed(waitMs))
        {
            fprintf(stderr, "ERROR 等待 %d ms 仍没收齐回声\n", waitMs);
        }
        exitCode = echoer.AllEchoed() ? 0 : 2;

        conn->Close();
        echoer.WaitClosed(5000);
        conn.reset();

        connector.Close();
        ctorHandler.WaitClosed(5000);
        // connector 在这里析构，它会同步等到所有 WSConnection 收尾为止
    }

    Runtime::Destroy();
    return exitCode;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
