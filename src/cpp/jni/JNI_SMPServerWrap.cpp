///////////////////////////////////////////////////////////////////////////////
// file : JNI_SMPServerWrap.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "Utils.h"
#include "SMPServer.h"
#include "Runtime.h"
#include "jni_utils.h"
#include "JNI_SMPConfig.h"
#include "JNI_SMPServerWrap.h"
#include "JNI_SMPacketWrap.h"


#define MY_TLS_GROUPS "X25519:P-384:P-521"



enum
{
	JSM_HSK_FINISH			= 1,
	JSM_NEW_CONN			= 2,
	JSM_ACCEPT				= 3,
	JSM_EXEC_DONE			= 4,
	JSM_RECV_CMD			= 5,
	JSM_RECV_DATA			= 6,
	JSM_CLOSE				= 7,
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

///////////////////////////////////////////////////////////////////////////////
// class : SMPServerConnectionWrap
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(SMPServerConnectionWrap, 32);

SMPServerConnectionWrap::SMPServerConnectionWrap()
	: m_pSelf(NULL)
	, m_pHandler(NULL)
	, m_pHandlerCls(NULL)
{
	//
}

SMPServerConnectionWrap::~SMPServerConnectionWrap()
{
	//
}

BCRESULT SMPServerConnectionWrap::Initialize(JNIEnv *env)
{
	jclass clazz = env->FindClass("org/difft/android/smp/ServerConnection"); 

	static JNINativeMethod methods[] = {
		{
			(char*)"initialize",
			(char*)"(JLorg/difft/android/smp/ServerConnection;Lorg/difft/android/smp/IServerHandler;)J",
			reinterpret_cast<void*>(_Initialize)
		},
		{
			(char*)"accept",
			(char*)"(JZLjava/lang/String;)I",
			reinterpret_cast<void*>(_Accept)
		},
		{
			(char*)"enableMask",
			(char*)"(JZ)I",
			reinterpret_cast<void*>(_EnableMask)
		},
		{
			(char*)"sendPacket",
			(char*)"(JJ)I",
			reinterpret_cast<void*>(_SendPacket)
		},
		{
			(char*)"close",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Close)
		},
		{
			(char*)"destroy",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Destroy)
		}
	};
	// Register native methods
	if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof((methods)[0])) < 0) {
		LogWarn(_LOCAL_, "RegisterNatives error");
		env->DeleteLocalRef(clazz);
		return BC_R_UNEXPECTED;
	}
	env->DeleteLocalRef(clazz);
	return BC_R_SUCCESS;
}

SMPServerConnectionWrap* SMPServerConnectionWrap::New()
{
	SMPServerConnectionWrap* pConnWrap = new SMPServerConnectionWrap();
	BCRESULT result = pConnWrap->Create();
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(pConnWrap);
		return 0;
	}
	return pConnWrap;
}

BCRESULT SMPServerConnectionWrap::Create()
{
	return BC_R_SUCCESS;
}

void SMPServerConnectionWrap::SetConnection(ServerConnPtr pConn)
{
	m_pConn = pConn;
}

jlong SMPServerConnectionWrap::_Initialize(
	JNIEnv* env,
	jobject,
	jlong handle,
	jobject connObj,
	jobject listener)
{
	SMPServerConnectionWrap* _this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	if (_this)
	{
		_this->m_pSelf = env->NewGlobalRef(connObj);
		_this->m_pHandler = env->NewGlobalRef(listener);
		jclass handlerCls = env->FindClass("org/difft/android/smp/IServerHandler");
		if (handlerCls)
		{
			_this->m_pHandlerCls = (jclass)env->NewGlobalRef(handlerCls);
			env->DeleteLocalRef(handlerCls);
		}
	}
	return handle;
}

jint SMPServerConnectionWrap::_Accept(
	JNIEnv* env, 
	jobject obj, 
	jlong handle, 
	jboolean bAccept, 
	jstring respStr)
{
	SMPServerConnectionWrap* _this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		std::shared_ptr<BCBuffer> payload = MakeBufferFromString(env, respStr);
		return _this->m_pConn->Accept(bAccept, payload);
	}
	return BC_R_INVALIDARG;
}

jint SMPServerConnectionWrap::_EnableMask(
	JNIEnv* env, 
	jobject obj, 
	jlong handle, 
	jboolean enable)
{
	SMPServerConnectionWrap* _this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		_this->m_pConn->EnableMask(enable);
		return 0;
	}
	return BC_R_INVALIDARG;
}

jint SMPServerConnectionWrap::_SendPacket(
	JNIEnv* env,
	jobject obj,
	jlong handle,
	jlong packet)
{
	SMPServerConnectionWrap* _this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	SMPacketWrap* _packet = reinterpret_cast<SMPacketWrap*>(packet);
	if (_this && _this->m_pConn && _packet && _packet->packet_)
	{
		return _this->m_pConn->SendPacket(_packet->packet_);
	}
	return BC_R_INVALIDARG;
}

void SMPServerConnectionWrap::_Close(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerConnectionWrap *_this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		_this->m_pConn->Close();
	}
}

void SMPServerConnectionWrap::_Destroy(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerConnectionWrap *_this = reinterpret_cast<SMPServerConnectionWrap*>(handle);
	if (_this)
	{
		delete _this;
	}
}

void SMPServerConnectionWrap::_Cleanup(JNIEnv* env)
{
	if (m_pHandler)
	{
		env->DeleteGlobalRef(m_pHandler);
		m_pHandler = NULL;
	}
	if (m_pHandlerCls)
	{
		env->DeleteGlobalRef(m_pHandlerCls);
		m_pHandlerCls = NULL;
	}
	if (m_pSelf)
	{
		env->DeleteGlobalRef(m_pSelf);
		m_pSelf = NULL;
	}
	m_pConn.reset();
}

void SMPServerConnectionWrap::OnHandshakeFinished()
{
}

void SMPServerConnectionWrap::OnExecDone(IRPCStub *pStub)
{
}

void SMPServerConnectionWrap::OnConnect(const char* lpszCmd, size_t size)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		JniByteBuffer jbuf(pEnv, lpszCmd, size);
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onNewConnection", "(Lorg/difft/android/smp/ServerConnection;Ljava/lang/String;)V");
		if (mid)
		{
			jstring jmsg = pEnv->NewStringUTF(lpszCmd);
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, jmsg);
			pEnv->DeleteLocalRef(jmsg);
		}
	}
}

void SMPServerConnectionWrap::OnRecvCmd(
	const SMPHeader& refHeader,
	const char* lpszCmd,
	size_t msg_size)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		JniByteBuffer jbuf(pEnv, lpszCmd, msg_size);
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onRecvCmd", "(Lorg/difft/android/smp/ServerConnection;JSILjava/nio/ByteBuffer;)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, refHeader.m_nTimestamp,
				refHeader.m_nTransId, jbuf.byteArrayObj_);
		}
	}
}

void SMPServerConnectionWrap::OnRecvData(
	const SMPHeader& refHeader,
	LPCVOID lpszMsg,
	size_t msg_size)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		JniByteBuffer jbuf(pEnv, lpszMsg, msg_size);
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onRecvData", "(Lorg/difft/android/smp/ServerConnection;JSILjava/nio/ByteBuffer;)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, refHeader.m_nTimestamp,
				refHeader.m_nTransId, jbuf.byteArrayObj_);
		}
	}
}

void SMPServerConnectionWrap::OnClosed(LPCSTR strReason)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onClosed", "(Lorg/difft/android/smp/ServerConnection;Ljava/lang/String;)V");
		if (mid)
		{
			jstring jmsg = pEnv->NewStringUTF(strReason);
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, jmsg);
			pEnv->DeleteLocalRef(jmsg);
		}
		_Cleanup(pEnv);
	}
	// After deref java object, its jvm's responsibility to call finalize
	// then destroy to delete this agent.
}

void SMPServerConnectionWrap::OnException(BCException& except)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onException", "(Lorg/difft/android/smp/ServerConnection;Ljava/lang/String;)V");
		if (mid)
		{
			jstring jmsg = pEnv->NewStringUTF(except.GetMsg().c_str());
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, jmsg);
			pEnv->DeleteLocalRef(jmsg);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPServerWrap
///////////////////////////////////////////////////////////////////////////////

SMPServerWrap::SMPServerWrap()
	: m_pServer(NULL)
	, m_pSelf(NULL)
	, m_pHandler(NULL)
	, m_pHandlerCls(NULL)
{
	//
}

SMPServerWrap::~SMPServerWrap()
{
	//
}

BCRESULT SMPServerWrap::Initialize(JNIEnv *env)
{
	jclass clazz = env->FindClass("org/difft/android/smp/Server");

	static JNINativeMethod methods[] = {
		{
			(char*)"initialize",
			(char*)"(Lorg/difft/android/smp/Server;Lorg/difft/android/smp/Config;Lorg/difft/android/smp/Server$InnerServerHandler;)J",
			reinterpret_cast<void*>(_New)
		},
		{
			(char*)"start",
			(char*)"(J)I",
			reinterpret_cast<void*>(_Start)
		},
		{
			(char*)"getStats",
			(char*)"(J)Ljava/util/HashMap;",
			reinterpret_cast<void*>(_GetStats)
		},
		{
			(char*)"close",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Close)
		},
		{
			(char*)"destroy",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Destroy)
		}
	};
	// 注册Native方法
	if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof((methods)[0])) < 0) {
		LogWarn(_LOCAL_, "RegisterNatives error");
		env->DeleteLocalRef(clazz);
		return BC_R_UNEXPECTED;
	}
	env->DeleteLocalRef(clazz);
	return BC_R_SUCCESS;
}

BCRESULT SMPServerWrap::Create(BCFObject* pConfig)
{
	if (!m_pServer)
	{
		m_pServer = new SMPServer();
		if (!m_pServer)
		{
			return BC_R_NOMEMORY;
		}
		if (!IS_BCF_OBJECT(pConfig))
		{
			return BC_R_INVALIDARG;
		}
		BCRESULT result;
		BCFObject sockConfig;

		Runtime::Initialize(pConfig);
		sockConfig.PutBool("ipv6", 0);
		sockConfig.PutString("server_host", "example.com");
		//sockConfig.PutInt("port", server_port);
		//sockConfig.PutString("private_key_file", "./cert/server.key");
		//sockConfig.PutString("certificate_file", "./cert/server.crt");
		sockConfig.PutString("tls_ciphers", XQC_TLS_CIPHERS);
		sockConfig.PutString("tls_groups", MY_TLS_GROUPS);
		sockConfig.PutInt("pacing_on", 0);
		*pConfig += sockConfig;
		result = m_pServer->Create(pConfig, this);
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(m_pServer);
			return result;
		}
		return BC_R_SUCCESS;
	}
	return BC_R_EXISTS;
}

jlong SMPServerWrap::_New(
	JNIEnv* env, 
	jobject obj, 
	jobject jselfObj, 
	jobject jConfig, 
	jobject jHandler)
{
	std::unique_ptr<BCFObject> pConfig(SMPConfig::ConvertFromJava(env, jConfig));
	if (!pConfig)
	{
		return 0;
	}
	SMPServerWrap* pSMPServer = new SMPServerWrap();
	if (!pSMPServer)
	{
		return 0;
	}
	BCRESULT result = pSMPServer->Create(pConfig.get());
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(pSMPServer);
		LogFatal(_LOCAL_, "Failed to create SMPServer, check the arguments.");
		return 0;
	}
	pSMPServer->m_pSelf = env->NewGlobalRef(jselfObj);
	pSMPServer->m_pHandler = env->NewGlobalRef(jHandler);
	jclass handlerCls = env->FindClass("org/difft/android/smp/Server$InnerServerHandler");
	if (handlerCls)
	{
		pSMPServer->m_pHandlerCls = (jclass)env->NewGlobalRef(handlerCls);
		env->DeleteLocalRef(handlerCls);
	}

	return (jlong)pSMPServer;
}

jint SMPServerWrap::_Start(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerWrap* _this = reinterpret_cast<SMPServerWrap*>(handle);
	if (_this && _this->m_pServer)
	{
		return _this->m_pServer->Start();
	}
	return BC_R_INVALIDARG;
}

jobject SMPServerWrap::_GetStats(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerWrap* _this = reinterpret_cast<SMPServerWrap*>(handle);
	if (_this && _this->m_pServer)
	{
		ConnStatS stats;
		_this->m_pServer->GetStats(stats);
		return StlStringSizeMapToJavaHashMap(env, stats.ToMap());
	}
	return NULL;
}

void SMPServerWrap::_Close(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerWrap* _this = reinterpret_cast<SMPServerWrap*>(handle);
	if (_this && _this->m_pServer)
	{
		_this->m_pServer->Close();
	}
}

void SMPServerWrap::_Destroy(JNIEnv* env, jobject obj, jlong handle)
{
	SMPServerWrap* _this = reinterpret_cast<SMPServerWrap*>(handle);
	if (_this)
	{
		delete _this;
	}
}

void SMPServerWrap::_Cleanup()
{
	JNIEnv* pEnv = GetThreadJNIEnv();
	if (m_pSelf && pEnv)
	{
		pEnv->DeleteGlobalRef(m_pSelf);
		m_pSelf = NULL;
	}
	if (m_pHandler && pEnv)
	{
		pEnv->DeleteGlobalRef(m_pHandler);
		m_pHandler = NULL;
	}
	if (m_pHandlerCls && pEnv)
	{
		pEnv->DeleteGlobalRef(m_pHandlerCls);
		m_pHandlerCls = NULL;
	}
	BC_SAFE_DELETE_PTR(m_pServer);
}

void SMPServerWrap::OnNewConn(RecvInfo *pInfo)
{
	// TODO : Notify java layer to make accept new connection choice
	if (m_pServer)
	{
		SMPServerConnectionWrap *pConn = SMPServerConnectionWrap::New();
		if (!pConn)
		{
			return;
		}
		if (m_pHandler && m_pHandlerCls)
		{
			JNIEnv* pEnv = GetThreadJNIEnv();
			jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
				"onNewInitial", "(J)V");
			if (mid)
			{
				pEnv->CallVoidMethod(m_pHandler, mid, (jlong)pConn);
			}
		}
		m_pServer->CreateConnection(pConn, pInfo);
	}
}

void SMPServerWrap::OnAccept(IServerConnectionHandler* handler)
{
}

void SMPServerWrap::OnExecDone(IRPCStub *pStub)
{
}

void SMPServerWrap::OnClosed()
{
	_Cleanup();
	// After deref java object, its jvm's responsibility to call finalize
	// then destroy to delete this agent.
}

void SMPServerWrap::OnException(BCException &except)
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI

// NS_CC_END