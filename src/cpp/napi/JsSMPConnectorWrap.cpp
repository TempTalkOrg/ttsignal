///////////////////////////////////////////////////////////////////////////////
// file : JsSMPConnectorWrap.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "../Runtime.h"
#include "../SMPConnector.h"
#include "../http-parser/http_parser.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsSMPConnectorWrap.h"

using namespace node;


#define MY_TLS_GROUPS "X25519:P-384:P-521"

#define DEFAULT_LOG_FILE "ttc.log"

enum
{
	JCM_HSK_FINISH			= 1,
	JCM_EXEC_DONE			= 2,
	JCM_STREAM_CREATED		= 3,
	JCM_STREAM_CLOSED		= 4,
	JCM_RECV_CMD			= 5,
	JCM_RECV_DATA			= 6,
	JCM_RESTART				= 7,
	JCM_CLOSE				= 8,
	JCM_LOG_MSG				= 9,
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// class : JsSMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference	JsSMPConnectorWrap::constructor_template;

JsSMPConnectorWrap::JsSMPConnectorWrap(Napi::CallbackInfo const& info)
	: Napi::ObjectWrap<JsSMPConnectorWrap>(info)
	, m_env(info.Env())
	, m_pConnector(NULL)
{
	if (!info.IsConstructCall()) {
		THROW_ERROR_VOID(m_env, "Use the new operator to create new SMPConnector objects");
	}
	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_VOID(m_env, "Invalid config arguments");
	}
	Napi::Object lConfig = info[0].As<Napi::Object>();
	BCRESULT result = Create(lConfig);
	if (result != BC_R_SUCCESS)
	{
		THROW_ERROR_VOID(m_env, "Failed to create SMPConnector, check the arguments.");
	}
	m_hSelf.Reset(info.This().As<Napi::Object>(), 1);
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsSMPConnectorWrap::~JsSMPConnectorWrap()
{
	_Cleanup();
}

void JsSMPConnectorWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);
	
	Napi::Function ctor = DefineClass(env, "Connector", {
        InstanceMethod<&JsSMPConnectorWrap::_CreateConnection>("__createConnection__"),
        InstanceMethod<&JsSMPConnectorWrap::_Close>("__close__"),
    });
    constructor_template.Reset(ctor);
    constructor_template.SuppressDestruct();
    exports.Set("Connector", ctor);
}

BCRESULT JsSMPConnectorWrap::Create(Napi::Object config)
{
	Napi::HandleScope scope(m_env);

	if (!m_pConnector)
	{
		m_pConnector = new SMPConnector();
		if (!m_pConnector)
		{
			return BC_R_NOMEMORY;
		}
		std::unique_ptr<BCFVar> pVar(ConvertBCFFromJS(m_env, config));
		if (!IS_BCF_OBJECT(pVar))
		{
			THROW_ERROR_WITH_RESULT(m_env, "Invalid arguments.", BC_R_INVALIDARG);
		}
		BCRESULT result;
		BCFObject sockConfig, *pConfig = (BCFObject*)pVar.get();

		//int server_port = 8003;
		char c_cong_ctl = 'B';
		char c_log_level = 'e';
		int pacing_on = 0;

		Runtime::Initialize(pConfig);
		sockConfig.PutBool("ipv6", 0);
		sockConfig.PutString("server_host", "example.com");
		//sockConfig.PutInt("port", server_port);
		/* client does not need to fill in private_key_file & cert_file */
		sockConfig.PutString("tls_ciphers", XQC_TLS_CIPHERS);
		sockConfig.PutString("tls_groups", MY_TLS_GROUPS);
		sockConfig.PutInt("pacing_on", pacing_on);
		sockConfig.PutInt("log_level", c_log_level);
		sockConfig.PutInt("c_cong_ctl", c_cong_ctl);
		sockConfig += *pConfig;
		Napi::Value callback = config.Get("log_callback");
		if (callback.IsFunction())
		{
			m_hLogCallback.Reset(callback.As<Napi::Function>(), 1);
		}
		result = m_pConnector->Create(&sockConfig, this);
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(m_pConnector);
			return result;
		}
		return BC_R_SUCCESS;
	}
	return BC_R_EXISTS;
}

Napi::Value JsSMPConnectorWrap::_CreateConnection(const Napi::CallbackInfo& info) 
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);
	if (info.Length() < 1 || !info[0].IsObject())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments.", env.Undefined());
	}
	BCFVar* pConfig = ConvertBCFFromJS(env, info[0]);
	if (!IS_BCF_OBJECT(pConfig))
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments.", env.Undefined());
	}
	if (m_pConnector && !Value().IsEmpty())
	{
		Napi::Object hConn = JsSMPConnectionWrap::New(env, info[0].As<Napi::Object>());
		JsSMPConnectionWrap* pConnWrap = JsSMPConnectionWrap::Unwrap(hConn);
		SMPConnection* pConn = m_pConnector->CreateConnection(pConnWrap, (BCFObject*)pConfig);
		pConnWrap->SetConnection(pConn);
		return scope.Escape(hConn);
	}
	return env.Undefined();
}

Napi::Value JsSMPConnectorWrap::_Close(const Napi::CallbackInfo& info)
{
	if (m_pConnector)
	{
		m_pConnector->Close();
	}
	return info.Env().Undefined();
}

void JsSMPConnectorWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);
	BC_SAFE_DELETE_PTR(m_pConnector);
	m_sLocalStubMgr.Clear();
	m_hSelf.Reset();
	m_hCallback.Reset();
}

void JsSMPConnectorWrap::_EventDtorCB(BCEventItemS &refEvent)
{

}

void JsSMPConnectorWrap::OnJsExecDone(RPCStub *pStub)
{
	Napi::HandleScope scope(m_env);
}

void JsSMPConnectorWrap::OnJsLog(int level, LPCSTR lpszMsg)
{
	Napi::HandleScope scope(m_env);
	if (m_hLogCallback.Value().IsFunction())
	{
		Napi::Function callback = m_hLogCallback.Value().As<Napi::Function>();
		ArgsList argv = { 
			Napi::Number::New(m_env, level), 
			Napi::String::New(m_env, lpszMsg)
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectorWrap::OnJsClose()
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = { Napi::String::New(m_env, close_sym) };
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
	_Cleanup();
}

bool JsSMPConnectorWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsSMPConnectorWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JCM_EXEC_DONE:
		if (refEvent.wParam)
		{
			RPCStub *pStub = (RPCStub *)refEvent.wParam;
			OnJsExecDone(pStub);
		}
		break;
	case JCM_LOG_MSG:
		OnJsLog(refEvent.wParam, (LPCSTR)refEvent.lParam);
		break;
	case JCM_CLOSE:
		OnJsClose();
		break;
	default:
		BCDefEventProc(refEvent);
		break;
	}
	return true;
}

void JsSMPConnectorWrap::OnExchangeShutdown()
{

}

void JsSMPConnectorWrap::OnExecDone(IRPCStub *pStub)
{
	RPCStub* pRPCStub = static_cast<RPCStub*>(pStub);
	BCEventItemS sEvent(MAKEEVENT(JCM_EXEC_DONE, 0, 0), pRPCStub);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectorWrap::OnClosed()
{
	BCEventItemS sEvent(MAKEEVENT(JCM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectorWrap::OnException(BCException &except)
{
	//
}

void JsSMPConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_LOG_MSG, 0, 0));
	sEvent.wParam = level;
	sEvent.lParam = (uint64_t)sEvent.CopyString(lpszMsg);
	JsExchanger::ExchangeEvent(sEvent, this);
}

///////////////////////////////////////////////////////////////////////////////
// class : JsSMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference JsSMPConnectionWrap::constructor_template;

JsSMPConnectionWrap::JsSMPConnectionWrap(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<JsSMPConnectionWrap>(info)
	, m_env(info.Env())
	, m_pConn(NULL)
{
	if (!info.IsConstructCall()) {
		THROW_ERROR_VOID(m_env, "Use the new operator to create new Connection objects");
	}
	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_VOID(m_env, "Invalid config arguments");
	}
	Napi::Object lConfig = info[0].As<Napi::Object>();
	BCRESULT result = Create(lConfig);
	if (result != BC_R_SUCCESS)
	{
		THROW_ERROR_VOID(m_env, "Failed to create Connection, check the arguments.");
	}
	m_hSelf.Reset(info.This().As<Napi::Object>(), 1);
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsSMPConnectionWrap::~JsSMPConnectionWrap()
{
	_Cleanup();
}

void JsSMPConnectionWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "Connection", {
        InstanceMethod<&JsSMPConnectionWrap::_Connect>("__connect__"),
        InstanceMethod<&JsSMPConnectionWrap::_SendPacket>("__sendPacket__"),
        InstanceMethod<&JsSMPConnectionWrap::_Restart>("__restart__"),
        InstanceMethod<&JsSMPConnectionWrap::_Close>("__close__"),
        InstanceMethod<&JsSMPConnectionWrap::_CloseStream>("__closeStream__"),
    });
    constructor_template.Reset(ctor);
    constructor_template.SuppressDestruct();
    exports.Set("Connection", ctor);
}

Napi::Object JsSMPConnectionWrap::New(Napi::Env env, Napi::Object hConfig)
{
	Napi::EscapableHandleScope scope(env);

	Napi::Object hConn = constructor_template.New({ hConfig });

	return scope.Escape(hConn).As<Napi::Object>();
}

BCRESULT JsSMPConnectionWrap::Create(Napi::Object config)
{
	Napi::HandleScope scope(m_env);

	return BC_R_SUCCESS;
}

void JsSMPConnectionWrap::SetConnection(SMPConnection* pConn)
{
	m_pConn = pConn;
}

Napi::Value JsSMPConnectionWrap::_Connect(const Napi::CallbackInfo& info) 
{
	Napi::Env env = info.Env();
	if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString()
		|| !info[2].IsNumber() || !info[3].IsFunction())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments.", env.Undefined());
	}
	std::string url_str = EncodeURI(info[0].As<Napi::String>().Utf8Value());
	    // 1. Parse URL
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url_str.c_str(), url_str.length(), 0, &u) != 0) {
        THROW_ERROR_WITH_RESULT(env, "Invalid URL.", env.Undefined());
    }
	std::string scheme = u.field_set & (1 << UF_SCHEMA) ? url_str.substr(u.field_data[UF_SCHEMA].off, u.field_data[UF_SCHEMA].len) : "";
    std::string host = u.field_set & (1 << UF_HOST) ? url_str.substr(u.field_data[UF_HOST].off, u.field_data[UF_HOST].len) : "";
    uint16_t port = u.field_set & (1 << UF_PORT) ? u.port : (scheme == "https" ? 443 : 80);
	std::string path = u.field_set & (1 << UF_PATH) ? url_str.substr(u.field_data[UF_PATH].off, u.field_data[UF_PATH].len) : "";
	std::string query = u.field_set & (1 << UF_QUERY) ? url_str.substr(u.field_data[UF_QUERY].off, u.field_data[UF_QUERY].len) : "";

	std::string propStr(info[1].As<Napi::String>().Utf8Value());
	if (port <= 0)
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid port.", env.Undefined());
	}

	if (m_pConn && !Value().IsEmpty())
	{
		BCRESULT result;
		uint32_t timeoutInMs = info[2].As<Napi::Number>().Uint32Value();
		Napi::Function callback = info[3].As<Napi::Function>();
		RPCStub *pStub = m_sLocalStubMgr.GetStub();
		if (!pStub) {
			ERROR_CALL(env, Value(), callback, kENOMEM.Ptr);
		}
		else {
			if (BC_R_SUCCESS != pStub->Create(kConnect.Ptr, callback)) {
				m_sLocalStubMgr.PutStub(pStub->m_nTransId);
				ERROR_CALL(env, Value(), callback, kEINITSTUB.Ptr);
			}
			else {
				pStub->m_lParams[0] = (uint64_t)pStub->m_sPool.Strdup(scheme.c_str());
				pStub->m_lParams[1] = (uint64_t)pStub->m_sPool.Strdup(host.c_str());
				pStub->m_lParams[2] = port;
				pStub->m_lParams[3] = (uint64_t)pStub->m_sPool.Strdup(path.c_str());
				pStub->m_lParams[4] = (uint64_t)pStub->m_sPool.Strdup(query.c_str());
				pStub->m_lParams[5] = (uint64_t)pStub->m_sPool.Strdup(propStr.c_str());
				pStub->m_lParams[6] = timeoutInMs;
				result = m_pConn->Connect(pStub);
				if (result != BC_R_SUCCESS) {
					ERROR_CALL(env, Value(), callback, kEINTERNAL.Ptr);
				}
				return env.Null();
			}
		}
	}
	return env.Null();
}

Napi::Value JsSMPConnectionWrap::_SendPacket(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsNumber()
		|| !info[2].IsNumber() || !info[3].IsNumber() || !info[4].IsBuffer())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid arguments.", env.Undefined());
	}
	if (m_pConn)
	{
		SMPacketPtr pPacket(new SMPacket());
		pPacket->type = (SMP_PTYPE)info[0].As<Napi::Number>().Uint32Value();
		pPacket->timestamp = info[1].As<Napi::Number>().Uint32Value();
		pPacket->trans_id = info[2].As<Napi::Number>().Uint32Value();
		pPacket->stream_id = info[3].As<Napi::Number>().Uint32Value();
		Napi::Buffer<uint8_t> buffer = info[4].As<Napi::Buffer<uint8_t>>();
		pPacket->Write(buffer.Data(), buffer.Length());
		m_pConn->SendPacket(pPacket);
	}
	return env.Null();
}

Napi::Value JsSMPConnectionWrap::_Restart(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (m_pConn)
	{
		m_pConn->Restart();
	}
	return info.Env().Undefined();
}

Napi::Value JsSMPConnectionWrap::_Close(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (m_pConn)
	{
		m_pConn->Close();
	}
	return info.Env().Undefined();
}

Napi::Value JsSMPConnectionWrap::_CloseStream(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 1 || !info[0].IsNumber())
	{
		THROW_ERROR_WITH_RESULT(env, "Invalid stream id.", env.Undefined());
	}
	if (m_pConn)
	{
		m_pConn->CloseStream(info[0].As<Napi::Number>().Uint32Value());
	}
	return info.Env().Undefined();
}

void JsSMPConnectionWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);
	BC_SAFE_DELETE_PTR(m_pConn);
	m_sLocalStubMgr.Clear();
	m_hSelf.Reset();
	m_hCallback.Reset();
}

void JsSMPConnectionWrap::_EventDtorCB(BCEventItemS &refEvent)
{

}

void JsSMPConnectionWrap::OnJsHandshakeFinished()
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = { Napi::String::New(m_env, handshake_finished_sym) };
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectionWrap::OnJsExecDone(RPCStub *pStub)
{
	Napi::HandleScope scope(m_env);

	if (pStub) 
	{
		if (kConnect.Equal(pStub->m_szCmd))
		{
			napi_value error = m_env.Null(), response = m_env.Null();
			if (pStub->m_result != BC_R_SUCCESS)
			{
				error = Napi::Error::New(m_env, bc_result2string(pStub->m_result)).Value();
			}
			if (pStub->m_lParams[0] && pStub->m_lParams[1])
			{
				response = Napi::String::New(m_env, (const char*)pStub->m_lParams[0], pStub->m_lParams[1]);
			}
			ArgsList argv = { 
				error,
				response
			};
			TRY_CATCH_CALL(m_env, Value(), pStub->m_fCallback, argv);
		}
		m_sLocalStubMgr.PutStub(pStub->m_nTransId, FALSE);
	}
}

void JsSMPConnectionWrap::OnJsStreamCreated(uint32_t nStreamId)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = {
			Napi::String::New(m_env, stream_created_sym),
			Napi::Number::New(m_env, nStreamId)
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectionWrap::OnJsStreamClosed(uint32_t nStreamId)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = {
			Napi::String::New(m_env, stream_closed_sym),
			Napi::Number::New(m_env, nStreamId)
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectionWrap::OnJsRecvCmd(uint32_t nStreamId, LPCSTR lpszCmd, size_t len)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = {
			Napi::String::New(m_env, command_sym),
			Napi::Number::New(m_env, nStreamId),
			Napi::String::New(m_env, lpszCmd, len)
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectionWrap::OnJsRecvData(uint32_t nStreamId, LPCVOID data, size_t len)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		Napi::Buffer<uint8_t> outBuffer = Napi::Buffer<uint8_t>::Copy(m_env, (uint8_t *)data, len);
		ArgsList argv = {
			Napi::String::New(m_env, data_sym),
			Napi::Number::New(m_env, nStreamId),
			outBuffer
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPConnectionWrap::OnJsRestart(BCRESULT result, const char *lpszAddr)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		napi_value error = m_env.Null(), response = m_env.Null();
		if (result != BC_R_SUCCESS)
		{
			error = Napi::Error::New(m_env, bc_result2string(result)).Value();
		}
		if (lpszAddr)
		{
			response = Napi::String::New(m_env, lpszAddr);
		}
		ArgsList argv = { 
			Napi::String::New(m_env, restart_sym),
			error,
			response
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
    }
}

void JsSMPConnectionWrap::OnJsClosed(LPCSTR strReason)
{
	Napi::HandleScope scope(m_env);

	BC_SAFE_DELETE_PTR(m_pConn);
    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		ArgsList argv = {
			Napi::String::New(m_env, close_sym),
			Napi::String::New(m_env, strReason)
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
	_Cleanup();
}

bool JsSMPConnectionWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsSMPConnectionWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JCM_HSK_FINISH:
		OnJsHandshakeFinished();
		break;
	case JCM_EXEC_DONE:
		if (refEvent.wParam)
		{
			RPCStub *pStub = (RPCStub *)refEvent.wParam;
			OnJsExecDone(pStub);
		}
		break;
	case JCM_STREAM_CREATED:
		OnJsStreamCreated(refEvent.wParam);
		break;
	case JCM_STREAM_CLOSED:
		OnJsStreamClosed(refEvent.wParam);
		break;
	case JCM_RECV_CMD:
		if (refEvent.wParam)
		{
			LPCSTR lpszCmd = (LPCSTR)refEvent.wParam;
			OnJsRecvCmd(refEvent.vParams[0], lpszCmd, refEvent.lParam);
		}
		break;
	case JCM_RECV_DATA:
		if (refEvent.wParam)
		{
			LPCSTR lpszMsg = (LPCSTR)refEvent.wParam;
			OnJsRecvData(refEvent.vParams[0], lpszMsg, refEvent.lParam);
		}
		break;
	case JCM_RESTART:
		OnJsRestart(refEvent.wParam, (LPCSTR)refEvent.lParam);
		break;
	case JCM_CLOSE:
		if (refEvent.wParam)
		{
			LPCSTR lpszReason = (LPCSTR)refEvent.wParam;
			OnJsClosed(lpszReason);
		}
		break;
	default:
		BCDefEventProc(refEvent);
		break;
	}
	return true;
}

void JsSMPConnectionWrap::OnExchangeShutdown()
{

}

void JsSMPConnectionWrap::OnHandshakeFinished()
{
	BCEventItemS sEvent(MAKEEVENT(JCM_HSK_FINISH, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnExecDone(IRPCStub *pStub)
{
	RPCStub* pRPCStub = static_cast<RPCStub*>(pStub);
	BCEventItemS sEvent(MAKEEVENT(JCM_EXEC_DONE, 0, 0), pRPCStub);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnStreamCreated(uint32_t nStreamId)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_STREAM_CREATED, 0, 0), nStreamId);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnStreamClosed(uint32_t nStreamId)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_STREAM_CLOSED, 0, 0), nStreamId);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnRecvCmd(const SMPHeader &refHeader, LPCSTR lpszCmd, size_t msg_size)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_RECV_CMD, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(lpszCmd, msg_size);
	sEvent.lParam = msg_size;
	sEvent.vParams[0] = refHeader.m_nStreamId;
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnRecvData(const SMPHeader &refHeader, LPCVOID data, size_t msg_size)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_RECV_DATA, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyBuffer(data, msg_size);
	sEvent.lParam = msg_size;
	sEvent.vParams[0] = refHeader.m_nStreamId;
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnRestart(BCRESULT result, const char *lpszAddr)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_RESTART, 0, 0));
	sEvent.wParam = (uint64_t)result;
	if (lpszAddr)
	{
		sEvent.lParam = (uint64_t)sEvent.CopyString(lpszAddr, strlen(lpszAddr));
	}
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnClosed(LPCSTR strReason)
{
	BCEventItemS sEvent(MAKEEVENT(JCM_CLOSE, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(strReason, strlen(strReason));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPConnectionWrap::OnException(BCException &except)
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

// NS_CC_END