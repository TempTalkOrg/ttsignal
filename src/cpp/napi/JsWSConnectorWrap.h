///////////////////////////////////////////////////////////////////////////////
// file : JsWSConnectorWrap.h
// author : anto.
//
// WSConnector（WebSocket 客户端）的 Node.js 绑定。导出的是**原始接口**（带双
// 下划线），供 src/js/index.js 的胶水层包成 EventEmitter + Promise 形态：
//
//   const ws   = ttsignal.__createWsConnector__({ vpnPolicy: 'force-physical' });
//   const conn = ws.__createConnection__({ pingIntervalMs: 15000 });
//   conn.__connect__('wss://sfu.example.com/signal', 5000, (err, ok) => { ... });
//   conn.__sendText__('hello');            // 返回 BCRESULT，0 = 成功
//   conn.__sendData__(buf);
//   conn.__sendPing__();
//   conn.__close__();
//   conn.__info__();                        // 单条连接的只读快照
//   ws.__close__([onClosed]);
//   ws.__info__();  ws.__stats__();
//
// ---------------------------------------------------------------------------
// 事件（连接级）
// ---------------------------------------------------------------------------
// 除 connectResult 之外的连接事件都打在 JS 原型上的 `_internalCallback(type, ...)`
// 上，与 JsSMPConnectionWrap 完全一致，胶手层照旧在那里 emit：
//
//   ('text',      string)      收到文本帧
//   ('data',      Buffer)      收到二进制帧
//   ('closed',    string)      连接关闭（**只在握手成功过的连接上发**）
//   ('exception', string)      帧解析违规（随后一定会走 closed）
//
// 连接器级：('close') —— 所有在途连接收尾完毕；('exception', string)。
//
// ⚠️ **connectResult 刻意不走 `_internalCallback`**，只走 `__connect__` 的那个
// 回调：它是"恰好一次"的语义，包成 Promise 才是自然形态，而且回调是随调用一起
// 传进来的，不依赖胶水层有没有在原型上装 `_internalCallback`（漏装的话事件会
// 静默丢失，而连接失败是最不能静默的一件事）。胶水层在那个回调里
// emit('connectResult', err, info) 即可。
//
// ⚠️ 握手失败与"连上之后被关闭"在 JS 侧同样是可区分的，这是 WSConnector.h 契约
// 第 2 条要求的语义：握手失败**只有** connectResult(err)，**没有** closed；
// 握手成功过的连接一定先 connectResult(null) 再 closed。绑定层原样保持。
//
// ---------------------------------------------------------------------------
// 诊断字段
// ---------------------------------------------------------------------------
// __connect__ 成功回调的第二个参数带 headers / peerIp / boundIfIndex /
// pinMethod，`conn.__info__()` 里也有同名字段。业务据此判断这条连接有没有真的
// 走物理网卡（boundIfIndex 非 0 即绑上了，pinMethod 说明用的哪种绑法），
// 不用翻日志。
//
// `ws.__info__()` 的形状是**恒定**的（关闭之后字段也都在，只是 closed = true），
// 其中 hasPathMonitor 报的是编译期有没有 TT_HAS_PATH_MONITOR：没有它
// prefer-physical 会静默回落系统路由（force-physical 则硬失败 64），所以把它
// 暴露出来，让"构建配置悄悄把网卡绑定关掉"这类缺陷在任何跑得起来的产物上都
// 当场露馅 —— 包括手搓编译命令编出来的东西，CMake 断言管不到那些。
//
// 日志：config.log_callback 收 (level, msg)。⚠️ 与 config.logFile **互斥且
// logFile 优先**（WSConnector::Create 里给了 logFile 就只装文件 appender），
// 两个都给时 log_callback 一条都不会触发。
// ⚠️ 不会串台到 QUIC/SMP：WSConnector.cpp 注册 appender 时 bExclusive = true，
// 而 BCLogger::Log 的语义是"logger_ctx 非空就只投那一个 appender，ctx 为空的
// 广播路径跳过 exclusive 的"。两个方向都不串。
//
// err 是一个 Error，额外挂了三个字段：
//   err.result      数字错误码，**原样透传** BCRESULT，不做归并
//   err.errName     错误码的符号名，如 "BC_R_WS_HANDSHAKE_FAILED"
//   err.errMessage  原生层给的诊断文案（force-physical 失败时写得很细）
// 常用取值：64 = BC_R_NO_PHYSICAL_INTERFACE（跨语言契约值，见
// deps/env/src/BC/Config.h:388），69 = BC_R_PIN_FAILED，
// 70 = BC_R_ROUTE_MISMATCH，71 = BC_R_DNS_FAILED，
// 72 = BC_R_TLS_VERIFY_FAILED，74 = BC_R_WS_HANDSHAKE_FAILED。
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
// IWSConnectionHandler 的五个回调都发生在该连接 TcpChannel 的事件循环线程上，
// IWSConnectorHandler::OnLog 更是被 BCLogger 持着**全局非递归自旋锁**调进来的。
// V8 只能在 Node 主线程上碰，所以这里**一律**：
//
//     C++ 回调线程：把数据拷进堆上的 payload -> JsExchanger 投递 -> 立刻返回
//     Node 主线程 ：uv_async 回调里取出 payload -> 调 JS -> 释放
//
// 照 JsSMPConnectorWrap / JsHttpConnectorWrap 的既有做法，不发明新机制。这不是
// 风格问题：
//   * WSConnector.h 契约第 8(5) 条要求回调里不做耗时操作 —— 在回调线程上调 V8
//     做不到，还会阻住那条连接的收尾；
//   * OnLog 里回调任何会打日志的 API 会重进 BCLogger 那把非递归自旋锁，100% CPU
//     空转挂死（WSConnector 为此修了三轮）。只投递、不回调，才躲得过。
//
// ---------------------------------------------------------------------------
// 生命周期 —— 本文件唯一真正需要判断的地方
// ---------------------------------------------------------------------------
// HTTP 侧是"有界的 request/response"：在途期间 Ref()，一结束 Unref()。WS 是
// **长连接**，配平模型不同，这里逐条写清楚。
//
// 1) handler 不是 wrap 本身，而是单独的 JsWSConnSink
//    ObjectWrap 的 finalize 时机由 GC / env 拆除决定，而 WSConnection **没有
//    "同步摘掉 handler"的公开接口**（ClearHandlerLocked 是 private，只有
//    ~WSConnector 的放弃路径用）。若让 JsWSConnectionWrap 直接充当 handler，
//    它一被回收，原生层手里那个 handler_ 指针就是野的。所以每条连接配一个
//    JsWSConnSink：它由**连接器 wrap** 拥有，只在 `delete WSConnector` 返回
//    之后才释放 —— 那一刻按 WSConnector.h 契约第 5 / 7 条，所有在途回调都已
//    跑完、所有 handler_ 都已被清空，再也不可能有回调进来。
//
// 2) 连接 JS 对象在"连接活着"的整段时间里被 Ref() 钉住
//    Connect() 返回 BC_R_SUCCESS 起钉住，直到**终局事件**在 Node 主线程上处理
//    完才 Unref()：
//      * connectResult(err)      —— 握手失败即终局（契约保证不再有 OnClosed）
//      * closed                  —— 握手成功过的连接的终局
//    也就是说：**只要 socket 还在，连接 JS 对象就不会被 GC，即使业务把变量丢了**。
//    这正是长连接该有的语义 —— EventEmitter 形态的连接因为"用户的变量出了作用域"
//    而悄悄不再 emit，是个很难查的坑。钉住是有终点的（closed 一定会来：业务
//    __close__、对端关闭、空闲超时、连接器 Close() 都会推到终局），所以这不是
//    JsSMPConnectorWrap 那种 m_hSelf 永久自引用（那个是"引用不放 -> 析构不跑 ->
//    引用不放"的环，SMP 的连接器实际上永久泄漏）。
//    唯一一处钉住可能不自然平衡的是 ~WSConnector 的**放弃路径**
//    （drainTimeoutMs 超时后掐断全部回调，closed 永远不来）；那条路上由
//    JsWSConnectorWrap 兜底：连接器一销毁就把所有还登记着的连接强行解钉。
//
// 3) 连接 wrap 持有连接器 JS 对象的强引用（m_hConnector）
//    否则业务写 `const conn = ws.__createConnection__()` 之后不再引用 ws，
//    连接器 wrap 会被 GC，它的析构又会 delete WSConnector -> 把这条正连着的
//    连接关掉。有了这条强引用，"连接器活得比它的连接久"就成了硬保证，
//    ~JsWSConnectorWrap 里"还有连接 wrap 活着"也就只可能发生在 env 拆除阶段
//    （那时引用计数已经无意义）。方向是单向的（连接 -> 连接器），不成环。
//
// 4) 事件路由集中在连接器 wrap
//    JsWSConnectorWrap 是唯一的 IExchangeHandler：所有 sink 都把事件投给它，
//    它按 connId 找到对应的连接 wrap 再调 JS。好处是 RemoveEventByHandler 只有
//    一处、且连接 wrap 被回收之后自然丢弃它的残留事件（查不到 id 就丢）。
//
// 5) Runtime 必须活得比 WSConnector 久（契约第 6 条）。NAPI 模块从不调
//    Runtime::Destroy()，Create() 里只负责保证它已经初始化。
///////////////////////////////////////////////////////////////////////////////

#ifndef JSWSCONNECTORWRAP_H_INCLUDED__
#define JSWSCONNECTORWRAP_H_INCLUDED__

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <napi.h>
#include <BC/Exchanger.h>

#include "macros.h"
#include "Utils.h"
#include "../WSConnector.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

class JsWSConnectorWrap;
class JsWSConnectionWrap;

///////////////////////////////////////////////////////////////////////////////
// enum : WSJsEvent —— payload 里的连接事件类型
///////////////////////////////////////////////////////////////////////////////

enum WSJsEvent
{
	WSJS_CONNECT_RESULT	= 1,
	WSJS_TEXT			= 2,
	WSJS_DATA			= 3,
	WSJS_CLOSED			= 4,
	WSJS_EXCEPTION		= 5,
};

///////////////////////////////////////////////////////////////////////////////
// struct : WSEventPayload —— 跨线程搬运一个连接事件
//
// 在 TcpChannel 线程上 new 出来，随事件投递到 Node 主线程。**所有权唯一归事件
// 的 cbDestroy**（JsWSConnectorWrap::_EventDtorCB）：无论事件是被正常处理完还是
// 被 RemoveEventByHandler 丢弃，payload 都在那里释放一次，JS 侧的处理函数不碰
// 它的生命期，省掉一整类 double-free。
///////////////////////////////////////////////////////////////////////////////

struct WSEventPayload
{
	uint64_t		nConnId			= 0;
	uint32_t		eEvent			= 0;		// WSJsEvent

	// WSJS_CONNECT_RESULT
	BCRESULT		result			= 0;
	HttpHeaderMap	mapHeaders;
	std::string		strPeerIp;
	uint32_t		nBoundIfIndex	= 0;
	std::string		strPinMethod;

	// WSJS_TEXT / WSJS_CLOSED / WSJS_EXCEPTION 的文案，WSJS_DATA 的原始字节。
	// ⚠️ 用 std::string 装二进制是刻意的：它能装任意字节（含 NUL），长度靠
	// size() 而不是结束符。
	std::string		strBody;

	// 连接失败时的诊断文案。原生层没给的话为空，_MakeError 会退到 bc_result2string
	std::string		strErrMessage;
};

///////////////////////////////////////////////////////////////////////////////
// class : JsWSConnSink —— 一条连接的 handler 承接体
//
// 为什么不让 JsWSConnectionWrap 直接实现 IWSConnectionHandler：见文件头
// "生命周期" 第 1 条。本对象由 JsWSConnectorWrap 拥有，**只有在
// `delete WSConnector` 返回之后**才会被释放。
//
// 五个回调都跑在 TcpChannel 线程上，只做"拷数据 + 投递事件"，一行 V8 都不碰，
// 也绝不回头调 ttsignal 任何会打日志的 API。
///////////////////////////////////////////////////////////////////////////////

class JsWSConnSink : public IWSConnectionHandler
{
public:
	JsWSConnSink(JsWSConnectorWrap *pOwner, uint64_t nConnId);
	virtual ~JsWSConnSink();

	// 由 ~JsWSConnectorWrap 在 `delete WSConnector` **之后**调用：此时已经不可能
	// 再有回调进来，置空只是把"事件还能投给谁"这件事写死，顺带让残留路径显式化。
	void			Detach();

	// 诊断信息要在**通道还活着**的时候抓（通道一销毁 PeerIp() 就只能读缓存），
	// 所以 sink 留一份弱引用，OnConnectResult 里 lock() 出来读。
	// 强引用在 JsWSConnectionWrap 那边（以及 WSConnector 自己的 conns 里）。
	std::weak_ptr<WS::WSConnection>	m_wpConn;

private:
	DECLARE_NO_COPY_CLASS(JsWSConnSink);

	// Override IWSConnectionHandler interfaces（TcpChannel 线程）
	void			OnConnectResult(BCRESULT result,
									const HttpHeaderMap &refHeaders) override;
	void			OnRecvText(LPCSTR lpszText) override;
	void			OnRecvData(LPCVOID data, size_t size) override;
	void			OnClosed(LPCSTR lpszReason) override;
	void			OnException(BCException &refExcept) override;

	// 填好 nConnId 之后投递。pPayload 的所有权一律交出去（投不出去也会被释放）。
	void			_Post(WSEventPayload *pPayload);

	// 保护 m_pOwner。⚠️ 加锁顺序恒为 sink->lock -> 交换器锁；Node 主线程从不在
	// 持交换器锁的情况下拿这把锁（Detach 发生在 delete WSConnector 之后、
	// RemoveEventByHandler 之前），所以不存在锁序反转。
	std::mutex			m_lock;
	JsWSConnectorWrap *	m_pOwner;
	const uint64_t		m_nConnId;
};

///////////////////////////////////////////////////////////////////////////////
// class : JsWSConnectionWrap —— 一条 WebSocket 连接的 JS 对象
//
// 只在 Node 主线程上被访问。原生回调经 sink -> 连接器 wrap -> 本对象。
///////////////////////////////////////////////////////////////////////////////

class JsWSConnectionWrap : public Napi::ObjectWrap<JsWSConnectionWrap>
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsWSConnectionWrap(const Napi::CallbackInfo &info);
	virtual ~JsWSConnectionWrap();

	static void				Initialize(Napi::Env env, Napi::Object exports);
	static inline bool		HasInstance(Napi::Env env, Napi::Value val) {
		Napi::HandleScope scope(env);
		if (!val.IsObject()) return false;
		Napi::Object obj = val.As<Napi::Object>();
		return obj.InstanceOf(constructor_template.Value());
	}
	// 用 node:: 限定而不是 ::（JsSMPConnectorWrap.h 里那种写法要靠调用方 .cpp
	// 顶部的 `using namespace node;` 才能在全局命名空间里找到符号，换个不带该
	// using 的 TU 引本头文件就会编不过）。
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(
			node::GetPrototypeProperty(m_env, Value(), propertyName));
	}

	// 由 JsWSConnectorWrap::_CreateConnection 在建好原生连接之后调用。
	// hConnector 是连接器的 JS 对象，本对象持它的强引用（见文件头第 3 条）。
	void					Attach(JsWSConnectorWrap *pOwner,
								   uint64_t nConnId,
								   const WSConnPtr &refConn,
								   Napi::Object hConnector);
	// 连接器 wrap 先析构（只可能发生在 env 拆除阶段）。清掉回指指针与回调、
	// 解钉、放手 WSConnPtr，之后本对象的所有 __xxx__ 方法都只会返回错误码。
	void					OnOwnerGone();
	// 这次 connect 的结果投不出去了（只可能发生在模块拆除阶段）：丢回调 + 解钉。
	// ⚠️ 与 OnOwnerGone 的区别是**不动 m_pOwner** —— 连接器还活着，回指指针一置空
	// 本对象析构时就不去销账，连接器的路由表里会留下野指针。
	void					AbandonPendingConnect();
	// 由连接器 wrap 在 `delete WSConnector` 之后调用：放手 WSConnPtr。原生
	// WSConnection 的 handler_ 此刻已成野指针，留着这个强引用没有任何用处。
	void					DetachNativeConn();
	uint64_t				ConnId() const { return m_nConnId; }

	// Node 主线程侧的事件处理。返回值无意义，纯为可读性。
	void					OnJsEvent(const WSEventPayload &refPayload);

protected:
	Napi::Value			_Connect(const Napi::CallbackInfo &info);
	Napi::Value			_SendText(const Napi::CallbackInfo &info);
	Napi::Value			_SendData(const Napi::CallbackInfo &info);
	Napi::Value			_SendPing(const Napi::CallbackInfo &info);
	Napi::Value			_Close(const Napi::CallbackInfo &info);
	Napi::Value			_Info(const Napi::CallbackInfo &info);

private:
	DECLARE_NO_COPY_CLASS(JsWSConnectionWrap);

	// 每次调完 JS 之后都要调 node::DrainPendingException(m_env)（声明在
	// napi/Utils.h）—— 不清 pending exception 会把整个 node 进程打死。
	void				_Pin();
	void				_Unpin();
	void				_SettleConnect(const WSEventPayload &refPayload);
	void				_EmitInternal(LPCSTR lpszType, Napi::Value hArg);

	Napi::Env				m_env;
	// 原型上的 _internalCallback。胶水层在那里 emit；没装就等于不订阅那几个事件。
	Napi::FunctionReference	m_hCallback;
	// __connect__ 传进来的回调，恰好触发一次
	Napi::FunctionReference	m_hConnectCallback;
	// 连接器的 JS 对象。强引用，见文件头第 3 条。
	Napi::ObjectReference	m_hConnector;

	JsWSConnectorWrap	*	m_pOwner;		// NULL = 连接器 wrap 已析构
	WSConnPtr				m_pConn;
	uint64_t				m_nConnId;

	// Connect() 已经返回过 BC_R_SUCCESS（在途或已完成），用来挡住第二次
	// __connect__ 把前一个回调覆盖掉
	bool					m_bConnectCalled;
	bool					m_bConnectSettled;
	bool					m_bUpgraded;	// 握手成功过 —— 决定还会不会来 closed
	bool					m_bClosedSeen;
	// Ref() 是否还欠一次 Unref()
	bool					m_bPinned;

	// 连上（或连接失败）时抓下来的诊断快照 —— 由 sink 在 OnConnectResult 里趁通道
	// 还活着取的，见 JsWSConnSink::OnConnectResult。
	//
	// ⚠️ 两个用途，别只看第一个：
	//   1) __info__() 的形状必须**恒定**（少字段会让读到 undefined 的人误以为
	//      "策略没生效"）；
	//   2) 连接器销毁之后 m_pConn 会被 DetachNativeConn() 放手，那之后 __info__()
	//      报的就是这份快照 —— 也就是"连上那一刻"的实况，而不是空值。
	//      在 m_pConn 还在的时候优先读原生实时值（它内部也会回落到 WSConnection
	//      自己在 OnChannelClosed 里缓存的那份）。
	std::string				m_strPeerIp;
	uint32_t				m_nBoundIfIndex;
	std::string				m_strPinMethod;
	// 原生给的失败现场描述（WSConnection::LastErrorMessage）。__info__().lastError
	// 报它 —— connectResult 的 err 只在那一次回调里出现，事后想复查就没地方看了。
	std::string				m_strLastError;
};

///////////////////////////////////////////////////////////////////////////////
// Class : JsWSConnectorWrap
///////////////////////////////////////////////////////////////////////////////

class JsWSConnectorWrap
	: public Napi::ObjectWrap<JsWSConnectorWrap>
	, public IExchangeHandler
	, public IWSConnectorHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsWSConnectorWrap(const Napi::CallbackInfo &info);
	virtual ~JsWSConnectorWrap();

	static void				Initialize(Napi::Env env, Napi::Object exports);
	static inline bool		HasInstance(Napi::Env env, Napi::Value val) {
		Napi::HandleScope scope(env);
		if (!val.IsObject()) return false;
		Napi::Object obj = val.As<Napi::Object>();
		return obj.InstanceOf(constructor_template.Value());
	}
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(
			node::GetPrototypeProperty(m_env, Value(), propertyName));
	}
	BCRESULT				Create(Napi::Object config);

	// 供 JsWSConnSink 在**任意线程**调用。返回 false 表示事件没能进队（交换器
	// 已经不在了），此时 pPayload 已被本函数释放。
	bool					PostEventPayload(WSEventPayload *pPayload);
	// 连接 wrap 析构时销账（Node 主线程）。sink 不在这里删 —— 原生层可能还握着它。
	void					OnConnectionWrapGone(uint64_t nConnId);
	// 把一次连接失败**异步**投回 JS（原生层同步失败也走这里，保证"回调永远不会
	// 在 __connect__ 的栈里同步触发"这条语义始终成立）。
	void					PostConnectFailure(uint64_t nConnId, BCRESULT result,
											   const std::string &refMessage);

protected:
	Napi::Value			_CreateConnection(const Napi::CallbackInfo &info);
	Napi::Value			_Close(const Napi::CallbackInfo &info);
	Napi::Value			_Info(const Napi::CallbackInfo &info);
	Napi::Value			_Stats(const Napi::CallbackInfo &info);

private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);
	// 参数校验，全部在 Node 主线程上跑。返回 false 时 outError 里是给 JS 抛出去
	// 的文案。
	bool				_ValidateConfig(Napi::Object config,
										std::string &outError);
	// 连接器已经收尾：释放所有 sink，并让还登记着的连接 wrap 放手它们的 WSConnPtr。
	// ⚠️ 只能在 `delete m_pConnector` 之后调。
	void				_ReleaseNativeRefs();

	// Node 主线程侧的事件处理
	void				OnJsConnEvent(WSEventPayload *pPayload);
	void				OnJsLog(int level, LPCSTR lpszMsg);
	void				OnJsClosed();
	void				OnJsException(LPCSTR lpszMsg);

private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	// Override IWSConnectorHandler interfaces（任意线程）
	void				OnLog(int level, LPCSTR lpszMsg) override;
	void				OnClosed() override;
	void				OnException(BCException &refExcept) override;

private:
	DECLARE_NO_COPY_CLASS(JsWSConnectorWrap);
	typedef std::map<uint64_t, JsWSConnSink *>			SinkMap;
	typedef std::map<uint64_t, JsWSConnectionWrap *>		ConnWrapMap;

	Napi::FunctionReference	m_hCallback;		// 原型上的 _internalCallback
	Napi::FunctionReference	m_hLogCallback;		// config.log_callback
	// __close__(cb) 传进来的回调。**必须是列表**：close() 幂等，但每一次调用都
	// 带着自己的回调，用单个引用去 Reset 会把前一个静默丢掉 —— 胶水层把 close
	// 包成 Promise 之后，那就是"第一个 Promise 永远悬着"。
	std::vector<Napi::FunctionReference>	m_vecCloseCallbacks;
	Napi::Env				m_env;
	WS::WSConnector		*	m_pConnector;

	// Create() 时从连接器抄下来的生效配置。留一份是为了让 __info__() 在连接器
	// 被销毁之后仍能报出**恒定形状**。这几项在 Create 之后不会再变。
	TTVpnPolicy				m_ePolicy;
	uint32_t				m_nDrainTimeoutMs;
	// __stats__() 的最后一次快照。连接器销毁之后仍然报得出来 —— 关闭之后正是最
	// 想看统计的时候（握手失败了几条、force-physical 的路由复核挡了几条）。
	ConnStatsMap			m_mapLastStats;

	// 只在 Node 主线程上访问，因此不需要锁。
	SinkMap					m_mapSinks;		// 原生 handler，连接器销毁后才释放
	ConnWrapMap				m_mapConnWraps;	// 事件路由表，连接 wrap 析构时摘除
	uint64_t				m_nNextConnId;

	// 是否要把库日志转发给 JS。**必须是原子的**：OnLog 在别的线程上读它，而它在
	// Create()/析构 里由 Node 主线程写。没配 log_callback 时在这里直接挡掉，省掉
	// 一次字符串拷贝 + 一次事件投递。
	std::atomic<bool>		m_bLogEnabled;
	// 已调过 __close__ 且还没收到 OnClosed。只用来挡住"重复调 WSConnector::Close()"。
	bool					m_bClosePending;
	// 还欠几次 Unref()。**不能用 bool** —— 连接器关完之后再调 __close__ 会各自
	// 补投一个 JWM_CLOSE，每一个都得有自己的那次 Ref()，否则调用方同 tick 丢掉
	// 最后一个引用时回调会被析构路径悄悄吞掉（Promise 永悬）。
	uint32_t				m_nPendingCloseRefs;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

#endif // JSWSCONNECTORWRAP_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : JsWSConnectorWrap.h
///////////////////////////////////////////////////////////////////////////////
