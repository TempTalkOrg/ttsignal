///////////////////////////////////////////////////////////////////////////////
// file : JsHttpConnectorWrap.cpp
// author : anto.
//
// 设计要点与线程模型见 JsHttpConnectorWrap.h 的文件头注释。
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "../Runtime.h"
#include "../TTErrors.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsHttpConnectorWrap.h"

#include <BC/Utils.h>

using namespace node;

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

enum
{
	JHM_RESULT		= 1,	// 一次请求的最终结果（wParam = HttpResultPayload*）
	JHM_LOG_MSG		= 2,	// 库日志
	JHM_CLOSE		= 3,	// 连接器收尾完毕
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

// 错误码符号名（_HttpResultName）与 _DrainPendingException 已移到
// napi/Utils.{h,cpp} 的 ResultName() / DrainPendingException()：WS 绑定也要用同一
// 份，各存一份会漂移出不一致的 errName。**纯移动，行为不变**；那两处的长注释
// （为什么不用 bc_result2string、不清 pending exception 会打死进程）一并搬过去了。

// HttpConnector::Request() 只返回错误码，具体原因在库日志里。没配 log_callback
// 的业务看不到那行日志，所以这里给几个高频码补一句人能看懂的话。
static LPCSTR _RequestFailureHint(BCRESULT result)
{
	switch (result)
	{
	case BC_R_NOTCONNECTED:
		return "HttpConnector 已经关闭，不能再发请求";
	case BC_R_SHUTTINGDOWN:
		return "HttpConnector 正在关闭，拒绝新请求";
	case BC_R_INVALIDARG:
		return "请求参数非法（URL 无法解析，或请求头里含 CR/LF）";
	case BC_R_NOTIMPLEMENTED:
		return "不能在 log_callback 里发请求（BC 日志器持着全局自旋锁，"
			   "重进会挂死）";
	default:
		return "发起请求失败";
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : JsHttpRequestTask
///////////////////////////////////////////////////////////////////////////////

JsHttpRequestTask::JsHttpRequestTask(JsHttpConnectorWrap *pOwner, uint32_t nId)
	: m_nId(nId)
	, m_pOwner(pOwner)
{
	//
}

JsHttpRequestTask::~JsHttpRequestTask()
{
	// m_hCallback 由 Node 主线程负责 Reset（析构本身也只发生在那条线程上）
}

void JsHttpRequestTask::OnHttpResponse(const HttpResponse &refResp)
{
	// TcpChannel 线程。只拷数据 + 投递，绝不碰 V8，也绝不回头调 ttsignal。
	HttpResultPayload *pPayload = new HttpResultPayload();

	pPayload->nId			= m_nId;
	pPayload->bOk			= true;
	pPayload->nStatus		= refResp.status;
	pPayload->strReason		= refResp.reason;
	pPayload->mapHeaders	= refResp.headers;
	pPayload->strBody		= refResp.body;
	pPayload->strPeerIp		= refResp.peerIp;
	pPayload->nBoundIfIndex	= refResp.boundIfIndex;
	pPayload->strPinMethod	= refResp.pinMethod;

	m_pOwner->PostResult(pPayload);
}

void JsHttpRequestTask::OnHttpError(BCRESULT result, const std::string &refMessage)
{
	// TcpChannel 线程。错误码**原样透传**，不归并成通用错误 ——
	// force-physical 的"失败可诊断"就靠它。
	HttpResultPayload *pPayload = new HttpResultPayload();

	pPayload->nId			= m_nId;
	pPayload->bOk			= false;
	pPayload->result		= result;
	pPayload->strErrMessage	= refMessage;

	m_pOwner->PostResult(pPayload);
}

///////////////////////////////////////////////////////////////////////////////
// class : JsHttpConnectorWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference JsHttpConnectorWrap::constructor_template;

JsHttpConnectorWrap::JsHttpConnectorWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsHttpConnectorWrap>(info)
	, m_env(info.Env())
	, m_pConnector(NULL)
	// 顺序跟着头文件里的声明顺序走，否则 -Wreorder-ctor
	, m_ePolicy(TT_VPN_POLICY_UNSET)
	, m_nMaxResponseBytes(0)
	, m_nDrainTimeoutMs(0)
	, m_nMaxIdleConnections(0)
	, m_nIdleTimeoutMs(0)
	, m_nNextTaskId(0)
	, m_bLogEnabled(false)
	, m_bClosePending(false)
{
	if (!info.IsConstructCall()) {
		THROW_ERROR_VOID(m_env, "Use the new operator to create new HttpConnector objects");
	}
	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_VOID(m_env, "Invalid config arguments");
	}
	Napi::Object lConfig = info[0].As<Napi::Object>();
	std::string  strError;
	if (!_ValidateConfig(lConfig, strError))
	{
		THROW_ERROR_VOID(m_env, strError.c_str());
	}
	BCRESULT result = Create(lConfig);
	if (result != BC_R_SUCCESS)
	{
		char szMsg[192];
		snprintf(szMsg, sizeof(szMsg),
				 "Failed to create HttpConnector: result=%u (%s)",
				 (unsigned)result, ResultName(result).c_str());
		THROW_ERROR_VOID(m_env, szMsg);
	}
	// ⚠️ 这里**故意不做** JsSMPConnectorWrap 那样的 m_hSelf 永久自引用。
	// 那个写法会让"创建了但没显式 close 的连接器"永远不被 GC 回收，而 HTTP
	// 连接器是很容易被这么用的。取而代之：在途请求期间用 Ref() 钉住对象，
	// 请求一结束就 Unref()，见 _Request / OnJsResult。
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsHttpConnectorWrap::~JsHttpConnectorWrap()
{
	_Cleanup();
}

void JsHttpConnectorWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "HttpConnector", {
		InstanceMethod<&JsHttpConnectorWrap::_Request>("__request__"),
		InstanceMethod<&JsHttpConnectorWrap::_Close>("__close__"),
		InstanceMethod<&JsHttpConnectorWrap::_Info>("__info__"),
		InstanceMethod<&JsHttpConnectorWrap::_SetIdleTimeout>("__setIdleTimeoutMs__"),
	});
	constructor_template.Reset(ctor);
	constructor_template.SuppressDestruct();
	exports.Set("HttpConnector", ctor);
}

BCRESULT JsHttpConnectorWrap::Create(Napi::Object config)
{
	Napi::HandleScope scope(m_env);

	if (m_pConnector)
	{
		return BC_R_EXISTS;
	}
	std::unique_ptr<BCFVar> pVar(ConvertBCFFromJS(m_env, config));
	if (!IS_BCF_OBJECT(pVar.get()))
	{
		return BC_R_INVALIDARG;
	}
	BCFObject *pConfig = (BCFObject *)pVar.get();

	// Runtime 必须活得比 HttpConnector 久（HttpConnector.h 契约第 5 条）。
	// 本模块从不调 Runtime::Destroy()，所以只要保证它已经初始化；已经初始化
	// 时 Runtime::Initialize 直接返回 BC_R_SUCCESS，不会覆盖已有线程配置。
	BCRESULT result = Runtime::Initialize(pConfig);
	if (result != BC_R_SUCCESS)
	{
		return result;
	}

	// 日志闸门要在 HttpConnector::Create 之前打开，否则它自己那行"已创建"的
	// INFO 会被挡掉。
	Napi::Value hLogCallback = config.Get("log_callback");
	if (hLogCallback.IsFunction())
	{
		m_hLogCallback.Reset(hLogCallback.As<Napi::Function>(), 1);
		m_bLogEnabled.store(true);
	}

	m_pConnector = new HttpConnector();
	result = m_pConnector->Create(pConfig, this);
	if (result != BC_R_SUCCESS)
	{
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
		m_hLogCallback.Reset();
		return result;
	}
	// 缓存生效配置，好让 __info__() 在连接器销毁之后仍然报出恒定形状。
	// 这几项在 Create 之后不会再变。
	m_ePolicy           = m_pConnector->EffectivePolicy();
	m_nMaxResponseBytes = m_pConnector->MaxResponseBytes();
	m_nDrainTimeoutMs   = m_pConnector->DrainTimeoutMs();
	m_nMaxIdleConnections = (uint32_t)m_pConnector->MaxIdleConnections();
	m_nIdleTimeoutMs    = m_pConnector->IdleTimeoutMs();
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// 参数校验
///////////////////////////////////////////////////////////////////////////////

bool JsHttpConnectorWrap::_ValidateConfig(Napi::Object config,
										  std::string &outError)
{
	Napi::HandleScope scope(m_env);

	// vpnPolicy：拼错了就当场抛，别让它在原生层留一行 WARN 然后静默回落平台
	// 默认档 —— force-physical 拼错回落成 prefer-physical 是会静默失去"真实
	// IP"保证的。判定用 tt_vpn_policy_from_string，不在这里重写取值表。
	Napi::Value hPolicy = config.Get("vpnPolicy");
	if (!hPolicy.IsUndefined() && !hPolicy.IsNull())
	{
		if (!hPolicy.IsString())
		{
			outError = "Invalid vpnPolicy: expected a string, one of "
					   "os, prefer-physical, force-physical.";
			return false;
		}
		std::string strPolicy = hPolicy.As<Napi::String>().Utf8Value();
		if (tt_vpn_policy_from_string(strPolicy.c_str()) == TT_VPN_POLICY_UNSET)
		{
			outError = "Invalid vpnPolicy: " + strPolicy +
					   ". Expected one of os, prefer-physical, force-physical.";
			return false;
		}
	}
	return true;
}

bool JsHttpConnectorWrap::_ParseRequest(Napi::Object options,
										HttpRequest &outRequest,
										std::string &outError)
{
	Napi::HandleScope scope(m_env);

	Napi::Value hUrl = options.Get("url");
	if (!hUrl.IsString())
	{
		outError = "Invalid request: 'url' is required and must be a string.";
		return false;
	}
	outRequest.url = hUrl.As<Napi::String>().Utf8Value();

	Napi::Value hValue = options.Get("method");
	if (!hValue.IsUndefined() && !hValue.IsNull())
	{
		if (!hValue.IsString())
		{
			outError = "Invalid request: 'method' must be a string.";
			return false;
		}
		std::string strMethod = hValue.As<Napi::String>().Utf8Value();
		if (!strMethod.empty())
		{
			outRequest.method = strMethod;
		}
	}

	hValue = options.Get("timeoutMs");
	if (!hValue.IsUndefined() && !hValue.IsNull())
	{
		if (!hValue.IsNumber())
		{
			outError = "Invalid request: 'timeoutMs' must be a number.";
			return false;
		}
		// 负数 / 超大值经 Uint32Value 会变成一个毫不相干的数字，当场拦住 ——
		// 同 HttpConnector 处理 drainTimeoutMs 时踩过的那个坑。
		double dTimeout = hValue.As<Napi::Number>().DoubleValue();
		if (!(dTimeout >= 0) || dTimeout > 4294967295.0)
		{
			outError = "Invalid request: 'timeoutMs' out of range "
					   "(expected 0 .. 4294967295).";
			return false;
		}
		outRequest.timeoutMs = (uint32_t)dTimeout;
	}

	hValue = options.Get("resolvedIp");
	if (!hValue.IsUndefined() && !hValue.IsNull())
	{
		if (!hValue.IsString())
		{
			outError = "Invalid request: 'resolvedIp' must be a string.";
			return false;
		}
		outRequest.resolvedIp = hValue.As<Napi::String>().Utf8Value();
	}

	hValue = options.Get("headers");
	if (!hValue.IsUndefined() && !hValue.IsNull())
	{
		if (!hValue.IsObject())
		{
			outError = "Invalid request: 'headers' must be an object.";
			return false;
		}
		Napi::Object hHeaders = hValue.As<Napi::Object>();
		Napi::Array  hNames   = hHeaders.GetPropertyNames();
		for (uint32_t i = 0; i < hNames.Length(); i++)
		{
			Napi::Value hName = hNames.Get(i);
			if (!hName.IsString())
			{
				continue;
			}
			std::string strName = hName.As<Napi::String>().Utf8Value();
			Napi::Value hItem   = hHeaders.Get(hName);
			if (hItem.IsUndefined() || hItem.IsNull())
			{
				continue;	// 与 fetch 一致：值为 undefined 的头当没给
			}
			if (!hItem.IsString())
			{
				outError = "Invalid request: header '" + strName +
						   "' must be a string.";
				return false;
			}
			if (strName.empty())
			{
				outError = "Invalid request: empty header name.";
				return false;
			}
			outRequest.headers[strName] = hItem.As<Napi::String>().Utf8Value();
		}
	}

	hValue = options.Get("body");
	if (!hValue.IsUndefined() && !hValue.IsNull())
	{
		if (hValue.IsBuffer())
		{
			Napi::Buffer<char> hBuffer = hValue.As<Napi::Buffer<char> >();
			outRequest.body.assign(hBuffer.Data(), hBuffer.Length());
		}
		else if (hValue.IsString())
		{
			outRequest.body = hValue.As<Napi::String>().Utf8Value();
		}
		else
		{
			outError = "Invalid request: 'body' must be a string or a Buffer.";
			return false;
		}
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// JS 侧方法
///////////////////////////////////////////////////////////////////////////////

Napi::Value JsHttpConnectorWrap::_Request(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsFunction())
	{
		THROW_ERROR_WITH_RESULT(env,
			"Invalid arguments: __request__(options, callback)", env.Undefined());
	}
	Napi::Object   hOptions  = info[0].As<Napi::Object>();
	Napi::Function hCallback = info[1].As<Napi::Function>();

	HttpRequest sRequest;
	std::string strError;
	if (!_ParseRequest(hOptions, sRequest, strError))
	{
		// 参数形状错误属于用法错误（写错类型），当场抛。真正的"请求失败"
		// 一律走回调，见下面。
		THROW_ERROR_WITH_RESULT(env, strError.c_str(), env.Undefined());
	}

	// 建任务并登记。id 单调递增且跳过 0（0 当"无效 id"用）。
	do
	{
		m_nNextTaskId++;
	} while (m_nNextTaskId == 0 || m_mapTasks.count(m_nNextTaskId) > 0);
	uint32_t nId = m_nNextTaskId;

	JsHttpRequestTask *pTask = new JsHttpRequestTask(this, nId);
	pTask->m_hCallback.Reset(hCallback, 1);
	m_mapTasks[nId] = pTask;
	// 在途请求期间钉住 JS 对象：否则用户一放手就可能被 GC 回收，析构会把
	// 在途请求连回调一起掐掉，业务那边的 Promise 永远不 settle。
	Ref();

	// HttpConnector::Request() 只给错误码，不给文案。URL / 请求头这两类最常见
	// 的错误在这里先用它自己的纯函数复核一遍，好把具体原因带给业务
	// （ParseUrl / BuildRequestText 都是 public static 纯函数，不重复实现）。
	HttpUrlParts sParts;
	std::string  strDetail;
	if (!HttpConnector::ParseUrl(sRequest.url, sParts, strDetail))
	{
		_PostFailure(nId, BC_R_INVALIDARG, "URL 解析失败：" + strDetail);
		return env.Undefined();
	}
	std::string strReqText;
	if (!HttpConnector::BuildRequestText(sRequest, sParts, strReqText, strDetail))
	{
		_PostFailure(nId, BC_R_INVALIDARG, "请求组装失败：" + strDetail);
		return env.Undefined();
	}

	BCRESULT result = m_pConnector
		? m_pConnector->Request(sRequest, pTask)
		: BC_R_NOTCONNECTED;
	if (result != BC_R_SUCCESS)
	{
		// 契约第 1 条：Request 返回非成功时保证不会有任何回调，pTask 不会再被
		// 原生层碰，可以安全地由我们自己收尾。
		_PostFailure(nId, result, _RequestFailureHint(result));
	}
	return env.Undefined();
}

Napi::Value JsHttpConnectorWrap::_Close(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() > 0 && info[0].IsFunction())
	{
		// ⚠️ 追加而不是覆盖。close() 是幂等的，但每一次调用都带着自己的回调；
		// 用单个引用去 Reset 会把前一个静默丢掉 —— Task 3 把 close 包成
		// Promise 之后那就是"第一个 Promise 永远悬着"，正是这一路刻意要避免
		// 的那类失败。第二次调用虽然被 m_bClosePending 挡掉了其余动作，
		// 回调仍然必须登记进来。
		Napi::FunctionReference hRef;
		hRef.Reset(info[0].As<Napi::Function>(), 1);
		m_vecCloseCallbacks.push_back(std::move(hRef));
	}
	if (m_pConnector && !m_bClosePending)
	{
		m_bClosePending = true;
		// OnClosed 事件到达之前别让对象被回收 —— 它要在那时销毁连接器。
		Ref();
		// ⚠️ Close() 有可能**同步**回调 OnClosed（当下没有在途请求时，
		// HttpConnector::Close 末尾的 _NotifyClosedIfDrained 会直接发）。
		// 我们的 OnClosed 只投递事件，所以这里不存在重入问题。
		m_pConnector->Close();
	}
	else if (!m_pConnector)
	{
		// 已经关完了。补投一次事件，免得调用方等一个永远不来的信号。
		// 走事件而不是直接调 OnJsClosed()，是为了保住"回调永远异步"这条语义。
		OnClosed();
	}
	return env.Undefined();
}

Napi::Value JsHttpConnectorWrap::_SetIdleTimeout(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsNumber())
	{
		Napi::TypeError::New(env,
			"setIdleTimeoutMs(ms): 'ms' must be a number.").ThrowAsJavaScriptException();
		return env.Undefined();
	}
	// 负数 / 超大值经 Uint32Value 会变成一个毫不相干的数字，当场拦住 ——
	// 同 timeoutMs / drainTimeoutMs 踩过的那个坑。
	double dMs = info[0].As<Napi::Number>().DoubleValue();
	if (!(dMs >= 0) || dMs > 4294967295.0)
	{
		Napi::RangeError::New(env,
			"setIdleTimeoutMs(ms): out of range (expected 0 .. 4294967295).")
			.ThrowAsJavaScriptException();
		return env.Undefined();
	}
	if (!m_pConnector)
	{
		// 已关闭：静默忽略而不是抛。关闭之后没有连接池可言，这个调用没有
		// 任何副作用，让它和 close() 的幂等语义保持一致。
		return env.Undefined();
	}
	m_pConnector->SetIdleTimeoutMs((uint32_t)dMs);
	m_nIdleTimeoutMs = m_pConnector->IdleTimeoutMs();
	return env.Undefined();
}

Napi::Value JsHttpConnectorWrap::_Info(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	Napi::Object hInfo = Napi::Object::New(env);
	hInfo.Set("closed", Napi::Boolean::New(env, m_pConnector == NULL));
	hInfo.Set("pendingRequests",
			  Napi::Number::New(env, (double)m_mapTasks.size()));

	// ⚠️ 形状必须**恒定**：诊断接口一旦关闭之后就少几个字段，JS 侧读到的是
	// undefined，排查的人会以为"策略没生效"。关闭后沿用创建时解析出来的值
	// （连接器已销毁，但这几项在 Create 之后就不会再变）。
	hInfo.Set("vpnPolicy", Napi::String::New(env,
		tt_vpn_policy_to_string(m_ePolicy)));
	hInfo.Set("maxResponseBytes",
			  Napi::Number::New(env, (double)m_nMaxResponseBytes));
	hInfo.Set("drainTimeoutMs",
			  Napi::Number::New(env, (double)m_nDrainTimeoutMs));
	hInfo.Set("maxIdleConnections",
			  Napi::Number::New(env, (double)m_nMaxIdleConnections));
	// 这一项运行期可改，所以连接器还在就现问；已关闭则沿用最后一次的值
	// （形状恒定的理由同上）。
	if (m_pConnector)
	{
		m_nIdleTimeoutMs = m_pConnector->IdleTimeoutMs();
	}
	hInfo.Set("idleTimeoutMs",
			  Napi::Number::New(env, (double)m_nIdleTimeoutMs));

	// 编译期有没有 path monitor。**这一行是刻意加的诊断出口**：没有
	// TT_HAS_PATH_MONITOR 时 TcpChannel::_PickPhysicalIfIndex() 整段被条件编译
	// 掉、恒返回 0，于是 prefer-physical 静默回落系统路由（force-physical 不会
	// 静默 —— TcpChannel.cpp:414-425 的硬校验写在 guard 之外，会直接
	// return BC_R_NO_PHYSICAL_INTERFACE=64）。把它暴露到 JS，"构建配置悄悄把
	// 网卡绑定关掉"这一整类缺陷在**任何**跑得起来的产物上都会当场露馅 ——
	// 包括手搓编译命令编出来的东西，而 CMake 的 configure 期断言管不到那些
	// （Task 0b 就是栽在手搓命令少了这个宏上）。
#if defined(TT_HAS_PATH_MONITOR)
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, true));
#else
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, false));
#endif
	return scope.Escape(hInfo);
}

///////////////////////////////////////////////////////////////////////////////
// 跨线程投递
///////////////////////////////////////////////////////////////////////////////

bool JsHttpConnectorWrap::PostResult(HttpResultPayload *pPayload)
{
	if (!pPayload)
	{
		return false;
	}
	BCEventItemS sEvent(MAKEEVENT(JHM_RESULT, 0, 0),
						(uint64_t)pPayload, (uint64_t)0, &_EventDtorCB);
	BCRESULT result = JsExchanger::ExchangeEvent(sEvent, this);
	if (result == BC_R_SUCCESS || result == BC_R_IGNORE)
	{
		// SUCCESS：进队了，payload 由 _EventDtorCB 在事件收尾时释放。
		// IGNORE ：ExchangeEvent 已经调过 BCDefEventProc，也就是已经释放了。
		return (result == BC_R_SUCCESS);
	}
	// 交换器实例已经没了（模块卸载中）。事件没进队，cbDestroy 不会被调用，
	// 这里自己收尾，否则 payload 泄漏。
	//
	// 注：这条分支**实际不可达** —— JsExchanger 的实例在 RegisterModule 里创建
	// 之后没有任何 Destroy 路径，m_pInstance 恒非空。保留它只是为了不依赖那个
	// 事实（哪天有人给交换器加了卸载逻辑，这里不会变成泄漏 + 悬着的 Promise）。
	// 也因此没有必要在这里回退 m_internal_events_count：ExchangeEvent 在返回
	// FAILURE 的那条路径上压根没有 ++ 过它。
	delete pPayload;
	sEvent.wParam = 0;
	return false;
}

void JsHttpConnectorWrap::_PostFailure(uint32_t nId, BCRESULT result,
									   const std::string &refMessage)
{
	HttpResultPayload *pPayload = new HttpResultPayload();

	pPayload->nId			= nId;
	pPayload->bOk			= false;
	pPayload->result		= result;
	pPayload->strErrMessage	= refMessage;

	if (!PostResult(pPayload))
	{
		// 投递不出去（只可能发生在模块拆除阶段）。我们此刻就在 Node 主线程上
		// （_PostFailure 只从 _Request / 关闭路径调用），直接把任务收干净，
		// 别让它挂在 m_mapTasks 里等析构。
		TaskMap::iterator iter = m_mapTasks.find(nId);
		if (iter != m_mapTasks.end())
		{
			JsHttpRequestTask *pTask = iter->second;
			m_mapTasks.erase(iter);
			pTask->m_hCallback.Reset();
			delete pTask;
			Unref();
		}
	}
}

void JsHttpConnectorWrap::_EventDtorCB(BCEventItemS &refEvent)
{
	// payload 的唯一释放点。事件正常处理完、被 RemoveEventByHandler 丢弃、
	// 或被交换器析构时 flush 掉，都会走到这里；置零让它幂等。
	if (EVENTMAJOR(refEvent.eType) == JHM_RESULT && refEvent.wParam)
	{
		delete (HttpResultPayload *)refEvent.wParam;
		refEvent.wParam = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Node 主线程侧
///////////////////////////////////////////////////////////////////////////////

Napi::Value JsHttpConnectorWrap::_MakeError(const HttpResultPayload &refPayload)
{
	Napi::EscapableHandleScope scope(m_env);

	std::string strName    = ResultName(refPayload.result);
	std::string strMessage = strName +
		"(" + std::to_string((unsigned)refPayload.result) + "): ";
	if (!refPayload.strErrMessage.empty())
	{
		strMessage += refPayload.strErrMessage;
	}
	else
	{
		// 库没给文案时退到 bc_result2string 的散文，别只丢一个数字出去
		LPCSTR lpszText = bc_result2string(refPayload.result);
		strMessage += lpszText ? lpszText : "unknown error";
	}
	Napi::Error  hError  = Napi::Error::New(m_env, strMessage);
	Napi::Object hObject = hError.Value();
	// 错误码原样透传，业务据此分档处理（例如 64 = 找不到物理网卡时降级重试）
	hObject.Set("result", Napi::Number::New(m_env, (double)refPayload.result));
	hObject.Set("errName", Napi::String::New(m_env, strName));
	hObject.Set("errMessage",
				Napi::String::New(m_env, refPayload.strErrMessage));
	return scope.Escape(hObject);
}

Napi::Value JsHttpConnectorWrap::_MakeResponse(const HttpResultPayload &refPayload)
{
	Napi::EscapableHandleScope scope(m_env);

	Napi::Object hResponse = Napi::Object::New(m_env);
	hResponse.Set("status", Napi::Number::New(m_env, refPayload.nStatus));
	// ⚠️ reason 是未净化的原始网络字节（可能含控制字符与内嵌 NUL）。
	// 打日志 / 拼 HTML 之前要自己过滤，见 HttpConnector.h 的说明。
	hResponse.Set("reason", Napi::String::New(m_env, refPayload.strReason));

	// 等价于 napi_default_jsproperty，但不依赖 NAPI_VERSION >= 8 那个条件编译
	const napi_property_attributes eAttrs = (napi_property_attributes)
		(napi_writable | napi_enumerable | napi_configurable);
	Napi::Object hHeaders = Napi::Object::New(m_env);
	for (HttpHeaderMap::const_iterator iter = refPayload.mapHeaders.begin();
		 iter != refPayload.mapHeaders.end(); ++iter)
	{
		// ⚠️ 用 DefineProperty 而不是 Set：头名完全由对端决定，而 Set 走的是
		// 普通属性赋值语义 —— 遇到 "__proto__" 之类的特殊名会触发原型上的
		// setter，而不是建一个自有属性。DefineProperty 直接定义自有属性，
		// 绕开所有 setter，远端也就没法拿响应头去动 JS 对象的原型。
		hHeaders.DefineProperty(Napi::PropertyDescriptor::Value(
			iter->first,
			Napi::String::New(m_env, iter->second),
			eAttrs));
	}
	hResponse.Set("headers", hHeaders);

	Napi::Buffer<uint8_t> hBody = refPayload.strBody.empty()
		? Napi::Buffer<uint8_t>::New(m_env, 0)
		: Napi::Buffer<uint8_t>::Copy(m_env,
									  (const uint8_t *)refPayload.strBody.data(),
									  refPayload.strBody.size());
	hResponse.Set("body", hBody);

	// 这三项是刻意暴露的：业务无需翻日志就能判断这次请求有没有真的走物理网卡。
	// boundIfIndex 非 0 即真的绑了网卡，pinMethod 说明用的是哪种绑法。
	hResponse.Set("peerIp", Napi::String::New(m_env, refPayload.strPeerIp));
	hResponse.Set("boundIfIndex",
				  Napi::Number::New(m_env, (double)refPayload.nBoundIfIndex));
	hResponse.Set("pinMethod", Napi::String::New(m_env, refPayload.strPinMethod));

	return scope.Escape(hResponse);
}

void JsHttpConnectorWrap::OnJsResult(HttpResultPayload *pPayload)
{
	Napi::HandleScope scope(m_env);

	TaskMap::iterator iter = m_mapTasks.find(pPayload->nId);
	if (iter == m_mapTasks.end())
	{
		// 任务已经被关闭 / 析构路径清理掉了，不再回调。
		return;
	}
	JsHttpRequestTask *pTask = iter->second;
	// 先摘除再调 JS：回调里完全可以再发一次请求（甚至 close），摘除在前才能
	// 保证那时看到的 m_mapTasks 是自洽的。
	m_mapTasks.erase(iter);

	if (pTask->m_hCallback.Value().IsFunction())
	{
		Napi::Function hCallback = pTask->m_hCallback.Value().As<Napi::Function>();
		Napi::Value    hSelf     = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		ArgsList argv = {
			pPayload->bOk ? m_env.Null()   : _MakeError(*pPayload),
			pPayload->bOk ? _MakeResponse(*pPayload) : m_env.Null()
		};
		TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
		DrainPendingException(m_env);
	}
	pTask->m_hCallback.Reset();
	delete pTask;
	// 与 _Request 里的 Ref() 配平。放在最后：Unref 之后本对象随时可能被 GC。
	Unref();
}

void JsHttpConnectorWrap::OnJsLog(int level, LPCSTR lpszMsg)
{
	Napi::HandleScope scope(m_env);

	if (m_hLogCallback.Value().IsFunction())
	{
		Napi::Function hCallback = m_hLogCallback.Value().As<Napi::Function>();
		Napi::Value    hSelf     = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		ArgsList argv = {
			Napi::Number::New(m_env, level),
			Napi::String::New(m_env, lpszMsg ? lpszMsg : "")
		};
		TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
		DrainPendingException(m_env);
	}
}

void JsHttpConnectorWrap::_SettlePendingTasks(BCRESULT result,
											  const std::string &refMessage)
{
	Napi::HandleScope scope(m_env);

	while (!m_mapTasks.empty())
	{
		TaskMap::iterator  iter  = m_mapTasks.begin();
		JsHttpRequestTask *pTask = iter->second;
		m_mapTasks.erase(iter);

		HttpResultPayload sPayload;
		sPayload.nId			= pTask->m_nId;
		sPayload.bOk			= false;
		sPayload.result			= result;
		sPayload.strErrMessage	= refMessage;

		if (pTask->m_hCallback.Value().IsFunction())
		{
			Napi::Function hCallback =
				pTask->m_hCallback.Value().As<Napi::Function>();
			Napi::Value hSelf = Value();
			if (hSelf.IsEmpty())
			{
				hSelf = m_env.Undefined();
			}
			ArgsList argv = { _MakeError(sPayload), m_env.Null() };
			TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
			DrainPendingException(m_env);
		}
		pTask->m_hCallback.Reset();
		delete pTask;
		Unref();
	}
}

void JsHttpConnectorWrap::OnJsClosed()
{
	Napi::HandleScope scope(m_env);

	// 契约第 3 条保证 OnClosed 排在所有在途请求回调之后，所以正常路径下这里
	// 已经没有任务了；兜一遍是为了 drainTimeoutMs=0 那类"放弃等待、在途请求
	// 被静默丢弃"的配置 —— 不兜的话业务的 Promise 永远不 settle。
	_SettlePendingTasks(BC_R_SHUTTINGDOWN,
						"HttpConnector 已关闭，在途请求被取消");

	// 连接器已经收尾完毕，这里销毁它以释放日志 appender 等资源。
	// ⚠️ 这不是"在回调里析构"（契约 7(3)）：我们是从 uv_async 回到 Node 主线程
	// 之后才做的，HTTP 那条回调栈早就返回了。也不持任何业务锁（契约 7(2)）。
	if (m_pConnector)
	{
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
	}

	// 先整体搬出来再逐个触发：回调里完全可以再调一次 __close__()（那时
	// m_pConnector 已经是 NULL，会补投一个事件），搬出来才不会在遍历过程中
	// 被追加进来的元素搅乱。
	std::vector<Napi::FunctionReference> vecCallbacks;
	vecCallbacks.swap(m_vecCloseCallbacks);
	for (size_t i = 0; i < vecCallbacks.size(); i++)
	{
		if (!vecCallbacks[i].Value().IsFunction())
		{
			continue;
		}
		Napi::Function hCallback = vecCallbacks[i].Value().As<Napi::Function>();
		Napi::Value    hSelf     = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		ArgsList argv = {};
		TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
		DrainPendingException(m_env);
	}
	for (size_t i = 0; i < vecCallbacks.size(); i++)
	{
		vecCallbacks[i].Reset();
	}
	if (m_hCallback.Value().IsFunction())
	{
		Napi::Function hCallback = m_hCallback.Value().As<Napi::Function>();
		Napi::Value    hSelf     = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		ArgsList argv = { Napi::String::New(m_env, close_sym) };
		TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
		DrainPendingException(m_env);
	}
	if (m_bClosePending)
	{
		m_bClosePending = false;
		Unref();		// 与 _Close 里的 Ref() 配平，放最后
	}
}

void JsHttpConnectorWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);

	// 1) 先关掉日志转发。析构过程里还会产生日志，没必要再往 JS 投。
	m_bLogEnabled.store(false);

	// 2) 关连接器并等它收尾。
	//    ⚠️ 这里不持任何业务锁（Node 主线程就是业务线程，而我们从不在持锁状态
	//    下析构），也不在任何 HTTP 回调栈里 —— 满足 HttpConnector.h 契约
	//    第 5 / 7(2) / 7(3) 条。先 Close() 再 delete，把"等在途请求收尾"这段
	//    等待压到最短（否则默认 drainTimeoutMs=30s 有可能真的堵住主线程）。
	if (m_pConnector)
	{
		m_pConnector->Close();
		BC_SAFE_DELETE_PTR(m_pConnector);
	}

	// 3) 连接器没了，此后不会再有任何 HTTP 回调，也就不会再有新事件被投递。
	//    把已经进队但还没跑的事件丢掉（payload 由 _EventDtorCB 释放）。
	//    时机上是安全的：事件的处理与丢弃都发生在 Node 主线程，也就是当前
	//    这条线程，不存在"正在处理"的并发副本。
	JsExchanger::RemoveEventByHandler(this);

	// 4) 清理还挂着的任务。**这里不回调 JS** —— 析构可能发生在 env 拆除阶段，
	//    那时调用 JS 不安全。所以正常业务应当显式 __close__()：那条路径会把
	//    在途请求以错误结束（见 OnJsClosed）。
	while (!m_mapTasks.empty())
	{
		TaskMap::iterator iter = m_mapTasks.begin();
		iter->second->m_hCallback.Reset();
		delete iter->second;
		m_mapTasks.erase(iter);
	}
	m_hCallback.Reset();
	m_hLogCallback.Reset();
	for (size_t i = 0; i < m_vecCloseCallbacks.size(); i++)
	{
		m_vecCloseCallbacks[i].Reset();
	}
	m_vecCloseCallbacks.clear();
}

///////////////////////////////////////////////////////////////////////////////
// IExchangeHandler
///////////////////////////////////////////////////////////////////////////////

bool JsHttpConnectorWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsHttpConnectorWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JHM_RESULT:
		if (refEvent.wParam)
		{
			OnJsResult((HttpResultPayload *)refEvent.wParam);
		}
		break;
	case JHM_LOG_MSG:
		OnJsLog((int)refEvent.wParam, (LPCSTR)refEvent.lParam);
		break;
	case JHM_CLOSE:
		OnJsClosed();
		break;
	default:
		break;
	}
	// 返回 true 让调用方走 BCDefEventProc，也就是调 cbDestroy 释放 payload。
	return true;
}

void JsHttpConnectorWrap::OnExchangeShutdown()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// IHttpConnectorHandler（任意线程）
///////////////////////////////////////////////////////////////////////////////

void JsHttpConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	// ⚠️ 本回调是 BCLogger 持着它的**全局非递归自旋锁**调进来的。除了"把字符串
	// 拷走 + 投递事件"以外什么都不能做 —— 特别是不能回头调 HttpConnector 的
	// 任何会打日志的 API（Request / 析构），那会重进那把非递归锁而 100% CPU
	// 空转挂死。详见 HttpConnector.h 里关于 OnLog 的那段。
	if (!m_bLogEnabled.load() || !lpszMsg)
	{
		return;
	}
	BCEventItemS sEvent(MAKEEVENT(JHM_LOG_MSG, 0, 0));
	sEvent.wParam = (uint64_t)level;
	sEvent.lParam = (uint64_t)sEvent.CopyString(lpszMsg);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsHttpConnectorWrap::OnClosed()
{
	// 可能来自任意线程，也可能就在调用方的 Close() 栈里同步发出。只投递。
	BCEventItemS sEvent(MAKEEVENT(JHM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file : JsHttpConnectorWrap.cpp
///////////////////////////////////////////////////////////////////////////////
