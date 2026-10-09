///////////////////////////////////////////////////////////////////////////////
// file : JsHttpConnectorWrap.h
// author : anto.
//
// HttpConnector 的 Node.js 绑定。导出的是**原始接口**（带双下划线），供
// src/js/index.js 的胶水层包成 Promise 形态：
//
//   const c = ttsignal.__createHttpConnector__({ vpnPolicy: 'force-physical' });
//   c.__request__({ method, url, headers, body, timeoutMs, resolvedIp },
//                 (err, resp) => { ... });
//   c.__close__([onClosed]);
//   c.__info__();          // 生效配置的只读快照，给诊断用
//
// resp 里除了 status / reason / headers / body，还带 peerIp / boundIfIndex /
// pinMethod —— 业务据此判断这次请求有没有真的走物理网卡，不用翻日志。
//
// __info__() 的形状是**恒定**的（关闭之后字段也都在，只是 closed = true），
// 其中 hasPathMonitor 报的是编译期有没有 TT_HAS_PATH_MONITOR：没有它
// prefer-physical 会静默回落系统路由（force-physical 则硬失败 64），所以把它
// 暴露出来，让"构建配置悄悄把网卡绑定关掉"这类缺陷在任何跑得起来的产物上
// 都当场露馅 —— 包括手搓编译命令编出来的东西，CMake 断言管不到那些。
//
// 日志：config.log_callback 收 (level, msg)。⚠️ 与 config.logFile **互斥且
// logFile 优先**（HttpConnector.cpp:1449-1453：给了 logFile 就只装文件
// appender），两个都给时 log_callback 一条都不会触发。
//
// err 是一个 Error，额外挂了三个字段：
//   err.result      数字错误码，**原样透传** BCRESULT，不做归并
//   err.errName     错误码的符号名，如 "BC_R_ROUTE_MISMATCH"
//   err.errMessage  原生层给的诊断文案（force-physical 失败时写得很细）
// 常用取值：64 = BC_R_NO_PHYSICAL_INTERFACE（跨语言契约值，见
// deps/env/src/BC/Config.h:388），69 = BC_R_PIN_FAILED，
// 70 = BC_R_ROUTE_MISMATCH，71 = BC_R_DNS_FAILED，
// 72 = BC_R_TLS_VERIFY_FAILED，73 = BC_R_RESPONSE_TOO_LARGE。
//
// ---------------------------------------------------------------------------
// 线程模型（这是本文件唯一真正要小心的地方）
// ---------------------------------------------------------------------------
// HttpConnector 的三个回调（OnHttpResponse / OnHttpError / OnClosed）发生在
// TcpChannel 的事件循环线程上，OnLog 更是被 BCLogger 持着**全局非递归自旋锁**
// 调进来的。V8 只能在 Node 主线程上碰，所以这里**一律**：
//
//     C++ 回调线程：把数据拷进堆上的 payload -> JsExchanger 投递 -> 立刻返回
//     Node 主线程 ：uv_async 回调里取出 payload -> 调 JS -> 释放
//
// 照 JsSMPConnectorWrap 的既有做法，不发明新机制。这不是风格问题：
//   * HttpConnector.h 契约第 7(4) 条要求回调里不做耗时操作 —— 直接在回调线程
//     上调 V8 是不可能的，做不到还会阻住那条连接的收尾；
//   * OnLog 里回调任何会打日志的 API 会重进 BCLogger 那把非递归自旋锁，
//     100% CPU 空转挂死。只投递、不回调，才躲得过。
//
// ---------------------------------------------------------------------------
// 生命周期（对应 HttpConnector.h 的 7 条契约）
// ---------------------------------------------------------------------------
// * 析构（GC finalize 或 env 拆除）跑在 Node 主线程上，**不持任何业务锁**，
//   也不在任何 HTTP 回调栈里（回调只投递事件就返回了，真正跑 JS 的代码在
//   uv_async 回调里）。所以契约 5 / 7(2) / 7(3) 都自然满足。
// * 析构里先 Close() 再 delete，把"等在途请求收尾"这段等待压到最短。
// * 在途请求期间用 ObjectWrap::Ref() 把 JS 对象钉住，请求一结束就 Unref()。
//   这样既保证"回调到达前对象不会被 GC 掉"，又不像 JsSMPConnectorWrap 的
//   m_hSelf 那样永久自引用（那会让没显式 close 的连接器永远不被回收）。
// * Runtime 必须活得比 HttpConnector 久（契约第 5 条）。NAPI 模块从不调
//   Runtime::Destroy()，Create() 里只负责保证它已经初始化。
///////////////////////////////////////////////////////////////////////////////

#ifndef JSHTTPCONNECTORWRAP_H_INCLUDED__
#define JSHTTPCONNECTORWRAP_H_INCLUDED__

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <napi.h>
#include <BC/Exchanger.h>

#include "macros.h"
#include "Utils.h"
#include "../HttpConnector.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

class JsHttpConnectorWrap;

///////////////////////////////////////////////////////////////////////////////
// struct : HttpResultPayload —— 跨线程搬运一次请求的最终结果
//
// 在 TcpChannel 线程上 new 出来，随事件投递到 Node 主线程。**所有权唯一归事件
// 的 cbDestroy**（JsHttpConnectorWrap::_EventDtorCB）：无论事件是被正常处理完
// 还是被 RemoveEventByHandler 丢弃，payload 都在那里释放一次，JS 侧的处理函数
// 不碰它的生命期，省掉一整类 double-free。
///////////////////////////////////////////////////////////////////////////////

struct HttpResultPayload
{
	uint32_t		nId				= 0;		// 对应的任务 id
	bool			bOk				= false;	// true = 有响应，false = 出错

	// bOk = false 时有效
	BCRESULT		result			= 0;
	std::string		strErrMessage;

	// bOk = true 时有效
	int				nStatus			= 0;
	// ⚠️ 原始网络字节，未净化：可能含控制字符与内嵌 NUL（见 HttpConnector.h
	// 里 HttpResponse::reason 的注释）。别直接打进终端。
	std::string		strReason;
	HttpHeaderMap	mapHeaders;
	std::string		strBody;
	std::string		strPeerIp;
	uint32_t		nBoundIfIndex	= 0;
	std::string		strPinMethod;
};

///////////////////////////////////////////////////////////////////////////////
// class : JsHttpRequestTask —— 一次请求一个，持有 JS 回调
//
// 由 Node 主线程创建与销毁；两个回调在 TcpChannel 线程上被调用，那里只做
// "拷数据 + 投递事件"。
///////////////////////////////////////////////////////////////////////////////

class JsHttpRequestTask : public IHttpRequestHandler
{
public:
	JsHttpRequestTask(JsHttpConnectorWrap *pOwner, uint32_t nId);
	virtual ~JsHttpRequestTask();

	// Override IHttpRequestHandler interfaces（TcpChannel 线程）
	void				OnHttpResponse(const HttpResponse &refResp) override;
	void				OnHttpError(BCRESULT result,
									const std::string &refMessage) override;

	// 只在 Node 主线程上 Reset / 取值
	Napi::FunctionReference	m_hCallback;
	uint32_t				m_nId;
private:
	DECLARE_NO_COPY_CLASS(JsHttpRequestTask);
	JsHttpConnectorWrap	*	m_pOwner;
};

///////////////////////////////////////////////////////////////////////////////
// Class : JsHttpConnectorWrap
///////////////////////////////////////////////////////////////////////////////

class JsHttpConnectorWrap
	: public Napi::ObjectWrap<JsHttpConnectorWrap>
	, public IExchangeHandler
	, public IHttpConnectorHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsHttpConnectorWrap(const Napi::CallbackInfo &info);
	virtual ~JsHttpConnectorWrap();

	static void				Initialize(Napi::Env env, Napi::Object exports);
	static inline bool		HasInstance(Napi::Env env, Napi::Value val) {
		Napi::HandleScope scope(env);
		if (!val.IsObject()) return false;
		Napi::Object obj = val.As<Napi::Object>();
		return obj.InstanceOf(constructor_template.Value());
	}
	// 用 node:: 限定而不是 ::（JsSMPConnectorWrap.h 里那种写法要靠调用方
	// .cpp 顶部的 `using namespace node;` 才能在全局命名空间里找到符号，
	// 换个不带该 using 的 TU 引本头文件就会编不过）。
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(
			node::GetPrototypeProperty(m_env, Value(), propertyName));
	}
	BCRESULT				Create(Napi::Object config);

	// 供 JsHttpRequestTask 在**任意线程**调用。返回 false 表示事件没能进队
	// （交换器已经不在了），此时 pPayload 已被本函数释放。
	bool					PostResult(HttpResultPayload *pPayload);
protected:
	Napi::Value			_Request(const Napi::CallbackInfo &info);
	Napi::Value			_Close(const Napi::CallbackInfo &info);
	Napi::Value			_Info(const Napi::CallbackInfo &info);
	Napi::Value			_SetIdleTimeout(const Napi::CallbackInfo &info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	// 每次调完 JS 之后都要调 node::DrainPendingException(m_env)（声明在
	// napi/Utils.h）—— 不清 pending exception 会把整个 node 进程打死
	// （100% 可复现，机制见 Utils.cpp 里那段注释）。

	// 参数校验 / 转换，全部在 Node 主线程上跑。返回 false 时 outError 里是
	// 给 JS 抛出去的文案。
	bool				_ValidateConfig(Napi::Object config,
										std::string &outError);
	bool				_ParseRequest(Napi::Object options,
									  HttpRequest &outRequest,
									  std::string &outError);

	// 把一次失败**异步**投回 JS。原生层同步失败也走这里，保证"回调永远不会
	// 在 __request__ 的栈里同步触发"这条语义始终成立。
	void				_PostFailure(uint32_t nId, BCRESULT result,
									 const std::string &refMessage);
	// 把还挂着的任务全部以错误结束（关闭路径的兜底）
	void				_SettlePendingTasks(BCRESULT result,
											const std::string &refMessage);

	Napi::Value			_MakeError(const HttpResultPayload &refPayload);
	Napi::Value			_MakeResponse(const HttpResultPayload &refPayload);

	// Node 主线程侧的事件处理
	void				OnJsResult(HttpResultPayload *pPayload);
	void				OnJsLog(int level, LPCSTR lpszMsg);
	void				OnJsClosed();
private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	// Override IHttpConnectorHandler interfaces（任意线程）
	void				OnLog(int level, LPCSTR lpszMsg) override;
	void				OnClosed() override;
private:
	DECLARE_NO_COPY_CLASS(JsHttpConnectorWrap);
	typedef std::map<uint32_t, JsHttpRequestTask *>	TaskMap;

	Napi::FunctionReference	m_hCallback;		// 原型上的 _internalCallback
	Napi::FunctionReference	m_hLogCallback;		// config.log_callback
	// __close__(cb) 传进来的回调。**必须是列表**：close() 幂等，但每一次调用
	// 都带着自己的回调，用单个引用去 Reset 会把前一个静默丢掉 —— Task 3 把
	// close 包成 Promise 之后，那就是"第一个 Promise 永远悬着"。
	std::vector<Napi::FunctionReference>	m_vecCloseCallbacks;
	Napi::Env				m_env;
	HttpConnector		*	m_pConnector;
	// Create() 时从连接器抄下来的生效配置。留一份是为了让 __info__() 在连接器
	// 被销毁之后仍能报出**恒定形状**（少字段会让读到 undefined 的人误以为
	// "策略没生效"）。这几项在 Create 之后不会再变。
	TTVpnPolicy				m_ePolicy;
	size_t					m_nMaxResponseBytes;
	uint32_t				m_nDrainTimeoutMs;
	// 连接池的两项。idleTimeoutMs 可在运行期改，所以它不像上面几项那样
	// "Create 之后就不会再变"——每次 _Info 都重新问连接器要（连接器已关闭时
	// 沿用最后一次缓存，保证 info 的形状恒定）。
	uint32_t				m_nMaxIdleConnections;
	uint32_t				m_nIdleTimeoutMs;
	// 在途请求。只在 Node 主线程上访问，因此不需要锁。
	TaskMap					m_mapTasks;
	uint32_t				m_nNextTaskId;
	// 是否要把库日志转发给 JS。**必须是原子的**：OnLog 在别的线程上读它，
	// 而它在 Create()/析构 里由 Node 主线程写。没配 log_callback 时在这里直接
	// 挡掉，省掉一次字符串拷贝 + 一次事件投递。
	//
	// 关于"会不会收到 QUIC/SMP 的日志"：**不会。**HttpConnector.cpp:1461-1463
	// 注册 appender 时 bExclusive = true，而 BCLogger::Log（BCLog.cpp:378-394）
	// 的语义是：logger_ctx 非空就只投那一个 appender；ctx 为空的广播路径会遍历
	// 全部 appender 但**跳过 exclusive 的**。所以两个方向都不串台 —— 实测
	// HTTP 的 log_callback 收不到 quic 日志，QUIC 的也收不到 HTTP 日志。
	std::atomic<bool>		m_bLogEnabled;
	// 已调过 __close__ 且还没收到 OnClosed。用来配平那一次 Ref()。
	bool					m_bClosePending;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

#endif // JSHTTPCONNECTORWRAP_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : JsHttpConnectorWrap.h
///////////////////////////////////////////////////////////////////////////////
