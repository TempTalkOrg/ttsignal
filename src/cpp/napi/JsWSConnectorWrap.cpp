///////////////////////////////////////////////////////////////////////////////
// file : JsWSConnectorWrap.cpp
// author : anto.
//
// 设计要点、线程模型与生命周期（尤其"WS 长连接的引用怎么配平"）见
// JsWSConnectorWrap.h 的文件头注释。
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "../Runtime.h"
#include "../TTErrors.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsWSConnectorWrap.h"

#include <BC/Utils.h>

using namespace node;

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

enum
{
	JWM_CONN_EVENT	= 1,	// 一个连接事件（wParam = WSEventPayload*）
	JWM_LOG_MSG		= 2,	// 库日志
	JWM_CLOSE		= 3,	// 连接器收尾完毕
	JWM_EXCEPTION	= 4,	// 连接器级异常
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

// 错误码符号名与"清 pending exception"都在 napi/Utils.{h,cpp} 里
// （ResultName() / DrainPendingException()）—— HTTP 绑定也用同一份，各存一份会
// 漂移出不一致的 errName。为什么不用 bc_result2string、以及不清 pending exception
// 会怎么把 node 进程打死，那两段长注释都在 Utils.cpp 里。

///////////////////////////////////////////////////////////////////////////////
// _WSConnectFailureHint —— 连接失败的诊断文案
//
// ⚠️ **这里是在补一个原生接口的缺口，不是在重复造轮子。**
//    IHttpRequestHandler::OnHttpError(result, message) 带诊断文案，而
//    IWSConnectionHandler::OnConnectResult(result, headers) **没有 message
//    形参**（Interface.h:255-257）。WSConnection::_DeliverOnce 里那段写得很细的
//    fail_message_（TcpChannel 给的 force-physical 现场描述）只进了日志，
//    没有任何公开途径能取到（fail_message_ 是 private，也没有 accessor）。
//    所以这里按错误码给一条**可照着改**的文案，让 JS 侧的 err.errMessage 不至于
//    只有一个数字。原生层那条更详细的描述仍然在 logFile / log_callback 里。
//    这个缺口已经作为 concern 上报，等原生接口补了 message 就应当改成原样透传。
//
// 措辞与 TcpChannel.cpp:920-970 那几条保持一致，免得同一件事两种说法。
///////////////////////////////////////////////////////////////////////////////

static std::string _WSConnectFailureHint(BCRESULT result)
{
	switch (result)
	{
	case BC_R_NO_PHYSICAL_INTERFACE:
		return "force-physical：找不到可用的物理网卡（当前 active 路径可能只有 "
			   "VPN 隧道，或产物缺少 TT_HAS_PATH_MONITOR —— 用 __info__() 的 "
			   "hasPathMonitor 核一下）。要连本机或内网服务端请显式使用 "
			   "vpnPolicy=os，或改用 prefer-physical（会自动放弃绑定）。";
	case BC_R_PIN_FAILED:
		return "force-physical：把 socket 绑到物理网卡失败。force-physical 的语义"
			   "是\"只走物理网卡，做不到就失败\"，所以这里不回落。要连本机或内网"
			   "服务端请显式使用 vpnPolicy=os，或改用 prefer-physical。";
	case BC_R_ROUTE_MISMATCH:
		return "force-physical：路由复核不通过 —— 内核仍然会把到该对端的包判给"
			   "别的网卡（环回 / 私网 / VPN 隧道对端最常见）。包不会从物理网卡"
			   "出去，服务端拿不到真实 IP，拒绝继续。要连本机或内网服务端请显式"
			   "使用 vpnPolicy=os，或改用 prefer-physical（会自动放弃绑定）。"
			   "具体是哪块网卡、哪个对端，见库日志里 [TcpChannel] 那一行。";
	case BC_R_DNS_FAILED:
		return "所有 DNS server 都失败了，域名解析不出来。force-physical / "
			   "prefer-physical 下 DNS 走的是自建查询（刻意绕开 VPN 的 TUN，"
			   "否则会拿到 fake-IP），可用 dnsServers 显式指定。";
	case BC_R_TLS_VERIFY_FAILED:
		return "TLS 校验失败：证书链 / 主机名 / SPKI pin 有一项没过。自签调试"
			   "环境请给 caCerts，或（仅调试）insecureSkipVerify=true。";
	case BC_R_WS_HANDSHAKE_FAILED:
		return "WebSocket 握手失败：响应不是 101，或 Upgrade / Connection / "
			   "Sec-WebSocket-Accept 校验不通过（对端很可能不是 WebSocket 端点，"
			   "或路径写错了）。具体哪一项不对，见库日志里 [WSConnection#] "
			   "那一行。";
	case BC_R_CONNECT_TIMEOUT:
		return "连接超时：DNS + connect + TLS + WebSocket 握手响应的总预算用完了"
			   "（connectTimeoutMs，或 __connect__ 的 timeoutMs）。";
	case BC_R_CONNREFUSED:
		return "对端拒绝连接（端口没在听）。";
	case BC_R_UNEXPECTEDEND:
		return "连接在 WebSocket 握手完成前被对端关闭。";
	case BC_R_CANCELED:
		return "连接被取消：连接器已 close，或本连接被主动关闭。";
	case BC_R_NOTIMPLEMENTED:
		return "不能在 log_callback 里发起连接（BC 日志器持着全局自旋锁，"
			   "重进会挂死）。";
	case BC_R_INVALIDARG:
		return "参数非法：URL 无法解析（scheme 必须是 ws / wss），或请求头里"
			   "含 CR/LF。";
	case BC_R_NOTCONNECTED:
		return "连接器已经关闭，或这条连接还没建立。";
	case BC_R_SHUTTINGDOWN:
		return "连接器正在关闭，拒绝新连接。";
	default:
		return "";		// 交给 bc_result2string 的散文
	}
}

// 造一个带 result / errName / errMessage 三个字段的 Error。错误码**原样透传**，
// 业务据此分档处理（例如 64 = 找不到物理网卡时降级重试）。
static Napi::Value _MakeWSError(Napi::Env env, BCRESULT result,
								const std::string &refMessage)
{
	Napi::EscapableHandleScope scope(env);

	std::string strName    = ResultName(result);
	std::string strDetail  = refMessage.empty() ? _WSConnectFailureHint(result)
												: refMessage;
	std::string strMessage = strName +
		"(" + std::to_string((unsigned)result) + "): ";
	if (!strDetail.empty())
	{
		strMessage += strDetail;
	}
	else
	{
		// 一句话都没有时退到 bc_result2string 的散文，别只丢一个数字出去
		LPCSTR lpszText = bc_result2string(result);
		strMessage += lpszText ? lpszText : "unknown error";
	}
	Napi::Error  hError  = Napi::Error::New(env, strMessage);
	Napi::Object hObject = hError.Value();
	hObject.Set("result",  Napi::Number::New(env, (double)result));
	hObject.Set("errName", Napi::String::New(env, strName));
	hObject.Set("errMessage", Napi::String::New(env, strDetail));
	return scope.Escape(hObject);
}

///////////////////////////////////////////////////////////////////////////////
// class : JsWSConnSink（TcpChannel 线程）
///////////////////////////////////////////////////////////////////////////////

JsWSConnSink::JsWSConnSink(JsWSConnectorWrap *pOwner, uint64_t nConnId)
	: m_pOwner(pOwner)
	, m_nConnId(nConnId)
{
	//
}

JsWSConnSink::~JsWSConnSink()
{
	//
}

void JsWSConnSink::Detach()
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_pOwner = NULL;
}

void JsWSConnSink::_Post(WSEventPayload *pPayload)
{
	if (!pPayload)
	{
		return;
	}
	pPayload->nConnId = m_nConnId;

	// ⚠️ 持 m_lock 调 PostEventPayload（内部会拿交换器的自旋锁）。加锁顺序恒为
	// sink->lock -> 交换器锁，反方向不存在：Node 主线程只在 Detach 里拿 sink->lock，
	// 而 Detach 排在 `delete WSConnector` 之后、RemoveEventByHandler 之前 ——
	// 那时所有回调线程都已收尾，压根不会来争这把锁。
	//
	// ⚠️ m_pOwner 在这里一定还活着，靠的也是同一个次序：连接器 wrap 的析构先
	// `delete WSConnector`（同步等在途回调跑完），才 Detach 各个 sink。所以只要
	// 还能走到这一行，那个 delete 就还没返回 —— 析构函数体正在跑，但成员（包括
	// IExchangeHandler 那几个基类字段）都还没销毁，投递是安全的；投进去的事件
	// 随后会被 _Cleanup 里的 RemoveEventByHandler 丢掉。
	std::lock_guard<std::mutex> lk(m_lock);
	if (!m_pOwner)
	{
		delete pPayload;
		return;
	}
	m_pOwner->PostEventPayload(pPayload);
}

void JsWSConnSink::OnConnectResult(BCRESULT result, const HttpHeaderMap &refHeaders)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->eEvent		= WSJS_CONNECT_RESULT;
	pPayload->result		= result;
	pPayload->mapHeaders	= refHeaders;

	// ⚠️ 诊断信息必须**趁通道还活着**抓：OnChannelClosed 一跑，channel_ 就被摘走
	// 交给 Runtime 任务销毁，PeerIp() 之后只能读连接自己缓存的那份。这里还在
	// 回调里，通道一定还在。
	// 本回调按 WSConnector 的不变量发生在 state->lock **之外**，所以调 PeerIp()
	// （内部要拿那把锁）不构成重入。
	WSConnPtr pConn = m_wpConn.lock();
	if (pConn)
	{
		pPayload->strPeerIp		= pConn->PeerIp();
		pPayload->nBoundIfIndex	= pConn->BoundIfIndex();
		pPayload->strPinMethod	= pConn->PinMethod();
		if (result != BC_R_SUCCESS)
		{
			// ⚠️ **原生现场描述在这里取**。OnConnectResult 的签名里没有 message
			// 形参（Interface.h:255-257），所以文案要回头找连接要 ——
			// WSConnection::_DeliverOnce 保证它在派发本回调之前就写好了。
			// 这一段才是 force-physical 排查真正要看的东西（哪块网卡、哪个对端、
			// scoped 路由把包判给了谁）；取不到（例如 Connect() 同步失败那条路，
			// 压根没走过 _DeliverOnce）时留空，由 _MakeWSError 回落到
			// _WSConnectFailureHint 的通用提示 —— **绝不能用空串把提示覆盖掉**。
			pPayload->strErrMessage = pConn->LastErrorMessage();
		}
	}
	_Post(pPayload);
}

void JsWSConnSink::OnRecvText(LPCSTR lpszText)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->eEvent = WSJS_TEXT;
	if (lpszText)
	{
		// ⚠️ 原生签名是 NUL 结尾的 C 字符串，所以文本帧里内嵌的 NUL 在这一层
		// 之前就已经被截断了（Interface.h 里 IWSConnectionHandler 的契约写明了）。
		// 需要精确长度的内容走二进制帧。
		pPayload->strBody = lpszText;
	}
	_Post(pPayload);
}

void JsWSConnSink::OnRecvData(LPCVOID data, size_t size)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->eEvent = WSJS_DATA;
	if (data && size > 0)
	{
		pPayload->strBody.assign((const char *)data, size);
	}
	_Post(pPayload);
}

void JsWSConnSink::OnClosed(LPCSTR lpszReason)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->eEvent = WSJS_CLOSED;
	if (lpszReason)
	{
		pPayload->strBody = lpszReason;
	}
	// OnClosed 收的 reason 是 _CloseReasonText 归纳过的短句（"对端发起关闭握手
	// （code=1000）"这类）。原始文案另外带一份，给 conn.__info__().lastError 用 ——
	// "连上之后为什么断"同样要可诊断。
	WSConnPtr pConn = m_wpConn.lock();
	if (pConn)
	{
		pPayload->strErrMessage = pConn->LastErrorMessage();
	}
	_Post(pPayload);
}

void JsWSConnSink::OnException(BCException &refExcept)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->eEvent  = WSJS_EXCEPTION;
	pPayload->strBody = refExcept.GetMsg().c_str();
	_Post(pPayload);
}

///////////////////////////////////////////////////////////////////////////////
// class : JsWSConnectionWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference JsWSConnectionWrap::constructor_template;

JsWSConnectionWrap::JsWSConnectionWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsWSConnectionWrap>(info)
	, m_env(info.Env())
	// 顺序跟着头文件里的声明顺序走，否则 -Wreorder-ctor
	, m_pOwner(NULL)
	, m_nConnId(0)
	, m_bConnectCalled(false)
	, m_bConnectSettled(false)
	, m_bUpgraded(false)
	, m_bClosedSeen(false)
	, m_bPinned(false)
	, m_nBoundIfIndex(0)
{
	if (!info.IsConstructCall())
	{
		THROW_ERROR_VOID(m_env,
			"Use the new operator to create new WsConnection objects");
	}
	// ⚠️ 正常路径下本构造只由 JsWSConnectorWrap::_CreateConnection 触发，随后
	// Attach() 才把原生连接装上。业务直接 `new WsConnection()` 也不会崩：所有
	// 方法在 m_pConn 为空时一律返回 BC_R_NOTCONNECTED / 抛用法错误。
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsWSConnectionWrap::~JsWSConnectionWrap()
{
	// ⚠️ **不调 JS**：析构可能发生在 env 拆除阶段，那时调用 JS 不安全。正常业务
	// 应当显式 __close__()，那条路径会把 connectResult / closed 正常发出去。
	if (m_pOwner)
	{
		// 从事件路由表里摘除。sink **不在这里删** —— 原生层可能还握着它当
		// handler_，它的释放时机严格定在 `delete WSConnector` 之后。
		m_pOwner->OnConnectionWrapGone(m_nConnId);
		m_pOwner = NULL;
	}
	// 钉住状态不需要（也不能）在这里配平：本对象正在被销毁，引用计数已无意义。
	m_bPinned = false;
	m_hCallback.Reset();
	m_hConnectCallback.Reset();
	m_hConnector.Reset();
	// 原生连接对象由 WSConnector 一直持强引用直到彻底收尾，所以这里放手是安全的
	// （WSConnector.h 契约第 3 条）。
	m_pConn.reset();
}

void JsWSConnectionWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "WsConnection", {
		InstanceMethod<&JsWSConnectionWrap::_Connect>("__connect__"),
		InstanceMethod<&JsWSConnectionWrap::_SendText>("__sendText__"),
		InstanceMethod<&JsWSConnectionWrap::_SendData>("__sendData__"),
		InstanceMethod<&JsWSConnectionWrap::_SendPing>("__sendPing__"),
		InstanceMethod<&JsWSConnectionWrap::_Close>("__close__"),
		InstanceMethod<&JsWSConnectionWrap::_Info>("__info__"),
	});
	constructor_template.Reset(ctor);
	constructor_template.SuppressDestruct();
	exports.Set("WsConnection", ctor);
}

void JsWSConnectionWrap::Attach(JsWSConnectorWrap *pOwner,
								uint64_t nConnId,
								const WSConnPtr &refConn,
								Napi::Object hConnector)
{
	m_pOwner  = pOwner;
	m_nConnId = nConnId;
	m_pConn   = refConn;
	// 连接器 JS 对象的强引用：没有它，业务只留着 conn、不再引用 ws 时连接器会被
	// GC，而它的析构会 delete WSConnector —— 把这条正连着的连接一起关掉。
	// 方向是单向的（连接 -> 连接器），不成环。
	if (!hConnector.IsEmpty())
	{
		m_hConnector.Reset(hConnector, 1);
	}
}

void JsWSConnectionWrap::OnOwnerGone()
{
	// 连接器 wrap 先析构了。这只可能发生在 env 拆除阶段（正常 GC 下 m_hConnector
	// 那条强引用保证连接器活得更久），所以这里同样**不调 JS**。
	m_pOwner = NULL;
	m_hConnectCallback.Reset();
	// ⚠️ 真的 Unref()，不是只把 m_bPinned 抹掉 —— 抹掉标志只是把那次 Ref() 泄掉，
	// 与注释说的"解钉"不是一件事。这里 Unref **另一个**对象的引用计数是合法的：
	// 本 wrap 还在调用方的 m_mapConnWraps 里，也就意味着它还没 finalize（wrap 是
	// 在自己的析构里摘除登记的），引用句柄仍然有效。
	// 反过来，谁都不该在**自己**的析构里 Unref 自己（见 _Cleanup 对 close 那几次
	// Ref 的处理）。
	// Unref 不会同步触发 GC，所以调用方遍历 m_mapConnWraps 时不会被打断。
	_Unpin();
	m_bClosedSeen = true;
	// 原生连接对象也放手：连接器已经没了，留着它只是让 handler_ 已成野指针的
	// WSConnection 多活一会儿（见 DetachNativeConn 的注释）。
	m_pConn.reset();
}

void JsWSConnectionWrap::AbandonPendingConnect()
{
	// 与 OnOwnerGone 的区别：**连接器 wrap 还活着**，所以 m_pOwner 绝不能置空 ——
	// 置空了本 wrap 的析构就不会去 OnConnectionWrapGone() 销账，调用方的
	// m_mapConnWraps 里会留下一个指向已释放对象的裸指针（下一个事件就是 UAF）。
	// 这里只做"这次 connect 没结果了"的收尾：丢回调 + 解钉。
	m_hConnectCallback.Reset();
	m_bConnectSettled = true;
	m_bClosedSeen     = true;	// 握手没成功过就是终局，不会再有 closed
	_Unpin();
}

void JsWSConnectionWrap::DetachNativeConn()
{
	// 由连接器 wrap 在 `delete WSConnector` **之后**调用（sink 也在那时释放）。
	//
	// ⚠️ 为什么要放手：sink 一释放，原生 WSConnection 里的 handler_ 就是野指针了。
	// 今天没人会去解引用它（delivered_ / connect_notified_ 两个闩加上
	// channel_ == NULL 挡住了所有派发路径，实测 400 轮 + MallocScribble 毒化也
	// 复现不了），但那份可达性是**别人的不变量**，改一行 WSConnector 就可能失效。
	// 这里主动放手，把这一整类问题消掉：连接器都没了，留着这个 shared_ptr 也没有
	// 任何用处。
	//
	// 诊断快照（m_strPeerIp / m_nBoundIfIndex / m_strPinMethod / m_strLastError）
	// 刻意保留 —— __info__() 的形状必须恒定，放手之后就靠它们报值。
	m_pConn.reset();
}

void JsWSConnectionWrap::_Pin()
{
	if (!m_bPinned)
	{
		m_bPinned = true;
		Ref();
	}
}

void JsWSConnectionWrap::_Unpin()
{
	if (m_bPinned)
	{
		m_bPinned = false;
		// ⚠️ Unref 之后本对象随时可能被 GC，调用方必须把它放在最后一步。
		Unref();
	}
}

void JsWSConnectionWrap::_EmitInternal(LPCSTR lpszType, Napi::Value hArg)
{
	Napi::HandleScope scope(m_env);

	if (!m_hCallback.Value().IsFunction())
	{
		return;		// 胶水层没在原型上装 _internalCallback = 不订阅这些事件
	}
	Napi::Function hCallback = m_hCallback.Value().As<Napi::Function>();
	Napi::Value    hSelf     = Value();
	if (hSelf.IsEmpty())
	{
		hSelf = m_env.Undefined();
	}
	Napi::Value hType = Napi::String::New(m_env, lpszType);
	if (hArg.IsEmpty())
	{
		hArg = m_env.Undefined();
	}
	ArgsList argv = { hType, hArg };
	TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
	// 每次调完 JS 都必须清，否则同一批 uv_async 里的下一个事件会被打死，详见
	// napi/Utils.cpp 里 DrainPendingException 的注释。
	DrainPendingException(m_env);
}

void JsWSConnectionWrap::_SettleConnect(const WSEventPayload &refPayload)
{
	Napi::HandleScope scope(m_env);

	m_bConnectSettled = true;
	if (refPayload.result == BC_R_SUCCESS)
	{
		m_bUpgraded = true;
	}
	// 诊断快照。留一份是为了让 __info__() 在通道销毁之后仍报出恒定形状。
	m_strPeerIp		= refPayload.strPeerIp;
	m_nBoundIfIndex	= refPayload.nBoundIfIndex;
	m_strPinMethod	= refPayload.strPinMethod;
	if (!refPayload.strErrMessage.empty())
	{
		m_strLastError = refPayload.strErrMessage;
	}

	if (m_hConnectCallback.Value().IsFunction())
	{
		Napi::Function hCallback =
			m_hConnectCallback.Value().As<Napi::Function>();
		// 先摘再调：回调里完全可以再调 __close__ / __sendText__，摘除在前才能
		// 保证那时看到的状态是自洽的。
		m_hConnectCallback.Reset();

		Napi::Value hSelf = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		if (refPayload.result == BC_R_SUCCESS)
		{
			// 等价于 napi_default_jsproperty，但不依赖 NAPI_VERSION >= 8 那个
			// 条件编译
			const napi_property_attributes eAttrs = (napi_property_attributes)
				(napi_writable | napi_enumerable | napi_configurable);
			Napi::Object hHeaders = Napi::Object::New(m_env);
			for (HttpHeaderMap::const_iterator iter = refPayload.mapHeaders.begin();
				 iter != refPayload.mapHeaders.end(); ++iter)
			{
				// ⚠️ 用 DefineProperty 而不是 Set：头名完全由对端决定，而 Set 走
				// 的是普通属性赋值语义 —— 遇到 "__proto__" 之类的特殊名会触发
				// 原型上的 setter，而不是建一个自有属性。DefineProperty 直接定义
				// 自有属性，绕开所有 setter，远端也就没法拿响应头去动 JS 对象的
				// 原型。
				hHeaders.DefineProperty(Napi::PropertyDescriptor::Value(
					iter->first,
					Napi::String::New(m_env, iter->second),
					eAttrs));
			}
			Napi::Object hInfo = Napi::Object::New(m_env);
			hInfo.Set("headers", hHeaders);
			// 这三项是刻意暴露的：业务无需翻日志就能判断这条连接有没有真的走
			// 物理网卡。boundIfIndex 非 0 即真的绑了网卡，pinMethod 说明绑法。
			hInfo.Set("peerIp", Napi::String::New(m_env, refPayload.strPeerIp));
			hInfo.Set("boundIfIndex",
					  Napi::Number::New(m_env, (double)refPayload.nBoundIfIndex));
			hInfo.Set("pinMethod",
					  Napi::String::New(m_env, refPayload.strPinMethod));

			ArgsList argv = { m_env.Null(), hInfo };
			TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
			DrainPendingException(m_env);
		}
		else
		{
			ArgsList argv = {
				_MakeWSError(m_env, refPayload.result, refPayload.strErrMessage),
				m_env.Null()
			};
			TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
			DrainPendingException(m_env);
		}
	}

	if (refPayload.result != BC_R_SUCCESS)
	{
		// 握手没成功 —— 按 WSConnector.h 契约第 2 条，**不会再有 OnClosed**，
		// 所以这次 connectResult 就是终局，钉住到此为止。
		m_bClosedSeen = true;
		_Unpin();
	}
}

void JsWSConnectionWrap::OnJsEvent(const WSEventPayload &refPayload)
{
	Napi::HandleScope scope(m_env);

	switch (refPayload.eEvent)
	{
	case WSJS_CONNECT_RESULT:
		if (m_bConnectSettled)
		{
			break;		// 契约保证只有一次；重复到达一律丢弃
		}
		_SettleConnect(refPayload);
		break;
	case WSJS_TEXT:
		_EmitInternal("text", Napi::String::New(m_env, refPayload.strBody));
		break;
	case WSJS_DATA:
		{
			Napi::Buffer<uint8_t> hData = refPayload.strBody.empty()
				? Napi::Buffer<uint8_t>::New(m_env, 0)
				: Napi::Buffer<uint8_t>::Copy(
					m_env, (const uint8_t *)refPayload.strBody.data(),
					refPayload.strBody.size());
			_EmitInternal("data", hData);
		}
		break;
	case WSJS_EXCEPTION:
		// 帧解析违规。随后一定会走 closed，所以这里**不是**终局，不解钉。
		_EmitInternal("exception", Napi::String::New(m_env, refPayload.strBody));
		break;
	case WSJS_CLOSED:
		{
			if (m_bClosedSeen)
			{
				break;
			}
			m_bClosedSeen = true;
			if (!m_bUpgraded)
			{
				// ⚠️ 握手没成功过就**不发 closed**（WSConnector.h 契约第 2 条：
				// 握手没成功，也就没有"连接关闭"这件事）。业务靠这条语义区分
				// "握手失败"与"连上之后被关掉"，多发一个 closed 会让重连逻辑
				// 走错分支。原生层本身不会在这种情况下发 OnClosed；能走到这里的
				// 只有连接器收尾时的兜底（OnJsClosed），包括"建了连接但从没
				// connect 过"这种最普通的形态。
				if (m_hConnectCallback.Value().IsFunction())
				{
					// connect 还悬着：以错误结束它，否则胶水层那个 Promise 永远
					// 不 settle。_SettleConnect 在失败支里会顺手解钉。
					WSEventPayload sFake;
					sFake.eEvent		= WSJS_CONNECT_RESULT;
					sFake.result		= BC_R_UNEXPECTEDEND;
					sFake.strErrMessage	=
						"连接在 WebSocket 握手完成前被关闭：" + refPayload.strBody;
					_SettleConnect(sFake);
				}
				else
				{
					_Unpin();
				}
				break;
			}
			if (!refPayload.strErrMessage.empty())
			{
				m_strLastError = refPayload.strErrMessage;
			}
			_EmitInternal("closed", Napi::String::New(m_env, refPayload.strBody));
			// 终局。放在最后：解钉之后本对象随时可能被 GC。
			_Unpin();
		}
		break;
	default:
		break;
	}
}

///////////////////////////////////////////////////////////////////////////////
// JsWSConnectionWrap —— JS 侧方法
///////////////////////////////////////////////////////////////////////////////

Napi::Value JsWSConnectionWrap::_Connect(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 3 || !info[0].IsString() || !info[1].IsNumber()
		|| !info[2].IsFunction())
	{
		THROW_ERROR_WITH_RESULT(env,
			"Invalid arguments: __connect__(url, timeoutMs, callback)",
			env.Undefined());
	}
	// 负数 / 超大值经 Uint32Value 会变成一个毫不相干的数字，当场拦住 —— 同
	// HttpConnector 处理 drainTimeoutMs 时踩过的那个坑。
	double dTimeout = info[1].As<Napi::Number>().DoubleValue();
	if (!(dTimeout >= 0) || dTimeout > 4294967295.0)
	{
		THROW_ERROR_WITH_RESULT(env,
			"Invalid timeoutMs: out of range (expected 0 .. 4294967295).",
			env.Undefined());
	}
	// ⚠️ 第二次 __connect__ 当场抛，**不是**默默换掉回调：换掉的话前一个回调
	// 永不触发，胶水层那个 Promise 就永远悬着（与 __close__ 的回调必须成列表
	// 是同一类问题）。原生层本身也会返回 BC_R_ALREADYRUNNING。
	if (m_bConnectCalled)
	{
		THROW_ERROR_WITH_RESULT(env,
			"__connect__ has already been called on this connection "
			"(one connection = one connect; create a new one to reconnect)",
			env.Undefined());
	}
	if (!m_pConn || !m_pOwner)
	{
		THROW_ERROR_WITH_RESULT(env,
			"This WsConnection is not attached to a connector "
			"(use connector.__createConnection__())", env.Undefined());
	}

	std::string strUrl = info[0].As<Napi::String>().Utf8Value();
	m_bConnectCalled = true;
	m_hConnectCallback.Reset(info[2].As<Napi::Function>(), 1);
	// ⚠️ 先钉住再发起：否则业务一放手（`(() => { c.__connect__(...) })()` 之类）
	// 就可能在结果回来之前被 GC 掉，回调丢失 -> Promise 永远不 settle。
	// 同步失败那条路也要钉住 —— 失败是**异步**投回去的（见下），事件到达前对象
	// 必须还在。
	_Pin();

	// 参数校验先用 WSConnection 自己的纯函数复核一遍，好把具体原因带给业务
	// （Connect() 只给错误码，不给文案）。
	WS::WSUrlParts sParts;
	std::string    strDetail;
	if (!WS::WSConnection::ParseUrl(strUrl, sParts, strDetail))
	{
		m_pOwner->PostConnectFailure(m_nConnId, BC_R_INVALIDARG,
									 "URL 解析失败：" + strDetail);
		return env.Undefined();
	}

	BCRESULT result = m_pConn->Connect(strUrl, (uint32_t)dTimeout);
	if (result != BC_R_SUCCESS)
	{
		// 契约第 1 条：Connect 返回非成功时保证不会有任何回调，所以这条连接的
		// 结局只能由我们自己投出去。
		m_pOwner->PostConnectFailure(m_nConnId, result,
									 _WSConnectFailureHint(result));
	}
	return env.Undefined();
}

Napi::Value JsWSConnectionWrap::_SendText(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 1 || !info[0].IsString())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments: __sendText__(text)",
								env.Undefined());
	}
	if (!m_pConn)
	{
		return Napi::Number::New(env, (double)BC_R_NOTCONNECTED);
	}
	// ⚠️ 返回的是**原样的 BCRESULT**（0 = 成功），不抛异常：发送失败是运行期
	// 常态（对端刚关、连接器已 close），抛异常会逼着业务给每一次 send 套
	// try/catch。参数类型写错才是用法错误，那种才抛。
	std::string strText = info[0].As<Napi::String>().Utf8Value();
	return Napi::Number::New(env, (double)m_pConn->SendText(strText));
}

Napi::Value JsWSConnectionWrap::_SendData(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 1 || !info[0].IsBuffer())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments: __sendData__(buffer)",
								env.Undefined());
	}
	if (!m_pConn)
	{
		return Napi::Number::New(env, (double)BC_R_NOTCONNECTED);
	}
	Napi::Buffer<uint8_t> hBuffer = info[0].As<Napi::Buffer<uint8_t> >();
	return Napi::Number::New(env, (double)
		m_pConn->SendData(hBuffer.Data(), hBuffer.Length()));
}

Napi::Value JsWSConnectionWrap::_SendPing(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (!m_pConn)
	{
		return Napi::Number::New(env, (double)BC_R_NOTCONNECTED);
	}
	return Napi::Number::New(env, (double)m_pConn->SendPing());
}

Napi::Value JsWSConnectionWrap::_Close(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (!m_pConn)
	{
		return env.Undefined();
	}
	// 可选的关闭原因码。给了非法值就按正常关闭处理，不抛 —— close 是收尾动作，
	// 不该因为参数挑剔而失败。
	BCRESULT result = BC_R_SUCCESS;
	if (info.Length() > 0 && info[0].IsNumber())
	{
		double dResult = info[0].As<Napi::Number>().DoubleValue();
		if (dResult >= 0 && dResult <= 4294967295.0)
		{
			result = (BCRESULT)dResult;
		}
	}
	// ⚠️ 幂等，而且**不打日志**（WSConnector.h 契约：Close / SendText / SendData
	// 在 OnLog 里调都是安全的）。真正的 closed 事件由原生层在通道收尾时发。
	m_pConn->Close(result);
	return env.Undefined();
}

Napi::Value JsWSConnectionWrap::_Info(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	Napi::Object hInfo = Napi::Object::New(env);
	hInfo.Set("id", Napi::Number::New(env, (double)m_nConnId));
	hInfo.Set("attached",   Napi::Boolean::New(env, m_pConn ? true : false));
	hInfo.Set("connecting", Napi::Boolean::New(env,
		m_bConnectCalled && !m_bConnectSettled));
	hInfo.Set("upgraded",   Napi::Boolean::New(env, m_bUpgraded));
	hInfo.Set("closed",     Napi::Boolean::New(env, m_bClosedSeen));

	// ⚠️ 形状必须**恒定**：诊断接口一旦关闭之后就少几个字段，JS 侧读到的是
	// undefined，排查的人会以为"策略没生效"。通道还在时报实时值，通道销毁之后
	// 报连上时抓的快照（原生的 PeerIp() 自己也会回落到它缓存的那一份）。
	std::string strPeerIp    = m_strPeerIp;
	uint32_t    nBoundIfIdx  = m_nBoundIfIndex;
	std::string strPinMethod = m_strPinMethod;
	if (m_pConn)
	{
		strPeerIp    = m_pConn->PeerIp();
		nBoundIfIdx  = m_pConn->BoundIfIndex();
		strPinMethod = m_pConn->PinMethod();
	}
	hInfo.Set("peerIp", Napi::String::New(env, strPeerIp));
	hInfo.Set("boundIfIndex", Napi::Number::New(env, (double)nBoundIfIdx));
	hInfo.Set("pinMethod", Napi::String::New(env, strPinMethod));
	// 原生给的失败现场描述（force-physical 排查真正要看的那段）。没有失败时是空串，
	// 字段本身**始终存在** —— 形状恒定，读到 undefined 的人才不会误判。
	hInfo.Set("lastError", Napi::String::New(env, m_strLastError));

	// 编译期有没有 path monitor。理由与 JsWSConnectorWrap::_Info 里那一段相同。
#if defined(TT_HAS_PATH_MONITOR)
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, true));
#else
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, false));
#endif
	return scope.Escape(hInfo);
}

///////////////////////////////////////////////////////////////////////////////
// class : JsWSConnectorWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference JsWSConnectorWrap::constructor_template;

JsWSConnectorWrap::JsWSConnectorWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsWSConnectorWrap>(info)
	, m_env(info.Env())
	, m_pConnector(NULL)
	// 顺序跟着头文件里的声明顺序走，否则 -Wreorder-ctor
	, m_ePolicy(TT_VPN_POLICY_UNSET)
	, m_nDrainTimeoutMs(0)
	, m_nNextConnId(0)
	, m_bLogEnabled(false)
	, m_bClosePending(false)
	, m_nPendingCloseRefs(0)
{
	if (!info.IsConstructCall())
	{
		THROW_ERROR_VOID(m_env,
			"Use the new operator to create new WsConnector objects");
	}
	if (info.Length() == 0 || !info[0].IsObject())
	{
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
				 "Failed to create WSConnector: result=%u (%s)",
				 (unsigned)result, ResultName(result).c_str());
		THROW_ERROR_VOID(m_env, szMsg);
	}
	// ⚠️ 这里**故意不做** JsSMPConnectorWrap 那样的 m_hSelf 永久自引用（那个写法
	// 是个环：引用不放 -> 析构不跑 -> 引用不放，SMP 的连接器实际上永久泄漏）。
	// 本连接器的存活由两件事保证：业务自己的引用，以及"每条连接 wrap 持一份
	// 连接器 JS 对象的强引用"（见头文件生命周期第 3 条）。两者都松手时才回收。
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsWSConnectorWrap::~JsWSConnectorWrap()
{
	_Cleanup();
}

void JsWSConnectorWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "WsConnector", {
		InstanceMethod<&JsWSConnectorWrap::_CreateConnection>("__createConnection__"),
		InstanceMethod<&JsWSConnectorWrap::_Close>("__close__"),
		InstanceMethod<&JsWSConnectorWrap::_Info>("__info__"),
		InstanceMethod<&JsWSConnectorWrap::_Stats>("__stats__"),
	});
	constructor_template.Reset(ctor);
	constructor_template.SuppressDestruct();
	exports.Set("WsConnector", ctor);
}

BCRESULT JsWSConnectorWrap::Create(Napi::Object config)
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

	// Runtime 必须活得比 WSConnector 久（WSConnector.h 契约第 6 条）。本模块从不
	// 调 Runtime::Destroy()，所以只要保证它已经初始化；已经初始化时
	// Runtime::Initialize 直接返回 BC_R_SUCCESS，不会覆盖已有线程配置。
	BCRESULT result = Runtime::Initialize(pConfig);
	if (result != BC_R_SUCCESS)
	{
		return result;
	}

	// 日志闸门要在 WSConnector::Create 之前打开，否则它自己那行"已创建"的 INFO
	// 会被挡掉。
	Napi::Value hLogCallback = config.Get("log_callback");
	if (hLogCallback.IsFunction())
	{
		m_hLogCallback.Reset(hLogCallback.As<Napi::Function>(), 1);
		m_bLogEnabled.store(true);
	}

	m_pConnector = new WS::WSConnector();
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
	m_ePolicy         = m_pConnector->EffectivePolicy();
	m_nDrainTimeoutMs = m_pConnector->DrainTimeoutMs();
	return BC_R_SUCCESS;
}

bool JsWSConnectorWrap::_ValidateConfig(Napi::Object config,
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

///////////////////////////////////////////////////////////////////////////////
// JsWSConnectorWrap —— JS 侧方法
///////////////////////////////////////////////////////////////////////////////

Napi::Value JsWSConnectorWrap::_CreateConnection(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (!m_pConnector)
	{
		THROW_ERROR_WITH_RESULT(env,
			"WsConnector is already closed", env.Undefined());
	}
	// pConfig 可选：不给就沿用连接器的默认配置，给了可逐键覆盖（连接级的键见
	// WSConnector.h 里 Create 的注释）。
	std::unique_ptr<BCFVar>	pVar;
	BCFObject			*	pConfig = NULL;
	if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull())
	{
		if (!info[0].IsObject())
		{
			THROW_ERROR_WITH_RESULT(env,
				"Invalid arguments: __createConnection__([config])",
				env.Undefined());
		}
		Napi::Object hConfig = info[0].As<Napi::Object>();
		std::string  strError;
		if (!_ValidateConfig(hConfig, strError))
		{
			THROW_ERROR_WITH_RESULT(env, strError.c_str(), env.Undefined());
		}
		pVar.reset(ConvertBCFFromJS(env, hConfig));
		if (!IS_BCF_OBJECT(pVar.get()))
		{
			THROW_ERROR_WITH_RESULT(env,
				"Invalid arguments: config must be a plain object",
				env.Undefined());
		}
		pConfig = (BCFObject *)pVar.get();
	}

	// 先把 JS 对象建出来：构造函数一旦抛异常，这里就没有任何原生资源需要回收。
	Napi::Object hConn = JsWSConnectionWrap::constructor_template.New({});
	if (hConn.IsEmpty() || env.IsExceptionPending())
	{
		return env.Undefined();
	}
	JsWSConnectionWrap *pWrap = JsWSConnectionWrap::Unwrap(hConn);
	if (!pWrap)
	{
		THROW_ERROR_WITH_RESULT(env,
			"Failed to create WsConnection object", env.Undefined());
	}

	// id 单调递增且跳过 0（0 当"无效 id"用）
	do
	{
		m_nNextConnId++;
	} while (m_nNextConnId == 0 || m_mapSinks.count(m_nNextConnId) > 0);
	const uint64_t nId = m_nNextConnId;

	// handler 是单独的 sink，不是 wrap 本身 —— 理由见头文件生命周期第 1 条。
	JsWSConnSink *pSink = new JsWSConnSink(this, nId);
	WSConnPtr     pConn = m_pConnector->CreateConnection(pConfig, pSink);
	if (!pConn)
	{
		// CreateConnection 失败时原生层不会记下这个 handler，就地释放是安全的。
		delete pSink;
		THROW_ERROR_WITH_RESULT(env,
			"WSConnector::CreateConnection failed (connector closing, "
			"or called from within log_callback)", env.Undefined());
	}
	pSink->m_wpConn = pConn;
	m_mapSinks[nId]     = pSink;
	m_mapConnWraps[nId] = pWrap;
	pWrap->Attach(this, nId, pConn, Value().As<Napi::Object>());

	return scope.Escape(hConn);
}

Napi::Value JsWSConnectorWrap::_Close(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() > 0 && info[0].IsFunction())
	{
		// ⚠️ 追加而不是覆盖。close() 是幂等的，但每一次调用都带着自己的回调；
		// 用单个引用去 Reset 会把前一个静默丢掉 —— 胶水层把 close 包成 Promise
		// 之后那就是"第一个 Promise 永远悬着"。第二次调用虽然被 m_bClosePending
		// 挡掉了其余动作，回调仍然必须登记进来。
		Napi::FunctionReference hRef;
		hRef.Reset(info[0].As<Napi::Function>(), 1);
		m_vecCloseCallbacks.push_back(std::move(hRef));
	}
	if (m_pConnector && !m_bClosePending)
	{
		m_bClosePending = true;
		// OnClosed 事件到达之前别让对象被回收 —— 它要在那时销毁连接器。
		m_nPendingCloseRefs++;
		Ref();
		// ⚠️ Close() 有可能**同步**回调 OnClosed（当下没有在途连接时，
		// WSConnector::Close 末尾的 _NotifyClosedIfDrained 会直接发）。我们的
		// OnClosed 只投递事件，所以这里不存在重入问题。
		m_pConnector->Close();
	}
	else if (!m_pConnector)
	{
		// 已经关完了。补投一次事件，免得调用方等一个永远不来的信号。走事件而不是
		// 直接调 OnJsClosed()，是为了保住"回调永远异步"这条语义。
		//
		// ⚠️ 这里**也要 Ref()**，与上面那支对称。少了它，调用方若在同一 tick 里
		// 丢掉最后一个 JS 引用，本对象就可能在事件被处理之前被回收：_Cleanup 会
		// Reset 掉 m_vecCloseCallbacks 并 RemoveEventByHandler 把刚投的 JWM_CLOSE
		// 丢掉 —— 回调永不触发，胶水层那个 close() 的 Promise 永远悬着。
		// 一次 Ref 对应一个 JWM_CLOSE 事件、对应一次 OnJsClosed，所以用计数配平
		// （幂等的第二次 close 也各自有自己的事件）。
		m_nPendingCloseRefs++;
		Ref();
		OnClosed();
	}
	return env.Undefined();
}

Napi::Value JsWSConnectorWrap::_Info(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	Napi::Object hInfo = Napi::Object::New(env);
	hInfo.Set("closed", Napi::Boolean::New(env, m_pConnector == NULL));
	// 还登记在事件路由表里的连接 wrap 数（不等于原生在途连接数，那个看 __stats__
	// 的 active_conn_size）
	hInfo.Set("connections",
			  Napi::Number::New(env, (double)m_mapConnWraps.size()));

	// ⚠️ 形状必须**恒定**：诊断接口一旦关闭之后就少几个字段，JS 侧读到的是
	// undefined，排查的人会以为"策略没生效"。关闭后沿用创建时解析出来的值
	// （连接器已销毁，但这几项在 Create 之后就不会再变）。
	hInfo.Set("vpnPolicy", Napi::String::New(env,
		tt_vpn_policy_to_string(m_ePolicy)));
	hInfo.Set("drainTimeoutMs",
			  Napi::Number::New(env, (double)m_nDrainTimeoutMs));

	// 编译期有没有 path monitor。**这一行是刻意加的诊断出口**：没有
	// TT_HAS_PATH_MONITOR 时 TcpChannel::_PickPhysicalIfIndex() 整段被条件编译
	// 掉、恒返回 0，于是 prefer-physical 静默回落系统路由（force-physical 不会
	// 静默 —— TcpChannel.cpp:414-425 的硬校验写在 guard 之外，会直接
	// return BC_R_NO_PHYSICAL_INTERFACE=64）。把它暴露到 JS，"构建配置悄悄把
	// 网卡绑定关掉"这一整类缺陷在**任何**跑得起来的产物上都会当场露馅 ——
	// 包括手搓编译命令编出来的东西，而 CMake 的 configure 期断言管不到那些。
#if defined(TT_HAS_PATH_MONITOR)
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, true));
#else
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, false));
#endif
	return scope.Escape(hInfo);
}

Napi::Value JsWSConnectorWrap::_Stats(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	// 连接器还在就取实时值并刷新缓存；已经销毁就报最后一次快照 —— 关闭之后正是
	// 最想看统计的时候（握手失败了几条、路由复核挡了几条）。
	if (m_pConnector)
	{
		ConnStatsMap mapStats;
		m_pConnector->GetStats(mapStats);
		m_mapLastStats = mapStats;
	}
	Napi::Object hStats = Napi::Object::New(env);
	for (ConnStatsMap::const_iterator iter = m_mapLastStats.begin();
		 iter != m_mapLastStats.end(); ++iter)
	{
		hStats.Set(iter->first, Napi::Number::New(env, (double)iter->second));
	}
	return scope.Escape(hStats);
}

///////////////////////////////////////////////////////////////////////////////
// 跨线程投递
///////////////////////////////////////////////////////////////////////////////

bool JsWSConnectorWrap::PostEventPayload(WSEventPayload *pPayload)
{
	if (!pPayload)
	{
		return false;
	}
	BCEventItemS sEvent(MAKEEVENT(JWM_CONN_EVENT, 0, 0),
						(uint64_t)pPayload, (uint64_t)0, &_EventDtorCB);
	BCRESULT result = JsExchanger::ExchangeEvent(sEvent, this);
	if (result == BC_R_SUCCESS || result == BC_R_IGNORE)
	{
		// SUCCESS：进队了，payload 由 _EventDtorCB 在事件收尾时释放。
		// IGNORE ：ExchangeEvent 已经调过 BCDefEventProc，也就是已经释放了。
		return (result == BC_R_SUCCESS);
	}
	// 交换器实例已经没了（模块卸载中）。事件没进队，cbDestroy 不会被调用，这里
	// 自己收尾，否则 payload 泄漏。
	//
	// 注：这条分支**实际不可达** —— JsExchanger 的实例在 RegisterModule 里创建
	// 之后没有任何 Destroy 路径，m_pInstance 恒非空。保留它只是为了不依赖那个
	// 事实。也因此没有必要在这里回退 m_internal_events_count：ExchangeEvent 在
	// 返回 FAILURE 的那条路径上压根没有 ++ 过它。
	delete pPayload;
	sEvent.wParam = 0;
	return false;
}

void JsWSConnectorWrap::PostConnectFailure(uint64_t nConnId, BCRESULT result,
											const std::string &refMessage)
{
	WSEventPayload *pPayload = new WSEventPayload();

	pPayload->nConnId		= nConnId;
	pPayload->eEvent		= WSJS_CONNECT_RESULT;
	pPayload->result		= result;
	pPayload->strErrMessage	= refMessage;

	if (!PostEventPayload(pPayload))
	{
		// 投递不出去（只可能发生在模块拆除阶段）。我们此刻就在 Node 主线程上，
		// 直接把这条连接的钉住解掉，别让它一直挂着。
		// ⚠️ 用 AbandonPendingConnect 而不是 OnOwnerGone：本连接器还活着，
		// OnOwnerGone 会把连接的回指指针置空，于是那条连接析构时不再来销账，
		// m_mapConnWraps 里就留下一个野指针 —— 下一个事件直接 UAF。
		ConnWrapMap::iterator iter = m_mapConnWraps.find(nConnId);
		if (iter != m_mapConnWraps.end())
		{
			iter->second->AbandonPendingConnect();
		}
	}
}

void JsWSConnectorWrap::_EventDtorCB(BCEventItemS &refEvent)
{
	// payload 的唯一释放点。事件正常处理完、被 RemoveEventByHandler 丢弃、或被
	// 交换器析构时 flush 掉，都会走到这里；置零让它幂等。
	if (EVENTMAJOR(refEvent.eType) == JWM_CONN_EVENT && refEvent.wParam)
	{
		delete (WSEventPayload *)refEvent.wParam;
		refEvent.wParam = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Node 主线程侧
///////////////////////////////////////////////////////////////////////////////

void JsWSConnectorWrap::OnConnectionWrapGone(uint64_t nConnId)
{
	// 连接 wrap 被 GC 了。sink **不删** —— 原生层可能还握着它当 handler_。
	// 它之后投来的事件在 OnJsConnEvent 里查不到 id，会被安静丢弃。
	m_mapConnWraps.erase(nConnId);
}

void JsWSConnectorWrap::OnJsConnEvent(WSEventPayload *pPayload)
{
	Napi::HandleScope scope(m_env);

	ConnWrapMap::iterator iter = m_mapConnWraps.find(pPayload->nConnId);
	if (iter == m_mapConnWraps.end())
	{
		// 对应的连接 wrap 已经被 GC（或连接器 wrap 已经把路由表清了），不再回调。
		return;
	}
	iter->second->OnJsEvent(*pPayload);
}

void JsWSConnectorWrap::OnJsLog(int level, LPCSTR lpszMsg)
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

void JsWSConnectorWrap::OnJsException(LPCSTR lpszMsg)
{
	Napi::HandleScope scope(m_env);

	if (m_hCallback.Value().IsFunction())
	{
		Napi::Function hCallback = m_hCallback.Value().As<Napi::Function>();
		Napi::Value    hSelf     = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		ArgsList argv = {
			Napi::String::New(m_env, "exception"),
			Napi::String::New(m_env, lpszMsg ? lpszMsg : "")
		};
		TRY_CATCH_CALL(m_env, hSelf, hCallback, argv);
		DrainPendingException(m_env);
	}
}

void JsWSConnectorWrap::_ReleaseNativeRefs()
{
	// ⚠️ **只能在 `delete m_pConnector` 之后调**。~WSConnector 会同步等在途连接
	// 收尾（超时则掐断全部回调并等在途回调跑完），它一返回，原生层就再也不可能
	// 碰任何 handler，sink 这时才能安全释放。
	for (SinkMap::iterator iter = m_mapSinks.begin();
		 iter != m_mapSinks.end(); ++iter)
	{
		iter->second->Detach();
		delete iter->second;
	}
	m_mapSinks.clear();

	// sink 没了 -> 原生 WSConnection 的 handler_ 成了野指针。让所有还登记着的连接
	// wrap 把 WSConnPtr 一起放手，把"stale WSConnection 持野 handler_"这一整类
	// 问题消掉（理由见 JsWSConnectionWrap::DetachNativeConn）。
	// 遍历快照：DetachNativeConn 只动被调对象自己的成员，不会回头改这张表，但
	// 拍个快照读起来更省心。
	std::vector<JsWSConnectionWrap *> vecWraps;
	for (ConnWrapMap::iterator iter = m_mapConnWraps.begin();
		 iter != m_mapConnWraps.end(); ++iter)
	{
		vecWraps.push_back(iter->second);
	}
	for (size_t i = 0; i < vecWraps.size(); i++)
	{
		vecWraps[i]->DetachNativeConn();
	}
}

void JsWSConnectorWrap::OnJsClosed()
{
	Napi::HandleScope scope(m_env);

	// 连接器已经收尾完毕，这里销毁它以释放日志 appender 等资源。
	// ⚠️ 这不是"在回调里析构"（WSConnector.h 契约第 8(4) 条）：我们是从
	// uv_async 回到 Node 主线程之后才做的，WS 那条回调栈早就返回了。也不持任何
	// 业务锁（契约 8(3)）。
	if (m_pConnector)
	{
		ConnStatsMap mapStats;
		m_pConnector->GetStats(mapStats);
		m_mapLastStats = mapStats;		// 关闭之后 __stats__ 还能报出来
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
	}
	// 连接器没了 -> sink 与各连接手里的 WSConnPtr 都可以放手了
	_ReleaseNativeRefs();

	// 把还登记着的连接强行推到终局。正常路径下每条连接在连接器 OnClosed 之前就
	// 已经收到过自己的 closed（_DeliverOnce 先发，销账的 Runtime 任务才发连接器
	// 级 OnClosed，两者按 FIFO 进同一个事件队列），所以这里一般是空转。真正需要
	// 它的是 ~WSConnector 的**放弃路径**：那条路上 closed 永远不会来，不兜的话
	// 钉住永不配平（连接 JS 对象泄漏）、Promise 永远悬着。
	//
	// 先整体搬出来再逐个处理：回调里完全可以再建连接 / 再 close，搬出来才不会在
	// 遍历过程中被搅乱。
	std::vector<JsWSConnectionWrap *> vecWraps;
	for (ConnWrapMap::iterator iter = m_mapConnWraps.begin();
		 iter != m_mapConnWraps.end(); ++iter)
	{
		vecWraps.push_back(iter->second);
	}
	for (size_t i = 0; i < vecWraps.size(); i++)
	{
		WSEventPayload sPayload;
		sPayload.eEvent		= WSJS_CLOSED;
		sPayload.nConnId	= vecWraps[i]->ConnId();
		sPayload.strBody	= "WSConnector 已关闭";
		// OnJsEvent 自己会挡掉"已经收过 closed"的情况，也会顺手把还悬着的
		// connect 回调以错误结束。
		vecWraps[i]->OnJsEvent(sPayload);
	}

	// 先整体搬出来再逐个触发：回调里完全可以再调一次 __close__()（那时
	// m_pConnector 已经是 NULL，会补投一个事件），搬出来才不会在遍历过程中被
	// 追加进来的元素搅乱。
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
	// 与 _Close 里的 Ref() 配平：一次 OnJsClosed 释放一次。放最后 —— Unref 之后
	// 本对象随时可能被 GC。
	m_bClosePending = false;
	if (m_nPendingCloseRefs > 0)
	{
		m_nPendingCloseRefs--;
		Unref();
	}
}

void JsWSConnectorWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);

	// 1) 先关掉日志转发。析构过程里还会产生日志，没必要再往 JS 投。
	m_bLogEnabled.store(false);

	// 2) 关连接器并等它收尾。
	//    ⚠️ 这里不持任何业务锁（Node 主线程就是业务线程，而我们从不在持锁状态下
	//    析构），也不在任何 WS 回调栈里 —— 满足 WSConnector.h 契约第 5 / 8(3) /
	//    8(4) 条。先 Close() 再 delete，把"等在途连接收尾"这段等待压到最短
	//    （否则默认 drainTimeoutMs=30s 有可能真的堵住主线程）。
	if (m_pConnector)
	{
		m_pConnector->Close();
		BC_SAFE_DELETE_PTR(m_pConnector);
	}

	// 3) 连接器没了，原生层再也不会碰任何 sink（~WSConnector 已同步等到在途回调
	//    跑完），可以安全释放；各连接手里的 WSConnPtr 同时放手。
	_ReleaseNativeRefs();

	// 4) 此后不会再有新事件被投递。把已经进队但还没跑的事件丢掉（payload 由
	//    _EventDtorCB 释放）。时机上是安全的：事件的处理与丢弃都发生在 Node
	//    主线程，也就是当前这条线程，不存在"正在处理"的并发副本。
	JsExchanger::RemoveEventByHandler(this);

	// 5) 断开还活着的连接 wrap。**这里不回调 JS** —— 析构可能发生在 env 拆除
	//    阶段，那时调用 JS 不安全（正常业务应当显式 __close__()：那条路径会把
	//    在途连接以 closed / connectResult(err) 结束，见 OnJsClosed）。
	//    正常 GC 下这个循环一定是空的：每条连接 wrap 都持着本对象的强引用，
	//    它们全没了本对象才可能被回收。
	//    ⚠️ 先拍快照再遍历：OnOwnerGone 里那次 Unref() 不会同步触发 GC（引用计数
	//    归零只是让对象**可回收**，finalize 排在后面的 GC 里），所以直接遍历也是
	//    安全的；拍快照是为了让这条推理不必成为读代码的前提。
	std::vector<JsWSConnectionWrap *> vecWraps;
	for (ConnWrapMap::iterator iter = m_mapConnWraps.begin();
		 iter != m_mapConnWraps.end(); ++iter)
	{
		vecWraps.push_back(iter->second);
	}
	m_mapConnWraps.clear();
	for (size_t i = 0; i < vecWraps.size(); i++)
	{
		vecWraps[i]->OnOwnerGone();
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

bool JsWSConnectorWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsWSConnectorWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JWM_CONN_EVENT:
		if (refEvent.wParam)
		{
			OnJsConnEvent((WSEventPayload *)refEvent.wParam);
		}
		break;
	case JWM_LOG_MSG:
		OnJsLog((int)refEvent.wParam, (LPCSTR)refEvent.lParam);
		break;
	case JWM_CLOSE:
		OnJsClosed();
		break;
	case JWM_EXCEPTION:
		OnJsException((LPCSTR)refEvent.lParam);
		break;
	default:
		break;
	}
	// 返回 true 让调用方走 BCDefEventProc，也就是调 cbDestroy 释放 payload。
	return true;
}

void JsWSConnectorWrap::OnExchangeShutdown()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// IWSConnectorHandler（任意线程）
///////////////////////////////////////////////////////////////////////////////

void JsWSConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	// ⚠️ 本回调是 BCLogger 持着它的**全局非递归自旋锁**调进来的。除了"把字符串
	// 拷走 + 投递事件"以外什么都不能做 —— 特别是不能回头调 WSConnector /
	// WSConnection 任何会打日志的 API（Create / CreateConnection / Connect /
	// 析构），那会重进那把非递归锁而 100% CPU 空转挂死。详见 WSConnector.h 里
	// 关于 OnLog 的那段。
	if (!m_bLogEnabled.load() || !lpszMsg)
	{
		return;
	}
	BCEventItemS sEvent(MAKEEVENT(JWM_LOG_MSG, 0, 0));
	sEvent.wParam = (uint64_t)level;
	sEvent.lParam = (uint64_t)sEvent.CopyString(lpszMsg);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsWSConnectorWrap::OnClosed()
{
	// 可能来自任意线程，也可能就在调用方的 Close() 栈里同步发出。只投递。
	BCEventItemS sEvent(MAKEEVENT(JWM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsWSConnectorWrap::OnException(BCException &refExcept)
{
	// 目前 WSConnector 不会调这个（连接级的异常走 IWSConnectionHandler），
	// 但接口要求实现，也就照样只投递。
	BCEventItemS sEvent(MAKEEVENT(JWM_EXCEPTION, 0, 0));
	sEvent.lParam = (uint64_t)sEvent.CopyString(refExcept.GetMsg().c_str());
	JsExchanger::ExchangeEvent(sEvent, this);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file : JsWSConnectorWrap.cpp
///////////////////////////////////////////////////////////////////////////////
