///////////////////////////////////////////////////////////////////////////////
// file : JNI_SMPServerWrap.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef JNI_SMPSERVERWRAP_H_INCLUDED__
#define JNI_SMPSERVERWRAP_H_INCLUDED__

#include <jni.h>
#include <BC/Exchanger.h>
#include "RPCStub.h"
#include "SMPServer.h"
#include "Interface.h"
#include "jni_utils.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

class SMPServerConnectionWrap;

///////////////////////////////////////////////////////////////////////////////
// Class : SMPServerConnectionWrap
///////////////////////////////////////////////////////////////////////////////

class SMPServerConnectionWrap
	: public IServerConnectionHandler
{
	DECLARE_FIXED_ALLOC(SMPServerConnectionWrap);

	friend class SMPGroupWrap;
public:
	SMPServerConnectionWrap();
    virtual ~SMPServerConnectionWrap();

	static BCRESULT		Initialize(JNIEnv *env);
	static SMPServerConnectionWrap*		New();
	BCRESULT			Create();
	void				SetConnection(ServerConnPtr pConn) override;
protected:
	static	jlong		_Initialize(
							JNIEnv* env,
							jobject,
							jlong handle,
							jobject connObj,
							jobject listener);
	static	jint		_Accept(
							JNIEnv* env, 
							jobject obj, 
							jlong handle, 
							jboolean bAccept, 
							jstring respInfo);
	static	jint		_EnableMask(
							JNIEnv* env, 
							jobject obj, 
							jlong handle, 
							jboolean enable);
	static	jint		_SendPacket(
							JNIEnv* env, 
							jobject obj,
							jlong handle,
							jlong packet);
	static	void		_Close(JNIEnv* env, jobject obj, jlong handle);
	static	void		_Destroy(JNIEnv* env, jobject, jlong handle);
private:
	void				_Cleanup(JNIEnv *env);
private:
	// Override IServerConnectionHandler interfaces
	void				OnHandshakeFinished() override;
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnConnect(const char* lpszCmd, size_t size) override;
	void				OnRecvCmd(
							const SMPHeader& refHeader, 
							const char* lpszCmd, 
							size_t msg_size) override;
	void				OnRecvData(
							const SMPHeader& refHeader,
							LPCVOID lpszMsg, 
							size_t msg_size) override;
	void				OnClosed(LPCSTR strReason) override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(SMPServerConnectionWrap);
	ServerConnPtr			m_pConn;
	jobject					m_pSelf;
	jobject					m_pHandler;
	jclass					m_pHandlerCls;
};

///////////////////////////////////////////////////////////////////////////////
// Class : SMPServerWrap
///////////////////////////////////////////////////////////////////////////////

class SMPServerWrap
	: public IServerHandler
{
public:
	SMPServerWrap();
    virtual ~SMPServerWrap();

	static BCRESULT		Initialize(JNIEnv *env);
	BCRESULT			Create(BCFObject* pConfig);
protected:
	static	jlong		_New(
							JNIEnv* env, 
							jobject obj, 
							jobject jselfObj, 
							jobject jConfig, 
							jobject jHandler);
	static	jint		_Start(JNIEnv* env, jobject obj, jlong handle);
	static	jobject		_GetStats(JNIEnv* env, jobject obj, jlong handle);
	static	void		_Close(JNIEnv* env, jobject obj, jlong handle);
	static	void		_Destroy(JNIEnv* env, jobject, jlong handle);
private:
	void				_Cleanup();
private:
	// Override IServerHandler interfaces
	void				OnNewConn(RecvInfo *pInfo) override;
	void				OnAccept(IServerConnectionHandler* handler) override;
	void				OnExecDone(IRPCStub *pStub) override;
	void				OnClosed() override;
	void				OnException(BCException &) override;
private:
    DECLARE_NO_COPY_CLASS(SMPServerWrap);
	SMPServer			*	m_pServer;
	jobject					m_pSelf;
	jobject					m_pHandler;
	jclass					m_pHandlerCls;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI


#endif // JNI_SMPSERVERWRAP_H_INCLUDED__
