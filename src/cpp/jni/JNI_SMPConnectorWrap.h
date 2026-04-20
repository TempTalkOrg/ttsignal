///////////////////////////////////////////////////////////////////////////////
// file : JNI_SMPConnectorWrap.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef JNI_SMPCONNECTORWRAP_H_INCLUDED__
#define JNI_SMPCONNECTORWRAP_H_INCLUDED__

#include <jni.h>
#include <BC/Exchanger.h>
#include "RPCStub.h"
#include "SMPConnector.h"
#include "Interface.h"
#include "JNI_SMPConfig.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

///////////////////////////////////////////////////////////////////////////////
// Class : SMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

class SMPConnectionWrap
	: public IConnectionHandler
{
public:
	SMPConnectionWrap();
    virtual ~SMPConnectionWrap();

	static BCRESULT		Initialize(JNIEnv *env);
	static jlong		New(BCFObject* pConfig);
	BCRESULT			Create(BCFObject* pConfig);
	void				SetConnection(SMPConnection* pConn);
protected:
	static	jlong		_Initialize(
							JNIEnv* env, 
							jobject, 
							jlong handle, 
							jobject connObj,
							jobject listener);
	static	jint		_Connect(
							JNIEnv* env, 
							jobject obj, 
							jlong handle, 
							jstring url, 
							jstring props,
							jint timeout_ms);
	static	jint		_SendPacket(
							JNIEnv* env, 
							jobject obj, 
							jlong handle, 
							jlong packet);
	static	void		_Restart(JNIEnv* env, jobject obj, jlong handle, jlong networkHandle);
	static	void		_Close(JNIEnv* env, jobject obj, jlong handle);
	static	void		_CloseStream(JNIEnv* env, jobject obj, jlong handle, jint nStreamId);
	static	void		_Destroy(JNIEnv* env, jobject, jlong handle);
private:
	void				_Cleanup(JNIEnv* env);
private:
	// Override IConnectionHandler interfaces
	void				OnHandshakeFinished() override;
	void				OnExecDone(IRPCStub *pStub) override;
	void 				OnStreamCreated(uint32_t nStreamId) override;
	void 				OnStreamClosed(uint32_t nStreamId) override;
	void				OnStreamDataAcked(
							uint32_t nStreamId,
							xqc_usec_t ack_delay_time,
							size_t acked_bytes,
							size_t inflight_bytes) override;
	void				OnStreamDataSent(
							uint32_t nStreamId, 
							uint32_t nTransId,
							size_t size) override;
	void				OnRecvCmd(
							const SMPHeader &refHeader,
							const char* lpszCmd, 
							size_t msg_size) override;
	void				OnRecvData(
							const SMPHeader &refHeader,
							LPCVOID lpszMsg, 
							size_t msg_size) override;
	void				OnClosed(LPCSTR strReason) override;
	void				OnRestart(BCRESULT result, const char *lpszAddr) override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(SMPConnectionWrap);
	RPCStubMgr				m_sLocalStubMgr;
	SMPConnection		*	m_pConn;
	jobject					m_pSelf;
	jobject					m_pHandler;
	jclass					m_pHandlerCls;
};

///////////////////////////////////////////////////////////////////////////////
// Class : SMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

class SMPConnectorWrap
	: public IConnectorHandler
{
public:
	SMPConnectorWrap();
    virtual ~SMPConnectorWrap();

	static BCRESULT		Initialize(JNIEnv *env);
	BCRESULT			Create(BCFObject* pConfig, LogHandler *pLogHandler);
protected:
	static	jlong		_New(JNIEnv* env, jobject, jobject config, jobject connector);
	static	jlong		_CreateConnection(
							JNIEnv* env, jobject,
							jlong connectorHandle, 
							jobject config);
	static	jobject		_GetStats(JNIEnv* env, jobject obj, jlong handle);
	static	void		_Close(JNIEnv* env, jobject, jlong handle);
	static	void		_Destroy(JNIEnv* env, jobject, jlong handle);
private:
	void				_Cleanup();
private:
	// Override IConnectorHandler interfaces
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnClosed() override;
	void				OnException(BCException &) override;
    void                OnLog(int level, LPCSTR lpszMsg) override;
private:
    DECLARE_NO_COPY_CLASS(SMPConnectorWrap);
	RPCStubMgr					m_sLocalStubMgr;
	SMPConnector			*	m_pConnector;
	jobject						m_pSelf;
	std::unique_ptr<LogHandler> m_pLogHandler;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI


#endif // JNI_SMPCONNECTORWRAP_H_INCLUDED__
