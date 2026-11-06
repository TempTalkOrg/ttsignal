












#include "HttpConnector.h"
#include "Runtime.h"

#include <BC/BCFCodec.h>
#include <BC/BCLog.h>       

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
















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





class Printer : public IHttpRequestHandler
{
public:
    void OnHttpResponse(const HttpResponse& r) override
    {
        
        printf("status: %d ", r.status);
        PrintSanitized(stdout, r.reason, false);
        printf("\n");
        printf("peerIp: %s\n", r.peerIp.c_str());
        printf("boundIfIndex: %u\n", r.boundIfIndex);
        printf("pinMethod: %s\n",
               r.pinMethod.empty() ? "(未绑定)" : r.pinMethod.c_str());
        printf("--- headers ---\n");
        for (HttpHeaderMap::const_iterator it = r.headers.begin();
             it != r.headers.end(); ++it)
        {
            
            PrintSanitized(stdout, it->first, false);
            printf(": ");
            PrintSanitized(stdout, it->second, false);
            printf("\n");
        }
        
        
        size_t show = r.body.size() < 512 ? r.body.size() : 512;
        printf("--- body (%zu bytes, 前 %zu) ---\n", r.body.size(), show);
        PrintSanitized(stdout, r.body.data(), show, true);
        printf("\n");

        std::unique_lock<std::mutex> lk(m_);
        exit_code_ = (r.status >= 200 && r.status < 400) ? 0 : 1;
        done_      = true;
        cv_.notify_all();
    }

    void OnHttpError(BCRESULT result, const std::string& message) override
    {
        
        fprintf(stderr, "ERROR result=%u: ", (unsigned)result);
        PrintSanitized(stderr, message, false);
        fprintf(stderr, "\n");

        std::unique_lock<std::mutex> lk(m_);
        exit_code_ = 2;
        done_      = true;
        cv_.notify_all();
    }

    bool Wait(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return done_; });
    }

    int ExitCode()
    {
        std::unique_lock<std::mutex> lk(m_);
        return exit_code_;
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    done_      = false;
    int                     exit_code_ = 2;
};





class ConnectorHandler : public IHttpConnectorHandler
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





static void Usage()
{
    fprintf(stderr,
            "用法: httpget --url <URL> [选项]\n"
            "  --method M            HTTP 方法，默认 GET\n"
            "  --header \"K: V\"       追加请求头，可重复\n"
            "  --body TEXT           请求体\n"
            "  --vpn-policy P        os | prefer-physical | force-physical\n"
            "  --dns IP[,IP...]      绕过 VPN 的 DNS 服务器\n"
            "  --ca FILE             CA 证书 PEM 文件；不给则用系统信任库\n"
            "  --spki-pin BASE64     只比对 SPKI pin\n"
            "  --insecure            跳过一切证书校验（仅调试）\n"
            "  --resolved-ip IP      跳过 DNS，直接用这个 IP\n"
            "  --max-response-bytes N  响应体上限，默认 8388608\n"
            "  --timeout-ms N        整次请求超时，默认 10000\n"
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





int main(int argc, char** argv)
{
    
    setvbuf(stdout, NULL, _IONBF, 0);

    HttpRequest req;
    const char* vpnPolicy       = NULL;
    const char* dnsServers      = NULL;
    const char* caFile          = NULL;
    const char* spkiPin         = NULL;
    const char* logLevelText    = "warn";
    bool        insecure        = false;
    uint64_t    maxResponseBytes = 0;

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

        TAKE("--url",     req.url)
        TAKE("--method",  req.method)
        TAKE("--body",    req.body)
        TAKE("--vpn-policy", vpnPolicy)
        TAKE("--dns",     dnsServers)
        TAKE("--ca",      caFile)
        TAKE("--spki-pin", spkiPin)
        TAKE("--resolved-ip", req.resolvedIp)
        TAKE("--log-level", logLevelText)
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
            req.headers[k] = v;
            continue;
        }
        if (!strcmp(a, "--timeout-ms"))
        {
            if (!need) { fprintf(stderr, "--timeout-ms 缺少参数\n"); return 1; }
            req.timeoutMs = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--max-response-bytes"))
        {
            if (!need) { fprintf(stderr, "--max-response-bytes 缺少参数\n"); return 1; }
            maxResponseBytes = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--insecure"))
        {
            insecure = true;
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))
        {
            Usage();
            return 0;
        }
        fprintf(stderr, "未知参数：%s\n", a);
        Usage();
        return 1;
    }

    if (req.url.empty())
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

    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 4);
    runtimeCfg.PutInt("timerThreads", 2);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        fprintf(stderr, "Runtime::Initialize 失败\n");
        return 2;
    }

    int exitCode = 2;
    {
        BCFObject cfg;
        cfg.PutInt("logLevel", (uint64_t)ParseLogLevel(logLevelText));
        if (vpnPolicy)        cfg.PutString("vpnPolicy", vpnPolicy);
        if (dnsServers)       cfg.PutString("dnsServers", dnsServers);
        if (!caPem.empty())   cfg.PutString("caCerts", caPem.c_str());
        if (spkiPin)          cfg.PutString("spkiPin", spkiPin);
        if (insecure)         cfg.PutBool("insecureSkipVerify", true);
        if (maxResponseBytes) cfg.PutInt("maxResponseBytes", maxResponseBytes);

        ConnectorHandler connHandler;
        HttpConnector    connector;
        BCRESULT r = connector.Create(&cfg, &connHandler);
        if (r != BC_R_SUCCESS)
        {
            fprintf(stderr, "HttpConnector::Create 失败 result=%u\n", (unsigned)r);
            Runtime::Destroy();
            return 2;
        }

        Printer printer;
        r = connector.Request(req, &printer);
        if (r != BC_R_SUCCESS)
        {
            fprintf(stderr, "HttpConnector::Request 失败 result=%u\n", (unsigned)r);
            connector.Close();
            connHandler.WaitClosed(3000);
            Runtime::Destroy();
            return 2;
        }

        
        
        int waitMs = (int)(req.timeoutMs ? req.timeoutMs : 10000) + 5000;
        if (!printer.Wait(waitMs))
        {
            fprintf(stderr, "ERROR 等待 %d ms 仍无结果（库内超时没有生效？）\n",
                    waitMs);
        }
        exitCode = printer.ExitCode();

        connector.Close();
        connHandler.WaitClosed(5000);
        
    }

    Runtime::Destroy();
    return exitCode;
}




