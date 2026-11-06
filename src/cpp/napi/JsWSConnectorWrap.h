




































































































































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





namespace node
{

class JsWSConnectorWrap;
class JsWSConnectionWrap;





enum WSJsEvent
{
	WSJS_CONNECT_RESULT	= 1,
	WSJS_TEXT			= 2,
	WSJS_DATA			= 3,
	WSJS_CLOSED			= 4,
	WSJS_EXCEPTION		= 5,
};










struct WSEventPayload
{
	uint64_t		nConnId			= 0;
	uint32_t		eEvent			= 0;		

	
	BCRESULT		result			= 0;
	HttpHeaderMap	mapHeaders;
	std::string		strPeerIp;
	uint32_t		nBoundIfIndex	= 0;
	std::string		strPinMethod;

	
	
	
	std::string		strBody;

	
	std::string		strErrMessage;
};












class JsWSConnSink : public IWSConnectionHandler
{
public:
	JsWSConnSink(JsWSConnectorWrap *pOwner, uint64_t nConnId);
	virtual ~JsWSConnSink();

	
	
	void			Detach();

	
	
	
	std::weak_ptr<WS::WSConnection>	m_wpConn;

private:
	DECLARE_NO_COPY_CLASS(JsWSConnSink);

	
	void			OnConnectResult(BCRESULT result,
									const HttpHeaderMap &refHeaders) override;
	void			OnRecvText(LPCSTR lpszText) override;
	void			OnRecvData(LPCVOID data, size_t size) override;
	void			OnClosed(LPCSTR lpszReason) override;
	void			OnException(BCException &refExcept) override;

	
	void			_Post(WSEventPayload *pPayload);

	
	
	
	std::mutex			m_lock;
	JsWSConnectorWrap *	m_pOwner;
	const uint64_t		m_nConnId;
};







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
	
	
	
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(
			node::GetPrototypeProperty(m_env, Value(), propertyName));
	}

	
	
	void					Attach(JsWSConnectorWrap *pOwner,
								   uint64_t nConnId,
								   const WSConnPtr &refConn,
								   Napi::Object hConnector);
	
	
	void					OnOwnerGone();
	
	
	
	void					AbandonPendingConnect();
	
	
	void					DetachNativeConn();
	uint64_t				ConnId() const { return m_nConnId; }

	
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

	
	
	void				_Pin();
	void				_Unpin();
	void				_SettleConnect(const WSEventPayload &refPayload);
	void				_EmitInternal(LPCSTR lpszType, Napi::Value hArg);

	Napi::Env				m_env;
	
	Napi::FunctionReference	m_hCallback;
	
	Napi::FunctionReference	m_hConnectCallback;
	
	Napi::ObjectReference	m_hConnector;

	JsWSConnectorWrap	*	m_pOwner;		
	WSConnPtr				m_pConn;
	uint64_t				m_nConnId;

	
	
	bool					m_bConnectCalled;
	bool					m_bConnectSettled;
	bool					m_bUpgraded;	
	bool					m_bClosedSeen;
	
	bool					m_bPinned;

	
	
	
	
	
	
	
	
	
	
	std::string				m_strPeerIp;
	uint32_t				m_nBoundIfIndex;
	std::string				m_strPinMethod;
	
	
	std::string				m_strLastError;
};





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

	
	
	bool					PostEventPayload(WSEventPayload *pPayload);
	
	void					OnConnectionWrapGone(uint64_t nConnId);
	
	
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
	
	
	bool				_ValidateConfig(Napi::Object config,
										std::string &outError);
	
	
	void				_ReleaseNativeRefs();

	
	void				OnJsConnEvent(WSEventPayload *pPayload);
	void				OnJsLog(int level, LPCSTR lpszMsg);
	void				OnJsClosed();
	void				OnJsException(LPCSTR lpszMsg);

private:
	
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	
	void				OnLog(int level, LPCSTR lpszMsg) override;
	void				OnClosed() override;
	void				OnException(BCException &refExcept) override;

private:
	DECLARE_NO_COPY_CLASS(JsWSConnectorWrap);
	typedef std::map<uint64_t, JsWSConnSink *>			SinkMap;
	typedef std::map<uint64_t, JsWSConnectionWrap *>		ConnWrapMap;

	Napi::FunctionReference	m_hCallback;		
	Napi::FunctionReference	m_hLogCallback;		
	
	
	
	std::vector<Napi::FunctionReference>	m_vecCloseCallbacks;
	Napi::Env				m_env;
	WS::WSConnector		*	m_pConnector;

	
	
	TTVpnPolicy				m_ePolicy;
	uint32_t				m_nDrainTimeoutMs;
	
	
	ConnStatsMap			m_mapLastStats;

	
	SinkMap					m_mapSinks;		
	ConnWrapMap				m_mapConnWraps;	
	uint64_t				m_nNextConnId;

	
	
	
	std::atomic<bool>		m_bLogEnabled;
	
	bool					m_bClosePending;
	
	
	
	uint32_t				m_nPendingCloseRefs;
};





} 

#endif 




