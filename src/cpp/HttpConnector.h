///////////////////////////////////////////////////////////////////////////////
// file   : HttpConnector.h
// author : anto
//
// 通用 HTTP 客户端。建在 TcpChannel 上，支持 http:// 与 https://。
//
// ⚠️ 本模块不认识任何业务接口。getServiceCallUrl 只是上层业务的一次普通
//    GET —— ttsignal 不拼它的 URL、不解析它的响应体。
//
// 默认做 keep-alive 连接池复用：响应收完后连接不关，按
// host+port+tls+网卡策略分桶留着，下一条同目标的请求直接捡走，省掉 TCP 握手
// 和整套 TLS 握手（实测同一 https 端点 280ms -> 95ms）。
// maxIdleConnections=0 可一键关掉，退回"一次请求一条连接"（那时请求头里会
// 显式发 Connection: close）。
//
// 池里的连接有三条回收路径：服务端那一侧的 keep-alive 超时（对端 FIN 一到就
// 连同看守一起销毁，实践中最常走这条）、本地空闲超时（idleTimeoutMs，默认
// 60 分钟，每次归还重新计时），以及 Close() 时被一并关掉。本地那一道是兜底，
// 专门挡住"永不主动关连接"的服务端 —— 没有它，对着那种服务端只发一次请求，
// 那条 fd 会一直占到连接器关停。
//
// 不自动跟随重定向（3xx 原样返回，Location 在 headers 里）。跟随重定向会让
// force-physical 的语义变复杂：重定向目标需要重新解析、重新复核路由，交给
// 业务显式发第二次请求更清晰。
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
// Create / Request / Close 可以在任意线程调用。IHttpRequestHandler 的两个回调
// 都发生在对应 TcpChannel 的事件循环线程上，IHttpConnectorHandler::OnClosed
// 发生在最后一条请求（或最后一条池化连接）收尾的那条线程上。
//
// ⚠️ 复用意味着两条**先后**的请求可能落在同一条通道、同一条事件循环线程上。
// 并发的请求仍然各走各的通道（池里没有空闲连接时照常新建），所以"回调之间
// 互不阻塞"这一点没变 —— 但在回调里做耗时操作，现在还会连累下一条捡到同一
// 条连接的请求。
//
// ---------------------------------------------------------------------------
// 生命周期契约
// ---------------------------------------------------------------------------
// 1. Request() 返回非 BC_R_SUCCESS 时不会有任何回调（同 TcpChannel::Open）。
// 2. Request() 返回 BC_R_SUCCESS 之后，OnHttpResponse 与 OnHttpError 二者
//    恰好触发一次。handler 必须活到那一刻。
// 3. Close() 幂等；所有在途请求走完并且内部对象全部销毁之后才回调 OnClosed。
// 4. ~HttpConnector() 会自动 Close() 并**同步等待**全部在途请求收尾，所以析构
//    本身是安全的；但它会阻塞调用线程，不要在事件循环线程上析构。
// 5. ⚠️ **Runtime 必须活得比 HttpConnector 久。** 内部对象的销毁是
//    Runtime::PostTask 出去做的，而 Runtime::PostTask 在 Runtime 已经
//    Destroy() 之后会被静默丢弃（Runtime.cpp:129 的 if (s_pInstance)）。
//    先 Runtime::Destroy() 再析构 HttpConnector，那些销毁任务永远不会跑，
//    析构里的等待也就永远等不到。正确顺序是：
//        HttpConnector 析构（或离开作用域） -> Runtime::Destroy()
//    为了不把错误顺序变成永久死锁，析构里**排空在途请求**那一段等待带上限
//    （drainTimeoutMs，默认 30 秒），超时会打 _ERROR_ 日志并**放弃等待**。
//
//    ⚠️ 但放弃之后还有**第二段等待，那一段没有上限**：已经在锁外执行的业务
//    回调必须跑完，析构才能返回 —— 否则调用方一释放 handler 就是 UAF，这段
//    等待是不能加超时的。它的时长完全由业务回调自己决定，所以：
//    **绝对不要在持有业务自己的锁的情况下析构 HttpConnector**（详见第 7 条
//    (2)），否则回调若要取同一把锁就是永久死锁。
// 6. 放弃等待（含 drainTimeoutMs = 0 这种"根本不等"的用法）之后：
//    * **任何 handler 都不再回调** —— OnClosed 不发，在途请求的
//      OnHttpResponse / OnHttpError 也不发。所以析构一返回，调用方就可以
//      安全释放 IHttpConnectorHandler 与所有 IHttpRequestHandler。
//    * 残留连接会在后台自行收尾并销毁，它们与内部共享状态（含日志 appender）
//      的存活期互相绑定，不会悬垂，也**不会泄漏**。
//    * 代价就是"没有收尾信号"：调用方拿不到 OnClosed，也不知道那些请求最终
//      是成是败。想要确定性收尾就别把 drainTimeoutMs 设太小。
//    * 唯一真正会泄漏的情形是销毁顺序反了（先 Runtime::Destroy()）——
//      那时负责销毁的 PostTask 永远不会跑，日志里会有对应的 _ERROR_。
//
// ---------------------------------------------------------------------------
// 7. 回调里能做什么、不能做什么
// ---------------------------------------------------------------------------
// OnHttpResponse / OnHttpError / OnClosed **在本模块的锁之外发出**，所以：
//
//   (1) **可以**在回调里直接调 Request() / Close()，立即执行，不会自锁。
//   (2) **可以**在回调里取业务自己的锁 —— 本模块的锁不会出现在你的栈帧下方。
//
//       ⚠️ **但反过来不行：不得在持有业务锁的情况下析构 HttpConnector。**
//       析构会等所有在途回调跑完，而那一段等待**没有上限**（加超时就等于把
//       UAF 放回来，见第 5 条）。于是：
//           线程 A：持业务锁 M -> delete connector -> 无上限等在途回调
//           线程 B：在途回调 -> 想拿业务锁 M -> 等 A
//       = 永久死锁。析构必须在**不持任何业务锁**的位置做。
//       （回调里取 M、以及"持 M 调 Request()/Close()"都没问题；唯独
//        "持 M 析构"不行。）
//   (3) **不要**在回调里析构 HttpConnector。本模块会检测到并打一条
//       "**违约用法**"的 _ERROR_，然后跳过等待直接放弃 —— 不会冻结，但
//       **在途请求被静默丢弃、不再回调任何 handler**。原因是本次回调所属的
//       那条连接要等回调返回之后才收尾，析构根本等不到它。
//       想要确定性收尾，请把析构挪出回调栈（例如投递到自己的事件循环里做）。
//   (4) 仍然**不要在回调里做耗时操作**：一条连接的回调没返回，它就不会收尾，
//       Close() / 析构 也就等不到它。这是"慢"，不是"死"。
//
// ⚠️ OnLog 是**另一回事，限制严格得多**：
//   BC 的日志器是同步的，OnLog 是在它持有一把**全局非递归自旋锁**的情况下被
//   调用的。因此在 OnLog 里**不得**调用本模块任何会打日志的 API ——
//   Request() / Close() / 析构 全都会打日志，会重进那把非递归自旋锁而 100%
//   CPU 空转挂死（同线程），或与另一线程构成锁序反转而死锁。
//   OnLog 里只应做"把字符串拷走"这类不回头调用本模块的事。
//   这是 BC 日志器的结构约束，本模块无法从内部消除，但做了两件事把伤害兜住：
//     * Request() 在 OnLog 栈帧里被调用时**直接返回 BC_R_NOTIMPLEMENTED**，
//       把一个无诊断的 100% CPU 挂死换成调用方看得见的错误码；
//     * 本模块自己在 OnLog 栈帧里产生的日志会被丢弃，不会重进那把锁。
//   Close() 不打日志，因此在 OnLog 里调它是安全的；析构会打日志，**不要**在
//   OnLog 里析构连接器。
//
// 补充（实现侧的不变量，供维护者参考）：本模块内部**持锁时禁止打日志**，
// 正是为了让上面 (1)(2)(3) 成立。细节与保障手段见 HttpConnector.cpp 顶部
// "state->lock 的 RAII 包装 + 持锁时禁止打日志的不变量"。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_HTTP_CONNECTOR_H
#define TT_HTTP_CONNECTOR_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <string>

#include <BC/BCFCodec.h>

#include "LLHTTPParser.h"
#include "TcpChannel.h"
#include "TTErrors.h"

///////////////////////////////////////////////////////////////////////////////
// struct : HttpRequest / HttpResponse
///////////////////////////////////////////////////////////////////////////////

struct HttpRequest
{
    std::string   method    = "GET";
    std::string   url;                    // http:// 或 https://
    HttpHeaderMap headers;
    std::string   body;
    uint32_t      timeoutMs = 10000;      // 覆盖 DNS + connect + TLS + 收响应
    std::string   resolvedIp;             // 非空则跳过 DNS
};

struct HttpResponse
{
    int           status       = 0;
    // status line 的 reason-phrase，如 "Not Found"。超长会被截断并判失败。
    //
    // ⚠️ **原始网络字节，未做任何净化**：llhttp 不过滤 reason-phrase 的字符，
    // 所以这里可能含控制字符（包括 ESC，足以往终端注入 ANSI 转义序列）与
    // 内嵌 NUL。直接打到终端、写进日志、或塞进 HTML 之前必须自己过滤。
    // tools/httpget.cpp 的 PrintSanitized() 是一个可抄的最小实现。
    std::string   reason;
    // 键统一小写。
    //
    // 关于净化：头**名**已由 llhttp 限制为 token 并被我们转成小写；头**值**
    // 的字符集也由 llhttp 卡住 —— 逐字节实测（deps/llhttp 2.0.4）只放行
    // TAB(0x09)、0x20..0x7E 与 0x80..0xFE，其余控制字符连同 0x7F(DEL) 和 0xFF
    // 一律 HPE_INVALID_HEADER_TOKEN 并把整条响应判废。所以这里**不存在**
    // reason 那种 ANSI 注入（ESC 0x1B 进不来）。仍属远端数据，别当可信输入用
    // （长度、内容都由对端决定），但不需要为终端转义单独过滤。
    // 边界由集成测试 case Z 钉住。
    //
    // ⚠️ 同名头以 ", " 合并（RFC 7230 3.2.2）。**这会拼坏 Set-Cookie** ——
    // 它是那条规则的著名例外，多张 cookie 各自独立，逗号合并之后没法可靠地
    // 拆回来（cookie 的 Expires 属性里本身就含逗号）。本模块定位是 JSON API
    // 客户端，不做 cookie 处理；需要完整 Set-Cookie 的调用方不能用这里的
    // headers，得先给本模块加一个"原始头列表"的出口。
    HttpHeaderMap headers;
    std::string   body;
    // 以下三项让业务无需翻日志就能判断这次请求有没有真的走物理网卡
    std::string   peerIp;
    uint32_t      boundIfIndex = 0;
    std::string   pinMethod;
};

///////////////////////////////////////////////////////////////////////////////
// struct : HttpUrlParts —— 纯函数解析结果，可单测
///////////////////////////////////////////////////////////////////////////////

struct HttpUrlParts
{
    std::string scheme;        // 小写的 "http" / "https"
    std::string host;          // 域名或 IP 字面量；IPv6 已剥掉方括号
    std::string hostHeader;    // Host 头原样取值：IPv6 带方括号，非默认端口带 :port
    uint16_t    port = 0;
    std::string target;        // 请求行里的 request-target，至少是 "/"，已去掉 #fragment
    bool        tls  = false;
};

///////////////////////////////////////////////////////////////////////////////
// class : IHttpRequestHandler / IHttpConnectorHandler
///////////////////////////////////////////////////////////////////////////////

class IHttpRequestHandler
{
public:
    IHttpRequestHandler() {}
    virtual ~IHttpRequestHandler() {}

    virtual void OnHttpResponse(const HttpResponse& resp)                 = 0;
    virtual void OnHttpError(BCRESULT result, const std::string& message) = 0;
};

class IHttpConnectorHandler
{
public:
    IHttpConnectorHandler() {}
    virtual ~IHttpConnectorHandler() {}

    virtual void OnLog(int level, LPCSTR msg) = 0;
    virtual void OnClosed()                   = 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : HttpConnector
///////////////////////////////////////////////////////////////////////////////

class HttpConnection;
class PooledChannel;

///////////////////////////////////////////////////////////////////////////////
// struct : HttpLogSink —— 外部日志 appender 的中转块
//
// AddExternalLogAppender 只能存一个 void*，直接把 HttpConnector* 交进去的话，
// 连接器一析构那个指针就是野的。垫这一层之后，appender 拿到的是本块，
// handler 可以在连接器放手时被安全置空，回调随即变成 no-op。
//
// 本块的所有权在 HttpConnectorState 上（连同 appender 一起），所以它的存活期
// 自动覆盖到最后一条残留连接 —— 既不悬垂，也不需要靠泄漏来回避。
///////////////////////////////////////////////////////////////////////////////

struct HttpLogSink
{
    // 保护 handler：放弃路径会把它置空，而那一刻别的线程可能正在打日志。
    //
    // ⚠️ 曾经把它改成递归锁，理由是"业务若在 OnLog 里析构连接器，析构还要再拿
    // 一次这把锁"。那个理由不成立：那条路径无论如何都挂 —— 析构最终会走到
    // RemoveLogAppender，而它要拿 BCLogger 的**全局非递归自旋锁**，可那把锁
    // 正被当前这次 OnLog 调用持有着。递归化只是把一个自锁换成另一个自锁。
    // 所以改回普通 mutex，并在头文件契约里明写"不要在 OnLog 里析构连接器"。
    std::mutex             lock;
    IHttpConnectorHandler* handler = NULL;
};

///////////////////////////////////////////////////////////////////////////////
// struct : HttpConnectorState —— 连接器的共享状态，**比连接器本身活得久**
//
// 为什么必须独立出来：析构放弃等待时 HttpConnector 会先于残留的
// HttpConnection 消失，而那些连接稍后还要回来销账（从 conns 摘除、通知 cv、
// 判断要不要发 OnClosed）。只要这些字段还长在 HttpConnector 身上，任何"回来
// 销账"的动作都是在碰已析构的对象。
//
// 早前试过"放弃前把连接回指连接器的裸指针掐断"，那只堵住了一半：负责 delete
// 连接的那个 Runtime 任务是**按值捕获裸指针**的，绕过了掐断这一步。
// drainTimeoutMs 越小越容易命中（取 0 时每次带在途请求的析构都必走这条路），
// 实测压测 7 次运行 7 次崩（heap-use-after-free / mutex lock failed）。
//
// 所以改成：连接器、每条连接、以及每个 delete 任务各持一份 shared_ptr，
// 最后一个撒手时本结构才析构。日志 appender 也挂在这里一起回收 —— 残留连接
// 和它们的 TcpChannel 都还握着 logger_ctx 在打日志，appender 必须活到最后
// 一条连接消失为止（提前移除就是野指针，实测 SEGV 在 BCLogger::Log）。
//
// ---------------------------------------------------------------------------
// 锁
// ---------------------------------------------------------------------------
// 一把递归锁保护下面所有字段。两个刻意的选择：
//
//   * **只有一把锁**：连接与连接器之间不再有第二把锁，也就没有锁序问题。
//   * **递归锁**：纯属保守。业务回调一律在**锁外**发出（见 HttpConnector.cpp
//     顶部"回调派发"那段），所以正常路径下不存在重入；递归锁只是万一哪条路径
//     被漏掉时不至于硬死锁。
//     （std::condition_variable 不支持 recursive_mutex，故配 condition_variable_any。）
//
// ⚠️ **持这把锁时禁止打日志**（禁止 LogQ 及任何会打日志的调用）。BC 的日志器
// 带一把全局非递归自旋锁，且持着它回调业务的 OnLog；持本锁打日志就构成
// state->lock -> BCLogger 全局锁，与"业务在 OnLog 里调 Request()/Close()"
// 正好反向，两线程 100% 死锁。细节与保障手段见 HttpConnector.cpp 顶部。
///////////////////////////////////////////////////////////////////////////////

struct HttpConnectorState
{
    std::recursive_mutex        lock;
    std::condition_variable_any cv;

    // 还没走完 OnChannelClosed 的连接
    std::set<HttpConnection*>   conns;
    // 已走完 OnChannelClosed、但负责 delete 它的 Runtime 任务还没跑完的连接。
    // 单独记一份而不是只用计数，是为了让放弃路径也能遍历到它们。
    std::set<HttpConnection*>   destroying;

    // ------------------------------------------------------------------
    // keep-alive 连接池：响应收完之后还活着、可以交给下一条请求的连接。
    //
    // key 由 _PoolKey() 生成，把所有"换一条连接就会变"的东西都拼进去
    // （host/port/tls/policy/网卡）——池化最危险的错误就是把一条连接交给
    // 一个不该走它的请求。
    //
    // 池里的每条连接都由它自己的 PooledChannel 看着（既是 owner 也是
    // ITcpChannelHandler）。**它们不在 conns / destroying 里**：那两个集合
    // 记的是"在途请求"，而池里的连接恰恰是没有请求的。
    // ------------------------------------------------------------------
    std::map<std::string, std::vector<PooledChannel*> > idle;
    // 每个 key 最多囤这么多条空闲连接。0 = 完全不复用（每条请求一条连接，
    // 请求头里发 Connection: close），用来一键退回旧行为。
    size_t                      max_idle_per_key = 4;
    // 已经从 idle 里摘掉、但负责销毁它的 Runtime 任务还没跑完的池化连接数。
    //
    // ⚠️ 它与 destroying 是同一个道理（见那里的注释）：摘除与销毁之间有间隙，
    // 排空判定只看 idle 的话，析构会在这个间隙里认为"池子空了"而返回，调用方
    // 随即释放 handler —— 紧接着那个销毁任务跑起来去发 OnClosed，就是 UAF。
    // ASan 实测：SEGV 在 _NotifyClosedIfDrained 的 handler->OnClosed()，
    // 偶发（三次里中一次）。
    size_t                      idle_destroying = 0;
    // 一条连接在池里躺多久还没人用就自行关掉。每次归还重新计时。
    //
    // 这是一道**兜底**，不是"多久算空闲"的最优解：正常情况下服务端自己的
    // keep-alive 超时（通常几十秒到几分钟）会先把连接收走，我们这边收到 FIN
    // 就回收了。它只负责挡住"永不主动关连接"的服务端 —— 没有它，对着那种
    // 服务端只发一次请求，那条 fd 会一直占到连接器关停。
    // 0 = 不设本地超时，完全听服务端的。
    uint32_t                    idle_timeout_ms = 60 * 60 * 1000;   // 60 分钟

    IHttpConnectorHandler*      handler         = NULL;
    bool                        closing         = false;
    bool                        closed_notified = false;
    // 连接器已放弃等待。此后**任何** handler 都不再回调（连接器级的 OnClosed
    // 与请求级的 OnHttpResponse / OnHttpError 都不发）—— 调用方在析构返回之后
    // 就可以释放它们了。
    bool                        abandoned       = false;
    // 正在**锁外**执行的业务回调条数。回调不在锁内发（理由见 HttpConnector.cpp
    // 顶部"回调派发"那段），改用这个计数来保证"调用方拿回控制权时，没有任何
    // 回调还握着它的 handler"：放弃路径清空 handler 之后要等它归零才返回。
    uint32_t                    callbacks_in_flight = 0;

    // 日志 appender 的所有权，见上面的说明。
    void*                       logger_ctx      = NULL;
    bool                        own_logger      = false;
    HttpLogSink*                log_sink        = NULL;

    ~HttpConnectorState();
};

class HttpConnector
{
public:
    HttpConnector();
    ~HttpConnector();

    // config 键（全部可选，pConfig 可传 NULL）：
    //   vpnPolicy          string  "os" / "prefer-physical" / "force-physical"
    //   androidNetHandle   int     Android Network.getNetworkHandle() value
    //   bypassVpn          bool    已废弃；与 vpnPolicy 同时给出时以后者为准
    //   dnsServers         array   自建 DNS 查询用的服务器列表（字符串 IP）
    //   dnsTimeoutMs       int     单台 DNS server 的超时，默认 1000
    //   caCerts            string  PEM 文本。空则回落系统信任库
    //   spkiPin            string  base64 SPKI pin，给了就只比对 pin
    //   insecureSkipVerify bool    仅供自签调试，会打 _WARN_
    //   maxResponseBytes   int     响应体上限，默认 8 MiB。**只管报文体**；
    //                              响应头另有一组独立的硬上限（总量 64 KiB /
    //                              单条 16 KiB / 200 条 / reason 512 字节），
    //                              取值与依据见 HttpConnector.cpp 顶部
    //   drainTimeoutMs     int     析构时等待在途请求收尾的上限，默认 30000，
    //                              上限 300000（5 分钟），越界会夹住并打 _WARN_。
    //                              **0 是合法值**：不等，掐断所有回调后立即返回。
    //                              代价见下面契约第 6 条
    //   maxIdleConnections int     keep-alive 连接池里每个目标（host+port+
    //                              tls+网卡策略）最多囤几条空闲连接，默认 4，
    //                              上限 64（越界夹住）。**0 = 关掉复用**，
    //                              退回"一次请求一条连接"，请求头里会显式发
    //                              Connection: close
    //   idleTimeoutMs      int     空闲连接在池里躺多久没人用就自行关掉，
    //                              默认 3600000（60 分钟），上限 86400000
    //                              （24 小时，越界夹住并打 _WARN_）。每次归还
    //                              重新计时。**0 = 不设本地超时**，完全听
    //                              服务端的。运行期可用 SetIdleTimeoutMs 改
    //   logLevel           int     BC 日志级别（1=error 2=warn 3=info 4=debug）
    //   logFile            string  给了就落文件，否则日志走 handler->OnLog
    BCRESULT Create(BCFObject* pConfig, IHttpConnectorHandler* handler);

    // 发起一次请求。参数校验同步进行；其余全部异步，结果走 handler。
    // outErrMessage 非 NULL 时，**同步失败**（返回非 BC_R_SUCCESS）的现场描述
    // 写进去；成功时清空。异步失败的文案照旧走 IHttpRequestHandler::OnHttpError
    // 的 message 形参。
    //
    // ⚠️ 同步路径上没有 OnHttpError 可用，不接这个出参的话，force-physical
    // 拿不到物理网卡时业务只看得到一个 BC_R_NO_PHYSICAL_INTERFACE(64)。
    BCRESULT Request(const HttpRequest& req, IHttpRequestHandler* handler,
                     std::string* outErrMessage = NULL);

    // 幂等。关掉所有在途请求；全部收尾后回调 IHttpConnectorHandler::OnClosed。
    void     Close();

    // ------------------------------------------------------------------
    // Create() 解析出来的最终配置。给诊断与单测看的只读视图 —— 配置合并
    // （vpnPolicy 与 bypassVpn 的优先级、各种默认值）出错时症状往往是"连上了
    // 但走错网卡"，不暴露的话只能靠翻日志倒推。
    // ------------------------------------------------------------------
    TTVpnPolicy      EffectivePolicy()  const { return policy_; }
    size_t           MaxResponseBytes() const { return max_response_bytes_; }
    uint32_t         DrainTimeoutMs()   const { return drain_timeout_ms_; }
    // 连接池的两项，读的是共享状态，所以不是 const 的裸成员访问。
    size_t           MaxIdleConnections() const;
    uint32_t         IdleTimeoutMs()      const;

    // 运行期调整空闲连接的存活上限。0 = 不设本地超时。上限 24 小时（越界夹住
    // 并打 _WARN_）。
    //
    // ⚠️ 只影响**此后**归还入池的连接：已经躺在池里的那些仍按各自入池时的
    // 时长计时。要让新值立刻对全部连接生效，改完再 Close() 重建连接器。
    void             SetIdleTimeoutMs(uint32_t ms);
    const DnsConfig& DnsSettings()      const { return dns_cfg_; }
    const TlsConfig& TlsSettings()      const { return tls_cfg_; }

    // ------------------------------------------------------------------
    // 下面两个是纯函数，不碰任何成员、不发起任何 IO，专供单测。
    // ------------------------------------------------------------------

    // 解析 http/https URL。不做 DNS —— 这是刻意的：
    // HTTPProtocol::ParseAddrFromUrl 内部会同步调 getaddrinfo，既阻塞调用线程，
    // 又绕开了 DnsResolver 的"不走 VPN"路径（TUN 全局模式下会拿到 fake-IP）。
    // 失败返回 false 并填 outErr。
    static bool ParseUrl(const std::string& url, HttpUrlParts& out,
                         std::string& outErr);

    // 组装请求报文。Host / Content-Length / Connection 由本函数权威决定，
    // req.headers 里的同名项被忽略（避免请求走私）；User-Agent 允许覆盖。
    // 头名或头值里出现 CR/LF 视为注入攻击，返回 false。
    // keepAlive = false 时在请求头里显式写 Connection: close（禁用连接池时
    // 的行为）；true 则什么都不发 —— HTTP/1.1 默认就是 keep-alive。
    static bool BuildRequestText(const HttpRequest& req,
                                 const HttpUrlParts& parts,
                                 std::string& out,
                                 std::string& outErr,
                                 bool keepAlive = false);

private:
    DECLARE_NO_COPY_CLASS(HttpConnector);
    friend class HttpConnection;

private:
    // 共享状态。连接器只是它的持有者之一，见 HttpConnectorState 顶部注释。
    std::shared_ptr<HttpConnectorState> state_;

    void*                       logger_ctx_    = NULL;   // state_->logger_ctx 的副本

    TTVpnPolicy                 policy_        = TT_VPN_POLICY_UNSET;
    uint64_t                    android_net_handle_ = 0;
    DnsConfig                   dns_cfg_;
    TlsConfig                   tls_cfg_;
    size_t                      max_response_bytes_ = 8 * 1024 * 1024;
    uint32_t                    drain_timeout_ms_   = 30000;
    uint64_t                    bad_drain_timeout_  = 0;   // 越界的原始取值，供告警
    uint64_t                    bad_idle_timeout_   = 0;   // 同上，idleTimeoutMs 的
    bool                        created_       = false;

    static void _LogCallback(void* data, int level, LPCSTR msg);
};

#endif // TT_HTTP_CONNECTOR_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
