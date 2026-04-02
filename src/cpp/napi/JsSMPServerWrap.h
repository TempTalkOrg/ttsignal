
#ifndef JSSMPSERVERWRAP_H_INCLUDED__
#define JSSMPSERVERWRAP_H_INCLUDED__

#include <napi.h>
#include <BC/Exchanger.h>
#include "macros.h"
#include "RPCStub.h"
#include "../SMPServer.h"
#include "../Interface.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

class JsSMPServerConnectionWrap;

///////////////////////////////////////////////////////////////////////////////
// Class : JsSMPServerWrap
///////////////////////////////////////////////////////////////////////////////

class JsSMPServerWrap
	: public Napi::ObjectWrap<JsSMPServerWrap>
	, public IExchangeHandler
	, public IServerHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsSMPServerWrap(const Napi::CallbackInfo& info);
    virtual ~JsSMPServerWrap();

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
	Napi::Value 		_Start(const Napi::CallbackInfo& info);
	Napi::Value 		_Close(const Napi::CallbackInfo& info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	void				OnJsNewConn(RecvInfo *pInfo);
	void				OnJsAccept(JsSMPServerConnectionWrap *pConn);
	void				OnJsExecDone(RPCStub *pStub);
	void				OnJsClose();
private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	void				OnNewConn(RecvInfo *pInfo) override;
	void				OnAccept(IServerConnectionHandler* handler) override;
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnClosed() override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(JsSMPServerWrap);
	Napi::ObjectReference	m_hSelf;
	Napi::FunctionReference	m_hCallback;
	Napi::Env				m_env;
	RPCStubMgr				m_sLocalStubMgr;
	SMPServer			*	m_pServer;
};

///////////////////////////////////////////////////////////////////////////////
// Class : JsSMPServerConnectionWrap
///////////////////////////////////////////////////////////////////////////////

class JsSMPServerConnectionWrap
	: public Napi::ObjectWrap<JsSMPServerConnectionWrap>
	, public IExchangeHandler
	, public IServerConnectionHandler
{
public:
	static Napi::FunctionReference constructor_template;
public:
	JsSMPServerConnectionWrap(const Napi::CallbackInfo& info);
    virtual ~JsSMPServerConnectionWrap();

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
	static Napi::Object New(Napi::Env env);
	BCRESULT			Create();
	void				SetConnection(ServerConnPtr pConn) override;
protected:
	Napi::Value			_Accept(const Napi::CallbackInfo& info);
	Napi::Value			_SendPacket(const Napi::CallbackInfo& info);
	Napi::Value			_Close(const Napi::CallbackInfo& info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	void				OnJsHandshakeFinished();
	void				OnJsExecDone(RPCStub *pStub);
	// Override IExchangeHandler interfaces
	void				OnJsConnect(LPCSTR props, size_t len);
	void				OnJsRecvCmd(LPCSTR cmd, size_t len);
	void				OnJsRecvData(LPCVOID data, size_t len);
	void				OnJsClosed(LPCSTR strReason);
private:
	// Override IExchangeHandler interfaces
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	// Override IServerConnectionHandler interfaces
	void				OnHandshakeFinished() override;
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnConnect(const char* lpszProps, size_t size)	override;
	void				OnRecvCmd(const SMPHeader &refHeader, const char* lpszCmd, size_t msg_size) override;
	void				OnRecvData(const SMPHeader &refHeader, LPCVOID data, size_t msg_size) override;
	void				OnClosed(LPCSTR strReason) override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(JsSMPServerConnectionWrap);
	Napi::ObjectReference	m_hSelf;
	Napi::FunctionReference	m_hCallback;
	Napi::Env				m_env;
	RPCStubMgr				m_sLocalStubMgr;
	ServerConnPtr			m_pConn;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node


#endif // JSSMPSERVERWRAP_H_INCLUDED__
