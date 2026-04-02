
#ifndef JSSMPCONNECTORWRAP_H_INCLUDED__
#define JSSMPCONNECTORWRAP_H_INCLUDED__

#include <napi.h>
#include <BC/Exchanger.h>
#include "macros.h"
#include "RPCStub.h"
#include "../SMPConnector.h"
#include "../Interface.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// Class : JsSMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

class JsSMPConnectorWrap
	: public Napi::ObjectWrap<JsSMPConnectorWrap>
	, public IExchangeHandler
	, public IConnectorHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsSMPConnectorWrap(const Napi::CallbackInfo& info);
    virtual ~JsSMPConnectorWrap();

	static void				Initialize(Napi::Env env, Napi::Object exports);
	static inline bool		HasInstance(Napi::Env env, Napi::Value val) {
		Napi::HandleScope scope(env);
		if (!val.IsObject()) return false;
		Napi::Object obj = val.As<Napi::Object>();
		return obj.InstanceOf(constructor_template.Value());
	}
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(::GetPrototypeProperty(m_env, Value(), propertyName));
	}
	BCRESULT				Create(Napi::Object config);
protected:
	Napi::Value			_CreateConnection(const Napi::CallbackInfo& info);
	Napi::Value			_Close(const Napi::CallbackInfo& info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	void				OnJsExecDone(RPCStub *pStub);
	void				OnJsLog(int level, LPCSTR lpszMsg);
	void				OnJsClose();
private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnClosed() override;
	void				OnException(BCException &) override;
	void				OnLog(int level, LPCSTR lpszMsg) override;
private:
    DECLARE_NO_COPY_CLASS(JsSMPConnectorWrap);
	Napi::ObjectReference	m_hSelf;
	Napi::FunctionReference	m_hCallback;
	Napi::FunctionReference	m_hLogCallback;
	Napi::Env				m_env;
	RPCStubMgr				m_sLocalStubMgr;
	SMPConnector		*	m_pConnector;
};

///////////////////////////////////////////////////////////////////////////////
// Class : JsSMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

class JsSMPConnectionWrap
	: public Napi::ObjectWrap<JsSMPConnectionWrap>
	, public IExchangeHandler
	, public IConnectionHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsSMPConnectionWrap(const Napi::CallbackInfo& info);
    virtual ~JsSMPConnectionWrap();

	static void				Initialize(Napi::Env env, Napi::Object exports);
	static inline bool		HasInstance(Napi::Env env, Napi::Value val) {
		Napi::HandleScope scope(env);
		if (!val.IsObject()) return false;
		Napi::Object obj = val.As<Napi::Object>();
		return obj.InstanceOf(constructor_template.Value());
	}
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(::GetPrototypeProperty(m_env, Value(), propertyName));
	}
	static Napi::Object 	New(Napi::Env env, Napi::Object config);
	BCRESULT				Create(Napi::Object config);
	void					SetConnection(SMPConnection* pConn);
protected:
	Napi::Value 		_Connect(const Napi::CallbackInfo& info);
	Napi::Value 		_SendPacket(const Napi::CallbackInfo& info);
	Napi::Value 		_Restart(const Napi::CallbackInfo& info);
	Napi::Value 		_Close(const Napi::CallbackInfo& info);
	Napi::Value 		_CloseStream(const Napi::CallbackInfo& info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	void				OnJsHandshakeFinished();
	void				OnJsExecDone(RPCStub *pStub);
	void 				OnJsStreamCreated(uint32_t nStreamId);
	void 				OnJsStreamClosed(uint32_t nStreamId);
	void				OnJsRecvCmd(uint32_t nStreamId, LPCSTR cmd, size_t len);
	void				OnJsRecvData(uint32_t nStreamId, LPCVOID data, size_t len);
	void				OnJsRestart(BCRESULT result, const char *lpszAddr);
	void				OnJsClosed(LPCSTR strReason);
private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	// Override IConnectionHandler interfaces
	void				OnHandshakeFinished() override;
	void				OnExecDone(IRPCStub *pStub) override;
	void 				OnStreamCreated(uint32_t nStreamId) override;
	void 				OnStreamClosed(uint32_t nStreamId) override;
	void				OnRecvCmd(const SMPHeader &refHeader, const char* lpszCmd, size_t msg_size) override;
	void				OnRecvData(const SMPHeader &refHeader, LPCVOID data, size_t msg_size) override;
	void				OnRestart(BCRESULT result, const char *lpszAddr) override;
	void				OnClosed(LPCSTR strReason) override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(JsSMPConnectionWrap);
	Napi::ObjectReference	m_hSelf;
	Napi::FunctionReference	m_hCallback;
	Napi::Env				m_env;
	RPCStubMgr				m_sLocalStubMgr;
	SMPConnection		*	m_pConn;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node


#endif // JSSMPCONNECTORWRAP_H_INCLUDED__
