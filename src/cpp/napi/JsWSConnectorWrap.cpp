






#include "../StdAfx.h"
#include "../Runtime.h"
#include "../TTErrors.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsWSConnectorWrap.h"

#include <BC/Utils.h>

using namespace node;





enum
{
	JWM_CONN_EVENT	= 1,	
	JWM_LOG_MSG		= 2,	
	JWM_CLOSE		= 3,	
	JWM_EXCEPTION	= 4,	
};





namespace node
{


























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
		return "";		
	}
}



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





JsWSConnSink::JsWSConnSink(JsWSConnectorWrap *pOwner, uint64_t nConnId)
	: m_pOwner(pOwner)
	, m_nConnId(nConnId)
{
	
}

JsWSConnSink::~JsWSConnSink()
{
	
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

	
	
	
	
	
	WSConnPtr pConn = m_wpConn.lock();
	if (pConn)
	{
		pPayload->strPeerIp		= pConn->PeerIp();
		pPayload->nBoundIfIndex	= pConn->BoundIfIndex();
		pPayload->strPinMethod	= pConn->PinMethod();
		if (result != BC_R_SUCCESS)
		{
			
			
			
			
			
			
			
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





Napi::FunctionReference JsWSConnectionWrap::constructor_template;

JsWSConnectionWrap::JsWSConnectionWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsWSConnectionWrap>(info)
	, m_env(info.Env())
	
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
	
	
	
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsWSConnectionWrap::~JsWSConnectionWrap()
{
	
	
	if (m_pOwner)
	{
		
		
		m_pOwner->OnConnectionWrapGone(m_nConnId);
		m_pOwner = NULL;
	}
	
	m_bPinned = false;
	m_hCallback.Reset();
	m_hConnectCallback.Reset();
	m_hConnector.Reset();
	
	
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
	
	
	
	if (!hConnector.IsEmpty())
	{
		m_hConnector.Reset(hConnector, 1);
	}
}

void JsWSConnectionWrap::OnOwnerGone()
{
	
	
	m_pOwner = NULL;
	m_hConnectCallback.Reset();
	
	
	
	
	
	
	
	_Unpin();
	m_bClosedSeen = true;
	
	
	m_pConn.reset();
}

void JsWSConnectionWrap::AbandonPendingConnect()
{
	
	
	
	
	m_hConnectCallback.Reset();
	m_bConnectSettled = true;
	m_bClosedSeen     = true;	
	_Unpin();
}

void JsWSConnectionWrap::DetachNativeConn()
{
	
	
	
	
	
	
	
	
	
	
	
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
		
		Unref();
	}
}

void JsWSConnectionWrap::_EmitInternal(LPCSTR lpszType, Napi::Value hArg)
{
	Napi::HandleScope scope(m_env);

	if (!m_hCallback.Value().IsFunction())
	{
		return;		
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
		
		
		m_hConnectCallback.Reset();

		Napi::Value hSelf = Value();
		if (hSelf.IsEmpty())
		{
			hSelf = m_env.Undefined();
		}
		if (refPayload.result == BC_R_SUCCESS)
		{
			
			
			const napi_property_attributes eAttrs = (napi_property_attributes)
				(napi_writable | napi_enumerable | napi_configurable);
			Napi::Object hHeaders = Napi::Object::New(m_env);
			for (HttpHeaderMap::const_iterator iter = refPayload.mapHeaders.begin();
				 iter != refPayload.mapHeaders.end(); ++iter)
			{
				
				
				
				
				
				hHeaders.DefineProperty(Napi::PropertyDescriptor::Value(
					iter->first,
					Napi::String::New(m_env, iter->second),
					eAttrs));
			}
			Napi::Object hInfo = Napi::Object::New(m_env);
			hInfo.Set("headers", hHeaders);
			
			
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
			break;		
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
				
				
				
				
				
				
				if (m_hConnectCallback.Value().IsFunction())
				{
					
					
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
			
			_Unpin();
		}
		break;
	default:
		break;
	}
}





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
	
	
	double dTimeout = info[1].As<Napi::Number>().DoubleValue();
	if (!(dTimeout >= 0) || dTimeout > 4294967295.0)
	{
		THROW_ERROR_WITH_RESULT(env,
			"Invalid timeoutMs: out of range (expected 0 .. 4294967295).",
			env.Undefined());
	}
	
	
	
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
	
	
	
	
	_Pin();

	
	
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
	
	
	BCRESULT result = BC_R_SUCCESS;
	if (info.Length() > 0 && info[0].IsNumber())
	{
		double dResult = info[0].As<Napi::Number>().DoubleValue();
		if (dResult >= 0 && dResult <= 4294967295.0)
		{
			result = (BCRESULT)dResult;
		}
	}
	
	
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
	
	
	hInfo.Set("lastError", Napi::String::New(env, m_strLastError));

	
#if defined(TT_HAS_PATH_MONITOR)
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, true));
#else
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, false));
#endif
	return scope.Escape(hInfo);
}





Napi::FunctionReference JsWSConnectorWrap::constructor_template;

JsWSConnectorWrap::JsWSConnectorWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsWSConnectorWrap>(info)
	, m_env(info.Env())
	, m_pConnector(NULL)
	
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

	
	
	
	BCRESULT result = Runtime::Initialize(pConfig);
	if (result != BC_R_SUCCESS)
	{
		return result;
	}

	
	
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
	
	
	m_ePolicy         = m_pConnector->EffectivePolicy();
	m_nDrainTimeoutMs = m_pConnector->DrainTimeoutMs();
	return BC_R_SUCCESS;
}

bool JsWSConnectorWrap::_ValidateConfig(Napi::Object config,
										std::string &outError)
{
	Napi::HandleScope scope(m_env);

	
	
	
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





Napi::Value JsWSConnectorWrap::_CreateConnection(const Napi::CallbackInfo &info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (!m_pConnector)
	{
		THROW_ERROR_WITH_RESULT(env,
			"WsConnector is already closed", env.Undefined());
	}
	
	
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

	
	do
	{
		m_nNextConnId++;
	} while (m_nNextConnId == 0 || m_mapSinks.count(m_nNextConnId) > 0);
	const uint64_t nId = m_nNextConnId;

	
	JsWSConnSink *pSink = new JsWSConnSink(this, nId);
	WSConnPtr     pConn = m_pConnector->CreateConnection(pConfig, pSink);
	if (!pConn)
	{
		
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
		
		
		
		
		Napi::FunctionReference hRef;
		hRef.Reset(info[0].As<Napi::Function>(), 1);
		m_vecCloseCallbacks.push_back(std::move(hRef));
	}
	if (m_pConnector && !m_bClosePending)
	{
		m_bClosePending = true;
		
		m_nPendingCloseRefs++;
		Ref();
		
		
		
		m_pConnector->Close();
	}
	else if (!m_pConnector)
	{
		
		
		
		
		
		
		
		
		
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
	
	
	hInfo.Set("connections",
			  Napi::Number::New(env, (double)m_mapConnWraps.size()));

	
	
	
	hInfo.Set("vpnPolicy", Napi::String::New(env,
		tt_vpn_policy_to_string(m_ePolicy)));
	hInfo.Set("drainTimeoutMs",
			  Napi::Number::New(env, (double)m_nDrainTimeoutMs));

	
	
	
	
	
	
	
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
		
		
		return (result == BC_R_SUCCESS);
	}
	
	
	
	
	
	
	
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
		
		
		
		
		
		ConnWrapMap::iterator iter = m_mapConnWraps.find(nConnId);
		if (iter != m_mapConnWraps.end())
		{
			iter->second->AbandonPendingConnect();
		}
	}
}

void JsWSConnectorWrap::_EventDtorCB(BCEventItemS &refEvent)
{
	
	
	if (EVENTMAJOR(refEvent.eType) == JWM_CONN_EVENT && refEvent.wParam)
	{
		delete (WSEventPayload *)refEvent.wParam;
		refEvent.wParam = 0;
	}
}





void JsWSConnectorWrap::OnConnectionWrapGone(uint64_t nConnId)
{
	
	
	m_mapConnWraps.erase(nConnId);
}

void JsWSConnectorWrap::OnJsConnEvent(WSEventPayload *pPayload)
{
	Napi::HandleScope scope(m_env);

	ConnWrapMap::iterator iter = m_mapConnWraps.find(pPayload->nConnId);
	if (iter == m_mapConnWraps.end())
	{
		
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
	
	
	
	for (SinkMap::iterator iter = m_mapSinks.begin();
		 iter != m_mapSinks.end(); ++iter)
	{
		iter->second->Detach();
		delete iter->second;
	}
	m_mapSinks.clear();

	
	
	
	
	
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

	
	
	
	
	if (m_pConnector)
	{
		ConnStatsMap mapStats;
		m_pConnector->GetStats(mapStats);
		m_mapLastStats = mapStats;		
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
	}
	
	_ReleaseNativeRefs();

	
	
	
	
	
	
	
	
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
		
		
		vecWraps[i]->OnJsEvent(sPayload);
	}

	
	
	
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

	
	m_bLogEnabled.store(false);

	
	
	
	
	
	if (m_pConnector)
	{
		m_pConnector->Close();
		BC_SAFE_DELETE_PTR(m_pConnector);
	}

	
	
	_ReleaseNativeRefs();

	
	
	
	JsExchanger::RemoveEventByHandler(this);

	
	
	
	
	
	
	
	
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
	
	return true;
}

void JsWSConnectorWrap::OnExchangeShutdown()
{
	
}





void JsWSConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	
	
	
	
	
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
	
	BCEventItemS sEvent(MAKEEVENT(JWM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsWSConnectorWrap::OnException(BCException &refExcept)
{
	
	
	BCEventItemS sEvent(MAKEEVENT(JWM_EXCEPTION, 0, 0));
	sEvent.lParam = (uint64_t)sEvent.CopyString(refExcept.GetMsg().c_str());
	JsExchanger::ExchangeEvent(sEvent, this);
}





} 




