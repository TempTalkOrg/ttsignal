





#include "../StdAfx.h"
#include "../Runtime.h"
#include "../TTErrors.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsHttpConnectorWrap.h"

#include <BC/Utils.h>

using namespace node;





enum
{
	JHM_RESULT		= 1,	
	JHM_LOG_MSG		= 2,	
	JHM_CLOSE		= 3,	
};





namespace node
{












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





JsHttpRequestTask::JsHttpRequestTask(JsHttpConnectorWrap *pOwner, uint32_t nId)
	: m_nId(nId)
	, m_pOwner(pOwner)
{
	
}

JsHttpRequestTask::~JsHttpRequestTask()
{
	
}

void JsHttpRequestTask::OnHttpResponse(const HttpResponse &refResp)
{
	
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
	
	
	HttpResultPayload *pPayload = new HttpResultPayload();

	pPayload->nId			= m_nId;
	pPayload->bOk			= false;
	pPayload->result		= result;
	pPayload->strErrMessage	= refMessage;

	m_pOwner->PostResult(pPayload);
}





Napi::FunctionReference JsHttpConnectorWrap::constructor_template;

JsHttpConnectorWrap::JsHttpConnectorWrap(const Napi::CallbackInfo &info)
	: Napi::ObjectWrap<JsHttpConnectorWrap>(info)
	, m_env(info.Env())
	, m_pConnector(NULL)
	
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

	m_pConnector = new HttpConnector();
	result = m_pConnector->Create(pConfig, this);
	if (result != BC_R_SUCCESS)
	{
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
		m_hLogCallback.Reset();
		return result;
	}
	
	
	m_ePolicy           = m_pConnector->EffectivePolicy();
	m_nMaxResponseBytes = m_pConnector->MaxResponseBytes();
	m_nDrainTimeoutMs   = m_pConnector->DrainTimeoutMs();
	m_nMaxIdleConnections = (uint32_t)m_pConnector->MaxIdleConnections();
	m_nIdleTimeoutMs    = m_pConnector->IdleTimeoutMs();
	return BC_R_SUCCESS;
}





bool JsHttpConnectorWrap::_ValidateConfig(Napi::Object config,
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
				continue;	
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
		
		
		THROW_ERROR_WITH_RESULT(env, strError.c_str(), env.Undefined());
	}

	
	do
	{
		m_nNextTaskId++;
	} while (m_nNextTaskId == 0 || m_mapTasks.count(m_nNextTaskId) > 0);
	uint32_t nId = m_nNextTaskId;

	JsHttpRequestTask *pTask = new JsHttpRequestTask(this, nId);
	pTask->m_hCallback.Reset(hCallback, 1);
	m_mapTasks[nId] = pTask;
	
	
	Ref();

	
	
	
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
		
		
		
		
		
		Napi::FunctionReference hRef;
		hRef.Reset(info[0].As<Napi::Function>(), 1);
		m_vecCloseCallbacks.push_back(std::move(hRef));
	}
	if (m_pConnector && !m_bClosePending)
	{
		m_bClosePending = true;
		
		Ref();
		
		
		
		m_pConnector->Close();
	}
	else if (!m_pConnector)
	{
		
		
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

	
	
	
	hInfo.Set("vpnPolicy", Napi::String::New(env,
		tt_vpn_policy_to_string(m_ePolicy)));
	hInfo.Set("maxResponseBytes",
			  Napi::Number::New(env, (double)m_nMaxResponseBytes));
	hInfo.Set("drainTimeoutMs",
			  Napi::Number::New(env, (double)m_nDrainTimeoutMs));
	hInfo.Set("maxIdleConnections",
			  Napi::Number::New(env, (double)m_nMaxIdleConnections));
	
	
	if (m_pConnector)
	{
		m_nIdleTimeoutMs = m_pConnector->IdleTimeoutMs();
	}
	hInfo.Set("idleTimeoutMs",
			  Napi::Number::New(env, (double)m_nIdleTimeoutMs));

	
	
	
	
	
	
	
	
#if defined(TT_HAS_PATH_MONITOR)
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, true));
#else
	hInfo.Set("hasPathMonitor", Napi::Boolean::New(env, false));
#endif
	return scope.Escape(hInfo);
}





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
		
		
		return (result == BC_R_SUCCESS);
	}
	
	
	
	
	
	
	
	
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
	
	
	if (EVENTMAJOR(refEvent.eType) == JHM_RESULT && refEvent.wParam)
	{
		delete (HttpResultPayload *)refEvent.wParam;
		refEvent.wParam = 0;
	}
}





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
		
		LPCSTR lpszText = bc_result2string(refPayload.result);
		strMessage += lpszText ? lpszText : "unknown error";
	}
	Napi::Error  hError  = Napi::Error::New(m_env, strMessage);
	Napi::Object hObject = hError.Value();
	
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
	
	
	hResponse.Set("reason", Napi::String::New(m_env, refPayload.strReason));

	
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
	hResponse.Set("headers", hHeaders);

	Napi::Buffer<uint8_t> hBody = refPayload.strBody.empty()
		? Napi::Buffer<uint8_t>::New(m_env, 0)
		: Napi::Buffer<uint8_t>::Copy(m_env,
									  (const uint8_t *)refPayload.strBody.data(),
									  refPayload.strBody.size());
	hResponse.Set("body", hBody);

	
	
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
		
		return;
	}
	JsHttpRequestTask *pTask = iter->second;
	
	
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

	
	
	
	_SettlePendingTasks(BC_R_SHUTTINGDOWN,
						"HttpConnector 已关闭，在途请求被取消");

	
	
	
	if (m_pConnector)
	{
		m_bLogEnabled.store(false);
		BC_SAFE_DELETE_PTR(m_pConnector);
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
	if (m_bClosePending)
	{
		m_bClosePending = false;
		Unref();		
	}
}

void JsHttpConnectorWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);

	
	m_bLogEnabled.store(false);

	
	
	
	
	
	if (m_pConnector)
	{
		m_pConnector->Close();
		BC_SAFE_DELETE_PTR(m_pConnector);
	}

	
	
	
	
	JsExchanger::RemoveEventByHandler(this);

	
	
	
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
	
	return true;
}

void JsHttpConnectorWrap::OnExchangeShutdown()
{
	
}





void JsHttpConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	
	
	
	
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
	
	BCEventItemS sEvent(MAKEEVENT(JHM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}





} 




