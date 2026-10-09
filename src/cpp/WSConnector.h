///////////////////////////////////////////////////////////////////////////////
// file : WSConnector.h
// author : zhoukai88@jd.com（原始实现）/ anto（移植进 ttsignal，接 TcpChannel）
//
// WebSocket 客户端（RFC 6455）。建在 TcpChannel 上，支持 ws:// 与 wss://，
// **host 可以是域名**（域名解析走 TcpChannel 里的 DnsResolver，因此
// force-physical 下不会被 VPN 的 TUN 劫持成 fake-IP）。
//
// 与 HttpConnector 是同一条协议栈上的两个上层协议：连接、绑网卡、路由复核、
// TLS 校验全部由 TcpChannel 负责，本文件只管 HTTP Upgrade 握手与 WS 帧。
//
// ---------------------------------------------------------------------------
// 从 jmp 移植时的实质变化（不只是改名字）
// ---------------------------------------------------------------------------
//   * 原实现把 HTTPProtocol::ParseAddrFromUrl 返回的 host 直接喂给 bc_net_pton，
//     **只能连 IP 字面量**；而 ParseAddrFromUrl 内部是同步阻塞的 getaddrinfo，
//     既堵调用线程又绕开了 DnsResolver 那条不走 VPN 的路径。现在改成纯函数
//     ParseUrl() + TcpChannel（异步 DNS）。
//   * 原实现建了 SSL_CTX 但**不校验证书、不设 SNI、min version 是 TLS1.1**。
//     现在走 TlsContext：证书链 + hostname 校验，可选 SPKI pin。
//   * 原实现发出去的帧**不掩码**（PackPacket 的 has_mask 默认 false）。RFC 6455
//     5.1 要求客户端必须掩码，合规服务端（gorilla / nginx / Cloudflare）会直接
//     以 1002 关连接 —— 也就是说原实现连不上任何标准服务端。现在一律掩码。
//   * 原实现在持自旋锁的情况下回调业务，中间靠手工 Unlock/Lock 开天窗。
//     现在照 HttpConnector 的做法：**锁内登记、锁外执行、回来注销**。
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
// Create / CreateConnection / Connect / Send* / Close 可以在任意线程调用。
// IWSConnectionHandler 的回调都发生在该连接对应 TcpChannel 的事件循环线程上
// （每条连接一条通道，互不干扰）；IWSConnectorHandler::OnClosed 发生在最后一条
// 连接收尾的那条线程上。
//
// ---------------------------------------------------------------------------
// 生命周期契约
// ---------------------------------------------------------------------------
// 1. Connect() 返回非 BC_R_SUCCESS 时不会有任何回调。
// 2. Connect() 返回 BC_R_SUCCESS 之后：
//      * OnConnectResult 恰好回调一次；
//      * result == BC_R_SUCCESS 时，之后还会恰好回调一次 OnClosed；
//      * result != BC_R_SUCCESS 时**不再有 OnClosed**（握手没成功，也就没有
//        "连接关闭"这件事）。
//    handler 必须活到最后一次回调返回。
// 3. WSConnPtr 是 shared_ptr，但**业务放手不等于对象销毁**：连接器会一直持有
//    一份强引用，直到该连接彻底收尾（TcpChannel 发过 OnChannelClosed 且它的
//    TcpChannel 已经销毁）。因此业务可以在任何时刻（含回调里）丢掉自己那份
//    WSConnPtr，不会踩到"销毁一条还活着的通道"。
// 4. Close() 幂等；所有在途连接走完之后回调 IWSConnectorHandler::OnClosed。
// 5. ~WSConnector 会自动 Close() 并**同步等待**在途连接收尾，所以析构本身安全；
//    但它会阻塞调用线程，不要在事件循环线程上析构。
// 6. ⚠️ **Runtime 必须活得比 WSConnector 久**（与 HttpConnector 同一条约束）。
//    内部对象的销毁是 Runtime::PostTask 出去做的，而 Runtime::PostTask 在
//    Runtime 已经 Destroy() 之后会被静默丢弃（Runtime.cpp:129）。正确顺序是：
//        WSConnector 析构（或离开作用域） -> Runtime::Destroy()
//    为了不把错误顺序变成永久死锁，析构里排空在途连接那一段等待带上限
//    （drainTimeoutMs，默认 30 秒），超时会打 _ERROR_ 日志并**放弃等待**。
// 7. 放弃等待之后：**任何 handler 都不再回调**（连接器的 OnClosed 与连接的
//    OnConnectResult / OnRecvText / OnRecvData / OnClosed / OnException 都不发），
//    所以析构一返回，调用方就可以安全释放所有 handler。残留连接会在后台自行
//    收尾并回收共享状态，不会悬垂、不会泄漏 —— 除非销毁顺序反了（见第 6 条）。
//
// ---------------------------------------------------------------------------
// 8. 回调里能做什么、不能做什么
// ---------------------------------------------------------------------------
// 所有业务回调都在**本模块的锁之外**发出，所以：
//   (1) 可以在回调里直接调 SendText / SendData / Close / CreateConnection；
//   (2) 可以在回调里取业务自己的锁；
//   (3) ⚠️ **不得在持有业务锁的情况下析构 WSConnector** —— 析构会等在途回调
//       跑完，而那一段等待没有上限（加超时等于把 UAF 放回来）。
//   (4) 不要在回调里析构 WSConnector：本模块会检测到并打一条"违约用法"的
//       _ERROR_，然后跳过等待直接放弃（在途连接被静默丢弃）。
//   (5) 不要在回调里做耗时操作 —— 那条连接的收尾会一直等着它返回。
//
// ⚠️ IWSConnectorHandler::OnLog 是另一回事，限制严格得多：BC 的日志器是同步的，
//   OnLog 是在它持有一把**全局非递归自旋锁**的情况下被调用的。因此在 OnLog 里
//   **不得**调用本模块任何会打日志的 API（Create / CreateConnection / Connect /
//   析构），否则重进那把非递归锁就是 100% CPU 空转挂死（同线程），或与另一线程
//   构成锁序反转而死锁。OnLog 里只应做"把字符串拷走"这类事。
//   Close() / SendText() / SendData() 不打日志，在 OnLog 里调是安全的。
//
// 补充（实现侧的不变量）：本模块内部**持锁时禁止打日志**，保障手段见
// WSConnector.cpp 顶部"StateLock + WS_LOGQ"那一段。
///////////////////////////////////////////////////////////////////////////////
#ifndef WSCONNECTOR_H_INCLUDED__
#define WSCONNECTOR_H_INCLUDED__

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <BC/BCFCodec.h>
#include <BC/BCFStream.h>
#include <BC/BCFixedAlloc.h>
#include <BC/BCLog.h>          // for _ERROR_ / _WARN_ / _INFO_ / _DEBUG_
#include <BC/BCStream.h>

#include "Interface.h"
#include "SMPacket.h"
#include "TcpChannel.h"
#include "TTErrors.h"
#include "WSParser.h"


using namespace BC;
using namespace SMP;

///////////////////////////////////////////////////////////////////////////////
// Namespace : WS
///////////////////////////////////////////////////////////////////////////////

namespace WS
{

class WSConnector;
// 连接器与所有连接共享的状态。定义在 .cpp 里 —— 它比连接器本身活得久，
// 理由与 HttpConnectorState 完全一致。
struct WSConnectorState;

///////////////////////////////////////////////////////////////////////////////
// struct : WSUrlParts —— ParseUrl 的产物，纯数据，可单测
///////////////////////////////////////////////////////////////////////////////

struct WSUrlParts
{
	std::string	scheme;			// 小写的 "ws" / "wss"
	std::string	host;			// 域名或 IP 字面量；IPv6 已剥掉方括号
	std::string	hostHeader;		// Host 头原样取值：IPv6 带方括号，非默认端口带 :port
	uint16_t	port = 0;
	std::string	target;			// 请求行里的 request-target，至少是 "/"
	bool		tls = false;
};

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection
///////////////////////////////////////////////////////////////////////////////

class WSConnection 
	: public LLHTTPParser
	, public WSParser
	, public ITcpChannelHandler
{
	DECLARE_FIXED_ALLOC(WSConnection);

	friend class WSConnector;

public:
    ///////////////////////////////////////////////////////////////////////////
    // class : Config —— 单条连接的配置
    //
    // 可拷贝：连接器先把自己解析好的默认值拷进来，再用 CreateConnection 的
    // pConfig 覆盖个别键。
    ///////////////////////////////////////////////////////////////////////////

    class Config
    {
    public:
        Config() {}
        ~Config() {}

		// 只覆盖 pConfig 里出现过的键，其余保持原值。pConfig 可为 NULL。
        BCRESULT		Init(BCFObject* pConfig);

		// 收包侧单帧（以及分片消息重组后）的 payload 上限
		size_t			maxFrameBytes		= WS_DEFAULT_MAX_FRAME_BYTES;
		// 越界被夹住的 maxFrameBytes 原始取值，供告警；0 表示没越界
		uint64_t		bad_max_frame_bytes	= 0;
		// 覆盖 DNS + connect + TLS + **WebSocket 握手响应**的总时长
		uint32_t		connectTimeoutMs	= 10000;
		// 主动发 ping 的间隔，0 = 不发
		uint32_t		pingIntervalMs		= 0;
		// 多久没有任何网络动作就判空闲并关连接，0 = 不判
		uint32_t		idleTimeoutMs		= 0;
		// 显式指定要绑的网卡；0 = 交给 TcpChannel 自动探测。
		// Android 上没有自动探测手段，force-physical 必须填 androidNetHandle。
		uint32_t		ifIndex				= 0;
		uint64_t		androidNetHandle	= 0;
		TTVpnPolicy		policy				= TT_VPN_POLICY_UNSET;
		DnsConfig		dns;
		TlsConfig		tls_cfg;
    };

public:
	WSConnection();
	~WSConnection() override;

	// 由 WSConnector::CreateConnection 调用，业务不要直接用。
	BCRESULT		Create(
						const std::shared_ptr<WSConnectorState> &state,
						BCFObject *pConfig,
						const Config &defaults,
						uint64_t id,
						IWSConnectionHandler *pHandler);
	// url 形如 ws://host[:port]/path 或 wss://host[:port]/path，host 可以是域名。
	// timeout_in_millisec 为 0 时用 Config::connectTimeoutMs。
	// 参数校验同步进行；返回 BC_R_SUCCESS 之后一切结果走 handler。
	BCRESULT		Connect(
						const std::string& url, 
						uint32_t time_out_in_millisec);
	// 追加到握手请求里的头。必须在 Connect() 之前调用，之后调用返回
	// BC_R_ALREADYRUNNING。Host / Upgrade / Connection / Sec-WebSocket-* 由本模块
	// 权威决定，业务给的同名项会被忽略。
	BCRESULT		SetRequestHeader(
						const std::string& name,
						const std::string& value);
	// ⚠️ 不校验 text 是不是合法 UTF-8（RFC 6455 5.6 要求文本帧必须是 UTF-8）。
	// 收到的文本帧同样不校验。需要严格性的调用方自己把关。
	BCRESULT		SendText(const std::string& text);
	BCRESULT		SendData(LPCVOID data, size_t size);
	// pkt->origion_data 以 binary 帧发出（SMPacket 没有 ws_type 字段，见
	// WSParser::PackPacket 的注释）
	BCRESULT		SendPacket(SMPacketPtr pkt);
	BCRESULT		SendPing();
	// 幂等。先尽力发一个 close 帧（1000 Normal Closure），再关通道。
	void			Close(BCRESULT result = BC_R_SUCCESS);
	// 把收到的明文（TLS 解密后、WS 解帧前）落盘，排查用
	BCRESULT		OpenDumpFile(const char* fileName);
	void			CloseDumpFile();

	IWSConnectionHandler *
					GetHandler() const;
	uint64_t		Id() const { return id_; }
	bool			IsUpgraded() const { return upgraded_.load(); }
	// 诊断信息，业务据此判断这条连接是否真的走了物理网卡
	std::string		PeerIp() const;
	uint32_t		BoundIfIndex() const;
	std::string		PinMethod() const;
	// 最近一次收尾时的失败现场描述（TcpChannel 给的 force-physical 诊断、握手校验
	// 到底哪一项不对……）。连上正常、或还没收尾时为空串。
	//
	// ⚠️ 存在的理由：IWSConnectionHandler::OnConnectResult 的签名里**没有 message
	// 形参**（Interface.h:255-257，与 jmp 对齐，JNI / Swift 绑定都按它实现了），
	// 而这段文案是 force-physical 现场排查的唯一依据 —— 只落日志的话，绑定层能
	// 交给业务的就只剩一个错误码。绑定层（NAPI / JNI / Swift）应当在
	// OnConnectResult / OnClosed 里调它，把文案原样带给业务。
	// 值在 _DeliverOnce 派发回调**之前**就写好了，所以在回调里读一定读得到。
	// 任意线程可调（与 PeerIp() 同一把锁）。
	std::string		LastErrorMessage() const;

	// ------------------------------------------------------------------
	// 纯函数，不碰任何成员、不发起任何 IO，专供单测。
	// ------------------------------------------------------------------

	// 解析 ws/wss URL。**不做 DNS** —— HTTPProtocol::ParseAddrFromUrl 内部会
	// 同步调 getaddrinfo，既阻塞调用线程，又绕开 DnsResolver 的"不走 VPN"路径
	// （TUN 全局模式下会拿到 fake-IP）。失败返回 false 并填 outErr。
	static bool		ParseUrl(
						const std::string& url,
						WSUrlParts& out,
						std::string& outErr);
	// 组装 WebSocket Upgrade 请求报文。头名/头值里出现 CR/LF 视为注入，返回 false。
	static bool		BuildHandshakeRequest(
						const WSUrlParts& parts,
						const std::string& secKey,
						const HttpHeaderMap& extraHeaders,
						std::string& out,
						std::string& outErr);
	// 校验握手响应。status 必须是 101，Upgrade/Connection/Sec-WebSocket-Accept
	// 都必须对得上。失败返回 false 并填 outErr。
	static bool		CheckHandshakeResponse(
						int status,
						const HttpHeaderMap& headers,
						const std::string& secKey,
						std::string& outErr);

private:
	DECLARE_NO_COPY_CLASS(WSConnection);

	// Override ITcpChannelHandler interfaces —— 全部跑在通道的事件循环线程上
	void			OnChannelReady() override;
	void			OnChannelData(const void* data, size_t size) override;
	void			OnChannelClosed(
						BCRESULT result,
						const std::string& reason) override;
	// Override WSParser interfaces
	int				OnWSWrite(
						std::shared_ptr<BCBuffer> data, 
						void* user_data = NULL) override;
	void			OnRecvWSFrame(
						const WSFrameHeader& header, 
						const uint8_t* payload,
						size_t payload_len) override;
	// Override LLHTTPParser interfaces
	int				http_on_url(const char* at, size_t length) override;
	int				http_on_status(const char* at, size_t length) override;
	int				http_on_header_field(const char* at, size_t length) override;
	int				http_on_header_value(const char* at, size_t length) override;
	int				http_on_headers_complete() override;
	int				http_on_message_complete() override;

	// Internal functions
	// 由 ~WSConnector 的放弃路径调用，**调用方必须已持有 state->lock**
	void			ClearHandlerLocked();
	// 由 WSConnector::Close / 析构调用，**调用方必须已持有 state->lock**。
	// 返回 true 表示这条连接从没 Open 过，调用方应就地把它从 conns 摘除
	// （不会有 OnChannelClosed 来摘它）。
	bool			_CancelLocked();
	// 把明文喂给 WSParser，并把它抛出的 BCException 转成 OnException + 关连接
	void			_FeedFrames(const void* data, size_t size);
	// 握手响应头的记账与上限。返回 false 表示已记好失败原因，调用方应立刻
	// 从 llhttp 回调里 return -1。
	bool			_CommitHeader();
	bool			_ChargeHeaderBytes(size_t length);
	bool			_CheckSingleHeaderSize(size_t incoming);
	// 发一帧。opcode 为控制帧时 payload 不得超过 125 字节；一律加掩码。
	BCRESULT		_SendFrame(uint8_t opcode, BufferPtr payload);
	// 把已经封好的帧交给通道。**任意线程可调**，内部持 state->lock 读 channel_。
	BCRESULT		_SendRawFrame(BufferPtr frame);
	void			_SendPong(const uint8_t* payload, size_t payload_len);
	void			_SendCloseFrame(uint16_t code);
	// 记下失败原因并推动通道关闭，真正的回调统一在 OnChannelClosed 里发。
	// ⚠️ 只在通道那条事件循环线程上调用。
	void			_Fail(BCRESULT result, const std::string& message);
	void			_DeliverConnectResult(BCRESULT result);
	// 持 state_->lock 写 last_error_message_。**必须在派发回调之前调**。
	void			_SetLastErrorMessage(const std::string& message);
	void			_DeliverOnce(BCRESULT closeResult, const std::string& closeReason);
	void			_ClassifyClose(BCRESULT result);
	void			_OnActiveCheck();
	void			_TouchNetAction();
	void			_WriteDump(const void* data, size_t size);
	std::string		_CloseReasonText(BCRESULT result, const std::string& raw) const;

private:
	// 共享状态。它比连接器活得久，所有回连接器的销账动作都经由它。
	std::shared_ptr<WSConnectorState>	state_;
	uint64_t							id_ = 0;
	Config								config_;
	// 业务 handler。由 state_->lock 保护（放弃路径会在同一把锁下置空）
	IWSConnectionHandler			*	handler_ = NULL;
	void							*	logger_ctx_ = NULL;

	// 通道。由 state_->lock 保护：OnChannelClosed 会把它摘下来交给 Runtime
	// 任务去销毁，与"别的线程正拿它 Send"必须互斥。
	//
	// ⚠️ 例外：在通道自己的事件循环线程上（OnChannelReady / OnChannelData /
	// OnRecvWSFrame / 挂在通道上的定时器回调）channel_ 一定非空，因为置空发生在
	// OnChannelClosed 里、而那是通道发出的最后一个回调。那些地方可以直接用。
	TcpChannel						*	channel_ = NULL;
	// Open() 调过且成功 —— 决定析构时能不能直接 delete channel_
	bool								channel_opened_ = false;
	std::atomic_bool					connect_called_;
	// 覆盖 DNS + connect + TLS + 握手响应的总预算，Connect() 时定下
	uint32_t							timeout_ms_ = 10000;

	// 握手
	std::string							url_;
	WSUrlParts							parts_;
	std::string							sec_key_;
	HttpHeaderMap						extra_headers_;
	std::string							req_text_;
	std::atomic_bool					upgraded_;
	int									hs_status_ = 0;
	HttpHeaderMap						hs_headers_;
	size_t								hs_header_bytes_ = 0;
	std::string							cur_field_;
	std::string							cur_value_;
	bool								last_was_value_ = false;

	// 收尾。下面这几个只在通道那条事件循环线程上读写（_DeliverOnce 只可能从
	// OnChannelClosed 里被调到），所以不用加锁；跨线程可达的那两个是 atomic。
	BCRESULT							fail_result_ = BC_R_SUCCESS;
	std::string							fail_message_;
	bool								delivered_ = false;
	// OnConnectResult 已经发过
	bool								connect_notified_ = false;
	// 握手成功过 —— 决定收尾时该发 OnClosed 还是 OnConnectResult(失败)
	bool								connected_ok_ = false;
	bool								peer_closed_frame_ = false;
	uint16_t							close_code_ = 0;
	// 业务调过 Close()。跨线程，故 atomic。
	std::atomic_bool					user_closed_;
	std::atomic<BCRESULT>				user_close_result_;
	// close 帧只发一次（对端发起关闭握手与业务 Close() 可以并发）
	std::atomic_bool					close_frame_sent_;
	// 通道销毁前拷下来的诊断信息
	std::string							peer_ip_;
	uint32_t							bound_ifindex_ = 0;
	std::string							pin_method_;
	// 收尾时的失败现场描述。由 state_->lock 保护（跨线程读，见
	// LastErrorMessage()）—— fail_message_ 那份只在通道线程上读写，不能直接暴露。
	std::string							last_error_message_;

	// 定时器。都挂在通道自己的事件队列上，回调线程与 OnChannelData /
	// OnChannelClosed 完全一致，因此不需要额外加锁。
	int32_t								hs_timer_ = 0;
	int32_t								check_timer_ = 0;
	uint64_t							start_ms_ = 0;
	uint64_t							latest_net_action_ms_ = 0;
	// 最近一次 ping 的发出时刻。last_ping_ms_ 用来算 RTT（收到 pong 就清零），
	// last_ping_sent_ms_ 只用来控制发送间隔。
	uint64_t							last_ping_ms_ = 0;
	uint64_t							last_ping_sent_ms_ = 0;

	// stats
	std::atomic<uint64_t>				total_recv_bytes_;
	std::atomic<uint64_t>				total_send_bytes_;
	std::atomic<uint64_t>				total_recv_frames_;
	std::atomic<uint64_t>				total_send_frames_;

	// dump file
	std::mutex							dump_lock_;
	std::unique_ptr<BCFOStream>			dump_file_;
};

///////////////////////////////////////////////////////////////////////////////
// class : WSConnector
///////////////////////////////////////////////////////////////////////////////

class WSConnector 
{
	friend class WSConnection;

public:
    ///////////////////////////////////////////////////////////////////////////
    // class : Config
    ///////////////////////////////////////////////////////////////////////////

    class Config
    {
    public:
        Config() {}
        ~Config() {}

        BCRESULT		Init(BCFObject* pConfig);

		// 析构时等待在途连接收尾的上限。0 是合法值（不等，掐断所有回调后立即
		// 返回），上限 5 分钟，越界会夹住并打 _WARN_。
		uint32_t				drain_timeout_ms	= 30000;
		uint64_t				bad_drain_timeout	= 0;	// 越界原值，供告警
		std::string				log_file;
		int32_t					log_level			= _INFO_;
		// 每条连接的默认配置，CreateConnection 的 pConfig 可逐键覆盖
		WSConnection::Config	conn;
		// vpnPolicy 解析过程中的诊断（Create 里打日志用）
		std::string				bad_policy_text;
		bool					both_policy_given	= false;
    };

public:
	WSConnector();
	virtual ~WSConnector();

	// config 键（全部可选，pConfig 可传 NULL）：
	//   vpnPolicy          string  "os" / "prefer-physical" / "force-physical"
	//   bypassVpn          bool    已废弃；与 vpnPolicy 同时给出时以后者为准
	//   dnsServers         array|string  自建 DNS 查询用的服务器列表
	//   dnsTimeoutMs       int     单台 DNS server 的超时，默认 1000
	//   caCerts            string  PEM 文本。空则回落系统信任库
	//   spkiPin            string  base64 SPKI pin，给了就只比对 pin
	//   insecureSkipVerify bool    仅供自签调试，会打 _WARN_
	//   clientCertFile     string  双向 TLS 用的客户端证书（原 certificate_file）
	//   clientKeyFile      string  （原 private_key_file）
	//   clientKeyPassword  string  （原 private_key_password）
	//   connectTimeoutMs   int     覆盖 DNS + connect + TLS + WS 握手，默认 10000
	//   pingIntervalMs     int     主动 ping 间隔，0 = 不发（默认）
	//   idleTimeoutMs      int     空闲多久关连接，0 = 不判（默认）
	//   maxFrameBytes      int     收包侧单帧/单消息上限，默认 8 MiB
	//   ifIndex            int     显式指定绑哪块网卡
	//   androidNetHandle   int     Android 的 network handle
	//   drainTimeoutMs     int     析构时排空在途连接的上限，默认 30000
	//   logLevel           int     BC 日志级别（1=error 2=warn 3=info 4=debug）
	//   logFile            string  给了就落文件，否则日志走 handler->OnLog
	BCRESULT				Create(
								BCFObject* pConfig,
								IWSConnectorHandler *pHandler);
	// pConfig 可为 NULL（沿用连接器的默认配置），也可以逐键覆盖上面那些
	// 与单条连接有关的键。失败返回空指针。
	WSConnPtr				CreateConnection(
								BCFObject *pConfig,
								IWSConnectionHandler* pHandler);
	void				*	GetLoggerCtx();
	// ⚠️ 只能在初始化阶段调用（Create 之后、第一次 CreateConnection 之前），
	// 而且不能与其它线程并发 —— 它内部要拿 BCLogger 的全局自旋锁，本模块刻意
	// 不给它套自己的锁（否则就构成锁序反转，见文件顶部）。
	// Create 时给了 logFile 的话这里返回 BC_R_EXISTS。
	BCRESULT				OpenLogFile(LPCSTR lpszLogFilePath);
	// 幂等。关掉所有在途连接；全部收尾后回调 IWSConnectorHandler::OnClosed。
	void					Close();
	BCRESULT				GetStats(ConnStatsMap& stats);

	// Create() 解析出来的最终配置，给诊断与单测看的只读视图
	TTVpnPolicy				EffectivePolicy() const { return config_.conn.policy; }
	uint32_t				DrainTimeoutMs()  const { return config_.drain_timeout_ms; }
	const DnsConfig		&	DnsSettings()     const { return config_.conn.dns; }
	const TlsConfig		&	TlsSettings()     const { return config_.conn.tls_cfg; }

private:
	DECLARE_NO_COPY_CLASS(WSConnector);

	static void				_LogCallback(void* data, int level, LPCSTR msg);

private:
	// 共享状态。连接器只是它的持有者之一，见 WSConnector.cpp 顶部注释。
	std::shared_ptr<WSConnectorState>	state_;
    Config								config_;
	void							*	logger_ctx_ = NULL;	// state_->logger_ctx 的副本
	uint64_t							next_conn_id_ = 0;
	bool								created_ = false;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : WS
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : WS

#endif // WSCONNECTOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : WSConnector.h
///////////////////////////////////////////////////////////////////////////////
