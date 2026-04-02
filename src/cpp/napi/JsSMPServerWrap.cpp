///////////////////////////////////////////////////////////////////////////////
// file : JsSMPServerWrap.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "../SMPServer.h"
#include "../Runtime.h"
#include "JsExchanger.h"
#include "Utils.h"
#include "JsSMPServerWrap.h"

using namespace node;


#define MY_TLS_GROUPS "X25519:P-384:P-521"

#define DEFAULT_LOG_FILE "tts.log"


enum
{
	JSM_HSK_FINISH			= 1,
	JSM_NEW_CONN			= 2,
	JSM_ACCEPT				= 3,
	JSM_EXEC_DONE			= 4,
	JSM_CONNECT				= 5,
	JSM_RECV_CMD			= 6,
	JSM_RECV_DATA			= 7,
	JSM_CLOSE				= 8,
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// class : JsSMPServerWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference JsSMPServerWrap::constructor_template;

JsSMPServerWrap::JsSMPServerWrap(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<JsSMPServerWrap>(info)
	, m_env(info.Env())
	, m_pServer(NULL)
{
	if (!info.IsConstructCall()) {
		THROW_ERROR_VOID(m_env, "Use the new operator to create new Server objects");
	}
	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_VOID(m_env, "Invalid config arguments");
	}
	Napi::Object lConfig = info[0].As<Napi::Object>();
	BCRESULT result = Create(lConfig);
	if (result != BC_R_SUCCESS)
	{
		BCPString strError;
		strError.Format("Failed to create SMPServer, check the arguments(%s).", bc_result2string(result));
		THROW_ERROR_VOID(m_env, strError.c_str());
	}
	m_hSelf.Reset(info.This().As<Napi::Object>(), 1);
	Napi::Value callback = GetPrototypeProperty(internalCallback_sym);
	if (callback.IsFunction())
	{
		m_hCallback.Reset(callback.As<Napi::Function>(), 1);
	}
}

JsSMPServerWrap::~JsSMPServerWrap()
{
	_Cleanup();
}

void JsSMPServerWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "Server", {
        InstanceMethod<&JsSMPServerWrap::_Start>("__start__"),
        InstanceMethod<&JsSMPServerWrap::_Close>("__close__"),
    });
    constructor_template.Reset(ctor);
    constructor_template.SuppressDestruct();
    exports.Set("Server", ctor);
}

BCRESULT JsSMPServerWrap::Create(Napi::Object config)
{
	Napi::HandleScope scope(m_env);

	if (!m_pServer)
	{
		m_pServer = new SMPServer();
		if (!m_pServer)
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

		char c_cong_ctl = 'B';
		char c_log_level = 'e';
		int pacing_on = 0;

		Runtime::Initialize(pConfig);
		sockConfig.PutBool("ipv6", 0);
		sockConfig.PutString("server_host", "example.com");
		//sockConfig.PutInt("port", server_port);
		//sockConfig.PutString("private_key_file", "./cert/server.key");
		//sockConfig.PutString("cert_file", "./cert/server.crt");
		sockConfig.PutString("tls_ciphers", XQC_TLS_CIPHERS);
		sockConfig.PutString("tls_groups", MY_TLS_GROUPS);
		sockConfig.PutInt("pacing_on", pacing_on);
		sockConfig.PutInt("log_level", c_log_level);
		sockConfig.PutInt("c_cong_ctl", c_cong_ctl);
		sockConfig.PutString("log_file", DEFAULT_LOG_FILE);
		sockConfig += *pConfig;
		result = m_pServer->Create(&sockConfig, this);
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(m_pServer);
			return result;
		}
		return BC_R_SUCCESS;
	}
	return BC_R_EXISTS;
}

Napi::Value JsSMPServerWrap::_Start(const Napi::CallbackInfo& info)
{
	if (m_pServer)
	{
		m_pServer->Start();
	}
	return info.Env().Undefined();
}

Napi::Value JsSMPServerWrap::_Close(const Napi::CallbackInfo& info)
{
	if (m_pServer)
	{
		m_pServer->Close();
	}
	return info.Env().Undefined();
}

void JsSMPServerWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);
	BC_SAFE_DELETE_PTR(m_pServer);
	m_sLocalStubMgr.Clear();
	m_hSelf.Reset();
	m_hCallback.Reset();
}

void JsSMPServerWrap::_EventDtorCB(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JSM_NEW_CONN:
		if (refEvent.wParam)
		{
			delete (RecvInfo*)refEvent.wParam;
			refEvent.wParam = 0;
		}
		break;
	}
}

void JsSMPServerWrap::OnJsNewConn(RecvInfo *pInfo)
{
	Napi::HandleScope scope(m_env);

	Napi::Object hConn = JsSMPServerConnectionWrap::New(m_env);
	JsSMPServerConnectionWrap* pConnWrap = JsSMPServerConnectionWrap::Unwrap(hConn);
	if (pConnWrap && m_pServer)
	{
		m_pServer->CreateConnection(pConnWrap, pInfo);
	}
}

void JsSMPServerWrap::OnJsAccept(JsSMPServerConnectionWrap* pConn)
{
	Napi::HandleScope scope(m_env);
    
    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
        
        // 准备参数
        std::initializer_list<Napi::Value> argv = { 
            Napi::String::New(m_env, connection_sym), 
            pConn->Value()  // 获取连接对象的 JS 表示
        };
        
        // 调用回调函数并处理可能的异常
        try {
            callback.Call(Value(), argv);
        } catch (const std::exception& e) {
            Napi::Error::New(m_env, e.what()).ThrowAsJavaScriptException();
        }
    }
}

void JsSMPServerWrap::OnJsExecDone(RPCStub *pStub)
{
	Napi::HandleScope scope(m_env);

}

void JsSMPServerWrap::OnJsClose()
{
	Napi::HandleScope scope(m_env);

	// 获取回调函数
    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
        
        // 准备参数
        std::initializer_list<Napi::Value> argv = { Napi::String::New(m_env, close_sym) };
        
        // 调用回调函数并处理可能的异常
        try {
            callback.Call(Value(), argv);
        } catch (const std::exception& e) {
            Napi::Error::New(m_env, e.what()).ThrowAsJavaScriptException();
        }
    }
	_Cleanup();
}

bool JsSMPServerWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsSMPServerWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JSM_NEW_CONN:
		if (refEvent.wParam)
		{
			m_pServer->initial_pkts_in_js++;
			m_pServer->DumpStats();

			RecvInfo *pInfo = (RecvInfo*)refEvent.wParam;
			OnJsNewConn(pInfo);
			refEvent.wParam = 0; // avoid to delete connect info
		}
		break;
	case JSM_ACCEPT:
		if (refEvent.wParam)
		{
			OnJsAccept(static_cast<JsSMPServerConnectionWrap*>(
				(IServerConnectionHandler*)refEvent.wParam));
		}
		break;
	case JSM_EXEC_DONE:
		if (refEvent.wParam)
		{
			RPCStub *pStub = (RPCStub *)refEvent.wParam;
			OnJsExecDone(pStub);
		}
		break;
	case JSM_CLOSE:
		OnJsClose();
		break;
	default:
		BCDefEventProc(refEvent);
		break;
	}
	return true;
}

void JsSMPServerWrap::OnExchangeShutdown()
{

}

void JsSMPServerWrap::OnNewConn(RecvInfo *pInfo)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_NEW_CONN, 0, 0), pInfo,
		NULL, _EventDtorCB);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerWrap::OnAccept(IServerConnectionHandler* handler)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_ACCEPT, 0, 0), handler);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerWrap::OnExecDone(IRPCStub *pStub)
{
	RPCStub* pRPCStub = static_cast<RPCStub*>(pStub);
	BCEventItemS sEvent(MAKEEVENT(JSM_EXEC_DONE, 0, 0), pRPCStub);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerWrap::OnClosed()
{
	BCEventItemS sEvent(MAKEEVENT(JSM_CLOSE, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerWrap::OnException(BCException &except)
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// class : JsSMPServerConnectionWrap
///////////////////////////////////////////////////////////////////////////////

Napi::FunctionReference	JsSMPServerConnectionWrap::constructor_template;

JsSMPServerConnectionWrap::JsSMPServerConnectionWrap(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<JsSMPServerConnectionWrap>(info)
	, m_env(info.Env())
	, m_pConn(NULL)
{
	if (!info.IsConstructCall()) {
		THROW_ERROR_VOID(m_env, "Use the new operator to create new ServerConnection objects");
	}
	BCRESULT result = Create();
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

JsSMPServerConnectionWrap::~JsSMPServerConnectionWrap()
{
	_Cleanup();
}

void JsSMPServerConnectionWrap::Initialize(Napi::Env env, Napi::Object exports)
{
	Napi::HandleScope scope(env);

	Napi::Function ctor = DefineClass(env, "ServerConnection", {
        InstanceMethod<&JsSMPServerConnectionWrap::_Accept>("__accept__"),
        InstanceMethod<&JsSMPServerConnectionWrap::_SendPacket>("__sendPacket__"),
        InstanceMethod<&JsSMPServerConnectionWrap::_Close>("__close__"),
    });
    constructor_template.Reset(ctor);
    constructor_template.SuppressDestruct();
    exports.Set("ServerConnection", ctor);
}

Napi::Object JsSMPServerConnectionWrap::New(Napi::Env env)
{
	Napi::EscapableHandleScope scope(env);

	Napi::Object hConn = constructor_template.New({});

	return scope.Escape(hConn).As<Napi::Object>();
}

BCRESULT JsSMPServerConnectionWrap::Create()
{
	return BC_R_SUCCESS;
}

void JsSMPServerConnectionWrap::SetConnection(ServerConnPtr pConn)
{
	m_pConn = pConn;
}

Napi::Value JsSMPServerConnectionWrap::_Accept(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::HandleScope scope(env);

	if (info.Length() < 1 || !info[0].IsBoolean())
	{
		Napi::TypeError::New(env, "Invalid arguments.");
		return env.Null();
	}
	bool bAccept = info[0].As<Napi::Boolean>();
	BufferPtr pBuffer(new BCBuffer());
	if (info.Length() > 1 && info[1].IsString())
	{
		std::string strInfo = info[1].As<Napi::String>().Utf8Value();
		pBuffer->Write(strInfo.c_str(), strInfo.length());
	}
	if (m_pConn)
	{
		m_pConn->Accept(bAccept, pBuffer);
	}
	return env.Null();
}

Napi::Value JsSMPServerConnectionWrap::_SendPacket(const Napi::CallbackInfo& info)
{
	Napi::HandleScope scope(info.Env());

	if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsNumber()
		|| !info[2].IsNumber() || !info[3].IsBuffer())
	{
		Napi::TypeError::New(info.Env(), "Invalid arguments.");
		return info.Env().Null();
	}
	if (m_pConn)
	{
		SMPacketPtr pPacket(new SMPacket());
		pPacket->type = (SMP_PTYPE)info[0].As<Napi::Number>().Uint32Value();
		pPacket->timestamp = info[1].As<Napi::Number>().Uint32Value();
		pPacket->trans_id = info[2].As<Napi::Number>().Uint32Value();
		Napi::Buffer<uint8_t> buffer = info[3].As<Napi::Buffer<uint8_t>>();
		pPacket->Write(buffer.Data(), buffer.Length());
		m_pConn->SendPacket(pPacket);
	}
	return info.Env().Null();
}

Napi::Value JsSMPServerConnectionWrap::_Close(const Napi::CallbackInfo& info)
{
	if (m_pConn)
	{
		m_pConn->Close();
	}
	return info.Env().Undefined();
}

void JsSMPServerConnectionWrap::_Cleanup()
{
	Napi::HandleScope scope(m_env);
	m_pConn.reset();
	m_sLocalStubMgr.Clear();
	m_hSelf.Reset();
	m_hCallback.Reset();
}

void JsSMPServerConnectionWrap::_EventDtorCB(BCEventItemS &refEvent)
{

}

void JsSMPServerConnectionWrap::OnJsHandshakeFinished()
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		std::initializer_list<Napi::Value> argv = { 
			Napi::String::New(m_env, handshake_finished_sym) 
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPServerConnectionWrap::OnJsExecDone(RPCStub *pStub)
{
	Napi::HandleScope scope(m_env);

	if (pStub) 
	{
		if (BC_R_SUCCESS == pStub->m_result) 
		{
			if (kConnect.Equal(pStub->m_szCmd))
			{
				ArgsList argv = {
					m_env.Null(),
					Napi::String::New(m_env, (const char*)pStub->m_lParams[0], pStub->m_lParams[1])
				};
				// TRY_CATCH_CALL(m_env, Value(), pStub->m_fCallback, argv);
				// 调用回调函数并处理可能的异常
				try {
					pStub->m_fCallback.Call(Value(), argv);
				} catch (const std::exception& e) {
					Napi::Error::New(m_env, e.what()).ThrowAsJavaScriptException();
				}
			}
			else
			{
				ArgsList argv = { m_env.Null() };
				TRY_CATCH_CALL(m_env, Value(), pStub->m_fCallback, argv);
			}
		}
		else 
		{
			BCPString strMsg;
			strMsg.Format("Failed to call '%s'[error code : %" _U32BITARG_"]",
				pStub->m_szCmd, pStub->m_result);
			ERROR_CALL(m_env, Value(), pStub->m_fCallback, strMsg.c_str());
		}
		m_sLocalStubMgr.PutStub(pStub->m_nTransId, FALSE);
	}
}

void JsSMPServerConnectionWrap::OnJsConnect(LPCSTR lpszProps, size_t len)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		std::initializer_list<Napi::Value> argv = { 
			Napi::String::New(m_env, connect_sym), 
			Napi::String::New(m_env, lpszProps, len)  
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPServerConnectionWrap::OnJsRecvCmd(LPCSTR lpszCmd, size_t len)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		std::initializer_list<Napi::Value> argv = { 
			Napi::String::New(m_env, command_sym),  
			Napi::String::New(m_env, lpszCmd, len) 
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPServerConnectionWrap::OnJsRecvData(LPCVOID lpszMsg, size_t len)
{
	Napi::HandleScope scope(m_env);

    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		Napi::Buffer<uint8_t> outBuffer = Napi::Buffer<uint8_t>::Copy(m_env, (uint8_t *)lpszMsg, len);
		std::initializer_list<Napi::Value> argv = { 
			Napi::String::New(m_env, data_sym),  
			outBuffer 
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
}

void JsSMPServerConnectionWrap::OnJsClosed(LPCSTR strReason)
{
	Napi::HandleScope scope(m_env);

	m_pConn.reset();
	// 获取回调函数
    if (m_hCallback.Value().IsFunction())
    {
        Napi::Function callback = m_hCallback.Value().As<Napi::Function>();
		std::initializer_list<Napi::Value> argv = { 
				Napi::String::New(m_env, close_sym),
				Napi::String::New(m_env, strReason) 
		};
		TRY_CATCH_CALL(m_env, Value(), callback, argv);
	}
	_Cleanup();
}

bool JsSMPServerConnectionWrap::OnBeforeExchangeEvent(BCEventItemS &refEvent)
{
	return true;
}

bool JsSMPServerConnectionWrap::OnExchangeEvent(BCEventItemS &refEvent)
{
	switch (EVENTMAJOR(refEvent.eType))
	{
	case JSM_HSK_FINISH:
		OnJsHandshakeFinished();
		break;
	case JSM_EXEC_DONE:
		if (refEvent.wParam)
		{
			RPCStub *pStub = (RPCStub *)refEvent.wParam;
			OnJsExecDone(pStub);
		}
		break;
	case JSM_CONNECT:
		if (refEvent.wParam)
		{
			LPCSTR lpszProps = (LPCSTR)refEvent.wParam;
			OnJsConnect(lpszProps, refEvent.lParam);
		}
		break;
	case JSM_RECV_CMD:
		if (refEvent.wParam)
		{
			LPCSTR lpszCmd = (LPCSTR)refEvent.wParam;
			OnJsRecvCmd(lpszCmd, refEvent.lParam);
		}
		break;
	case JSM_RECV_DATA:
		if (refEvent.wParam)
		{
			LPCVOID lpszMsg = (LPCVOID)refEvent.wParam;
			OnJsRecvData(lpszMsg, refEvent.lParam);
		}
		break;
	case JSM_CLOSE:
		OnJsClosed((LPCSTR)refEvent.wParam);
		break;
	default:
		BCDefEventProc(refEvent);
		break;
	}
	return true;
}

void JsSMPServerConnectionWrap::OnExchangeShutdown()
{

}

void JsSMPServerConnectionWrap::OnHandshakeFinished()
{
	BCEventItemS sEvent(MAKEEVENT(JSM_HSK_FINISH, 0, 0));
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnExecDone(IRPCStub *pStub)
{
	RPCStub* pRPCStub = static_cast<RPCStub*>(pStub);
	BCEventItemS sEvent(MAKEEVENT(JSM_EXEC_DONE, 0, 0), pRPCStub);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnConnect(LPCSTR lpszProps, size_t msg_size)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_CONNECT, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(lpszProps, msg_size);
	sEvent.lParam = msg_size;
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnRecvCmd(const SMPHeader &refHeader,  LPCSTR lpszCmd, size_t msg_size)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_RECV_CMD, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(lpszCmd, msg_size);
	sEvent.lParam = msg_size;
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnRecvData(const SMPHeader &refHeader, LPCVOID lpszMsg, size_t msg_size)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_RECV_DATA, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyBuffer(lpszMsg, msg_size);
	sEvent.lParam = msg_size;
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnClosed(LPCSTR strReason)
{
	BCEventItemS sEvent(MAKEEVENT(JSM_CLOSE, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(strReason, strlen(strReason));
	sEvent.lParam = strlen(strReason);
	JsExchanger::ExchangeEvent(sEvent, this);
}

void JsSMPServerConnectionWrap::OnException(BCException &except)
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

// NS_CC_END