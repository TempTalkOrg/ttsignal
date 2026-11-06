































































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





namespace node
{

class JsHttpConnectorWrap;










struct HttpResultPayload
{
	uint32_t		nId				= 0;		
	bool			bOk				= false;	

	
	BCRESULT		result			= 0;
	std::string		strErrMessage;

	
	int				nStatus			= 0;
	
	
	std::string		strReason;
	HttpHeaderMap	mapHeaders;
	std::string		strBody;
	std::string		strPeerIp;
	uint32_t		nBoundIfIndex	= 0;
	std::string		strPinMethod;
};








class JsHttpRequestTask : public IHttpRequestHandler
{
public:
	JsHttpRequestTask(JsHttpConnectorWrap *pOwner, uint32_t nId);
	virtual ~JsHttpRequestTask();

	
	void				OnHttpResponse(const HttpResponse &refResp) override;
	void				OnHttpError(BCRESULT result,
									const std::string &refMessage) override;

	
	Napi::FunctionReference	m_hCallback;
	uint32_t				m_nId;
private:
	DECLARE_NO_COPY_CLASS(JsHttpRequestTask);
	JsHttpConnectorWrap	*	m_pOwner;
};





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
	
	
	
	inline Napi::Value GetPrototypeProperty(const std::string &propertyName) {
		Napi::EscapableHandleScope scope(m_env);
		return scope.Escape(
			node::GetPrototypeProperty(m_env, Value(), propertyName));
	}
	BCRESULT				Create(Napi::Object config);

	
	
	bool					PostResult(HttpResultPayload *pPayload);
protected:
	Napi::Value			_Request(const Napi::CallbackInfo &info);
	Napi::Value			_Close(const Napi::CallbackInfo &info);
	Napi::Value			_Info(const Napi::CallbackInfo &info);
	Napi::Value			_SetIdleTimeout(const Napi::CallbackInfo &info);
private:
	void				_Cleanup();
	static void			_EventDtorCB(BCEventItemS &refEvent);

	
	
	

	
	
	bool				_ValidateConfig(Napi::Object config,
										std::string &outError);
	bool				_ParseRequest(Napi::Object options,
									  HttpRequest &outRequest,
									  std::string &outError);

	
	
	void				_PostFailure(uint32_t nId, BCRESULT result,
									 const std::string &refMessage);
	
	void				_SettlePendingTasks(BCRESULT result,
											const std::string &refMessage);

	Napi::Value			_MakeError(const HttpResultPayload &refPayload);
	Napi::Value			_MakeResponse(const HttpResultPayload &refPayload);

	
	void				OnJsResult(HttpResultPayload *pPayload);
	void				OnJsLog(int level, LPCSTR lpszMsg);
	void				OnJsClosed();
private:
	
	bool				OnBeforeExchangeEvent(BCEventItemS &refEvent) override;
	bool				OnExchangeEvent(BCEventItemS &refEvent) override;
	void				OnExchangeShutdown() override;
	
	void				OnLog(int level, LPCSTR lpszMsg) override;
	void				OnClosed() override;
private:
	DECLARE_NO_COPY_CLASS(JsHttpConnectorWrap);
	typedef std::map<uint32_t, JsHttpRequestTask *>	TaskMap;

	Napi::FunctionReference	m_hCallback;		
	Napi::FunctionReference	m_hLogCallback;		
	
	
	
	std::vector<Napi::FunctionReference>	m_vecCloseCallbacks;
	Napi::Env				m_env;
	HttpConnector		*	m_pConnector;
	
	
	
	TTVpnPolicy				m_ePolicy;
	size_t					m_nMaxResponseBytes;
	uint32_t				m_nDrainTimeoutMs;
	
	
	
	uint32_t				m_nMaxIdleConnections;
	uint32_t				m_nIdleTimeoutMs;
	
	TaskMap					m_mapTasks;
	uint32_t				m_nNextTaskId;
	
	
	
	
	
	
	
	
	
	std::atomic<bool>		m_bLogEnabled;
	
	bool					m_bClosePending;
};





} 

#endif 




