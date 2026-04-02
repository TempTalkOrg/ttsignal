///////////////////////////////////////////////////////////////////////////////
// file : SMPConnectorWrap.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "BC/BCJson.h"
#include "Utils.h"
#include "SMPConnector.h"
#include "Runtime.h"
#include "jni_utils.h"
#include "JNI_SMPConfig.h"
#include "JNI_SMPConnectorWrap.h"
#include "JNI_SMPacketWrap.h"
#include <http-parser/http_parser.h>


#define MY_TLS_GROUPS "X25519:P-384:P-521"



enum
{
	JCM_HSK_FINISH			= 1,
	JCM_EXEC_DONE			= 2,
	JCM_RECV_CMD			= 3,
	JCM_RECV_DATA			= 4,
	JCM_CLOSE				= 5,
};

///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnectionWrap
///////////////////////////////////////////////////////////////////////////////

SMPConnectionWrap::SMPConnectionWrap()
	: m_pConn(NULL)
	, m_pSelf(NULL)
	, m_pHandler(NULL)
	, m_pHandlerCls(NULL)
{
	//
}

SMPConnectionWrap::~SMPConnectionWrap()
{
	//
}

BCRESULT SMPConnectionWrap::Initialize(JNIEnv *env)
{
	jclass clazz = env->FindClass("org/difft/android/smp/Connection");

	static JNINativeMethod methods[] = {
		{
			(char*)"initialize",
			(char*)"(JLorg/difft/android/smp/Connection;Lorg/difft/android/smp/Connection$IHandler;)J",
			reinterpret_cast<void*>(_Initialize)
		},
		{
			(char*)"connect",
			(char*)"(JLjava/lang/String;Ljava/lang/String;I)I",
			reinterpret_cast<void*>(_Connect)
		},
		{
			(char*)"sendPacket",
			(char*)"(JJ)I",
			reinterpret_cast<void*>(_SendPacket)
		},
		{
			(char*)"restart",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Restart)
		},
		{
			(char*)"close",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Close)
		},
		{
			(char*)"closeStream",
			(char*)"(JI)V",
			reinterpret_cast<void*>(_CloseStream)
		},
		{
			(char*)"destroy",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Destroy)
		}
	};
	// register native methods
	if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof((methods)[0])) < 0) {
		LogWarn(_LOCAL_, "RegisterNatives error");
		env->DeleteLocalRef(clazz);
		return BC_R_UNEXPECTED;
	}
	env->DeleteLocalRef(clazz);
	return BC_R_SUCCESS;
}

jlong SMPConnectionWrap::New(BCFObject* pConfig)
{
	SMPConnectionWrap* pConnWrap = new SMPConnectionWrap();
	BCRESULT result = pConnWrap->Create(pConfig);
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(pConnWrap);
		return 0;
	}
	return (jlong)pConnWrap;
}

BCRESULT SMPConnectionWrap::Create(BCFObject *pConfig)
{
	return BC_R_SUCCESS;
}

void SMPConnectionWrap::SetConnection(SMPConnection* pConn)
{
	m_pConn = pConn;
}

jlong SMPConnectionWrap::_Initialize(
	JNIEnv* env, 
	jobject, 
	jlong handle, 
	jobject connObj, 
	jobject listener)
{
	if (handle == 0)
	{
		return 0;
	}
	SMPConnectionWrap* _this = reinterpret_cast<SMPConnectionWrap*>(handle);
	_this->m_pSelf = env->NewGlobalRef(connObj);
	_this->m_pHandler = env->NewGlobalRef(listener);
	jclass handlerCls = env->FindClass("org/difft/android/smp/Connection$IHandler");
	if (handlerCls)
	{
		_this->m_pHandlerCls = (jclass)env->NewGlobalRef(handlerCls);
		env->DeleteLocalRef(handlerCls);
	}
	return handle;
}

jint SMPConnectionWrap::_Connect(
	JNIEnv* env, 
	jobject obj, 
	jlong handle, 
	jstring jurl, 
	jstring jprops,
	jint timeout_ms)
{
	std::string url_str = JStrToCStr(env, jurl);
	std::string prop_str = JStrToCStr(env, jprops);

    Json::Reader reader;
    Json::Value props;
    if (!reader.parse(prop_str, props) || !props.isObject()) {
        JniUtils::ThrowException(env, "Invalid properties.");
		return BC_R_INVALIDARG;
    }

	url_str = EncodeURI(url_str);
	// Parse URL
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url_str.c_str(), url_str.length(), 0, &u) != 0) {
        JniUtils::ThrowException(env, "Invalid URL.");
		return BC_R_INVALIDARG;
    }
	std::string scheme = u.field_set & (1 << UF_SCHEMA) ? url_str.substr(u.field_data[UF_SCHEMA].off, u.field_data[UF_SCHEMA].len) : "";
    std::string host = u.field_set & (1 << UF_HOST) ? url_str.substr(u.field_data[UF_HOST].off, u.field_data[UF_HOST].len) : "";
    uint16_t port = u.field_set & (1 << UF_PORT) ? u.port : (scheme == "https" ? 443 : 80);
	std::string path = u.field_set & (1 << UF_PATH) ? url_str.substr(u.field_data[UF_PATH].off, u.field_data[UF_PATH].len) : "";
	std::string query = u.field_set & (1 << UF_QUERY) ? url_str.substr(u.field_data[UF_QUERY].off, u.field_data[UF_QUERY].len) : "";

	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		BCRESULT result = BC_R_UNEXPECTED;
		RPCStub *pStub = _this->m_sLocalStubMgr.GetStub();
		if (!pStub) {
			JniUtils::ThrowException(env, "Failed to allocate stub.");
			return BC_R_FAILURE;
		}
		else {
			if (BC_R_SUCCESS != pStub->Create(kConnect.Ptr)) {
				_this->m_sLocalStubMgr.PutStub(pStub->m_nTransId);
			}
			else {
				pStub->m_lParams[0] = (uint64_t)pStub->m_sPool.Strdup(scheme.c_str());
				pStub->m_lParams[1] = (uint64_t)pStub->m_sPool.Strdup(host.c_str());
				pStub->m_lParams[2] = port;
				pStub->m_lParams[3] = (uint64_t)pStub->m_sPool.Strdup(path.c_str());
				pStub->m_lParams[4] = (uint64_t)pStub->m_sPool.Strdup(query.c_str());
				pStub->m_lParams[5] = (uint64_t)pStub->m_sPool.Strdup(prop_str.c_str());
				pStub->m_lParams[6] = timeout_ms;
				result = _this->m_pConn->Connect(pStub);
				if (result != BC_R_SUCCESS) {
					JniUtils::ThrowException(env, "Failed to call SMPConnection::Connect.");
				}
			}
		}
		return result;
	}
	return BC_R_FAILURE;
}

jint SMPConnectionWrap::_SendPacket(
	JNIEnv* env, 
	jobject obj, 
	jlong handle, 
	jlong packet)
{
	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	SMPacketWrap* _packet = reinterpret_cast<SMPacketWrap*>(packet);
	if (_this && _this->m_pConn && _packet && _packet->packet_)
	{
		return _this->m_pConn->SendPacket(_packet->packet_);
	}
	return BC_R_INVALIDARG;
}

void SMPConnectionWrap::_Restart(JNIEnv* env, jobject obj, jlong handle)
{
	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		_this->m_pConn->Restart();
	}
}

void SMPConnectionWrap::_Close(JNIEnv* env, jobject obj, jlong handle)
{
	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		_this->m_pConn->Close();
	}
}

void SMPConnectionWrap::_CloseStream(JNIEnv* env, jobject obj, jlong handle, jint nStreamId)
{
	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	if (_this && _this->m_pConn)
	{
		_this->m_pConn->CloseStream(nStreamId);
	}
}

void SMPConnectionWrap::_Destroy(JNIEnv* env, jobject obj, jlong handle)
{
	SMPConnectionWrap *_this = reinterpret_cast<SMPConnectionWrap*>(handle);
	if (_this)
	{
		delete _this;
	}
}

void SMPConnectionWrap::_Cleanup(JNIEnv *env)
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
	BC_SAFE_DELETE_PTR(m_pConn);
	m_sLocalStubMgr.Clear();
}

void SMPConnectionWrap::OnHandshakeFinished()
{
}

void SMPConnectionWrap::OnExecDone(IRPCStub *pStub)
{
	if (pStub)
	{
        if (kConnect.Equal(pStub->m_szCmd))
        {
            if (m_pHandler && m_pHandlerCls)
            {
                JNIEnv* pEnv = GetThreadJNIEnv();
                BCRESULT result = pStub->m_result;
                jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
                    "onConnectResult", "(Lorg/difft/android/smp/Connection;ILjava/lang/String;)V");
                if (mid)
                {
					if (pStub->m_lParams[0] && pStub->m_lParams[1])
					{
						std::string strMsg((const char*)pStub->m_lParams[0], pStub->m_lParams[1]);
						jstring jmsg = pEnv->NewStringUTF(strMsg.c_str());
						pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, result, jmsg);
						pEnv->DeleteLocalRef(jmsg);
					}
					else
					{
						pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, result, NULL);
					}
                }
            }
        }
		m_sLocalStubMgr.PutStub(pStub->m_nTransId, FALSE);
	}
}

void SMPConnectionWrap::OnStreamCreated(uint32_t nStreamId)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onStreamCreated", "(Lorg/difft/android/smp/Connection;I)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, nStreamId);
		}
	}
}

void SMPConnectionWrap::OnStreamClosed(uint32_t nStreamId)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onStreamClosed", "(Lorg/difft/android/smp/Connection;I)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, nStreamId);
		}
	}
}

void SMPConnectionWrap::OnRecvCmd(
	const SMPHeader& refHeader,
	LPCSTR lpszCmd, 
	size_t msg_size)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		JniByteArray jbuf(pEnv, lpszCmd, msg_size);
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onRecvCmd", "(Lorg/difft/android/smp/Connection;JII[B)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, refHeader.m_nTimestamp, 
				refHeader.m_nTransId, refHeader.m_nStreamId, jbuf.byteArrayObj_);
		}
	}
}

void SMPConnectionWrap::OnRecvData(
	const SMPHeader& refHeader,
	LPCVOID lpszMsg, 
	size_t msg_size)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		JniByteArray jbuf(pEnv, lpszMsg, msg_size);
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onRecvData", "(Lorg/difft/android/smp/Connection;JII[B)V");
		if (mid)
		{
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, refHeader.m_nTimestamp, 
				refHeader.m_nTransId, refHeader.m_nStreamId, jbuf.byteArrayObj_);
		}
	}
}

void SMPConnectionWrap::OnRestart(BCRESULT result, const char * lpszAddr)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onRestart", "(Lorg/difft/android/smp/Connection;ILjava/lang/String;)V");
		if (mid)
		{
			jstring jAddress = NULL;
			if (lpszAddr)
			{
				jAddress = pEnv->NewStringUTF(lpszAddr);
			}
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, result, jAddress);
			pEnv->DeleteLocalRef(jAddress);
		}
	}
}

void SMPConnectionWrap::OnClosed(LPCSTR strReason)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onClosed", "(Lorg/difft/android/smp/Connection;Ljava/lang/String;)V");
		if (mid)
		{
			jstring jmsg = pEnv->NewStringUTF(strReason);
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, jmsg);
			pEnv->DeleteLocalRef(jmsg);
		}
		_Cleanup(pEnv);
		// After deref java object, its jvm's responsibility to call finalize
		// then destroy to delete this agent.
	}
}

void SMPConnectionWrap::OnException(BCException &except)
{
	if (m_pHandler && m_pHandlerCls)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
		jmethodID mid = JniUtils::GetMethodID(pEnv, m_pHandlerCls,
			"onException", "(Lorg/difft/android/smp/Connection;Ljava/lang/String;)V");
		if (mid)
		{
			jstring jmsg = pEnv->NewStringUTF(except.GetMsg().c_str());
			pEnv->CallVoidMethod(m_pHandler, mid, m_pSelf, jmsg);
			pEnv->DeleteLocalRef(jmsg);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnectorWrap
///////////////////////////////////////////////////////////////////////////////

SMPConnectorWrap::SMPConnectorWrap()
	: m_pConnector(NULL)
	, m_pSelf(NULL)
{
	//
}

SMPConnectorWrap::~SMPConnectorWrap()
{
	_Cleanup();
}

BCRESULT SMPConnectorWrap::Initialize(JNIEnv* env)
{
	jclass clazz = env->FindClass("org/difft/android/smp/Connector");

	static JNINativeMethod methods[] = {
		{
			(char*)"initialize",
			(char*)"(Lorg/difft/android/smp/Connector;Lorg/difft/android/smp/Config;)J",
			reinterpret_cast<void*>(_New)
		},
		{
			(char*)"createConnection",
			(char*)"(JLorg/difft/android/smp/Config;)J",
			reinterpret_cast<void*>(_CreateConnection)
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
	// register native methods
	if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof((methods)[0])) < 0) {
		LogWarn(_LOCAL_, "RegisterNatives error");
		env->DeleteLocalRef(clazz);
		return BC_R_UNEXPECTED;
	}
	env->DeleteLocalRef(clazz);
	return BC_R_SUCCESS;
}

BCRESULT SMPConnectorWrap::Create(BCFObject *pConfig, LogHandler *pLogHandler)
{
    std::unique_ptr<LogHandler> pLocalLogHandler(pLogHandler);
	if (!m_pConnector)
	{
		m_pConnector = new SMPConnector();
		if (!m_pConnector)
		{
			return BC_R_NOMEMORY;
		}
		BCRESULT result;
		BCFObject sockConfig;

		Runtime::Initialize(pConfig);
		sockConfig.PutBool("ipv6", 0);
		sockConfig.PutString("server_host", "example.com");
		//sockConfig.PutInt("port", server_port);
		/* client does not need to fill in private_key_file & certificate_file */
		sockConfig.PutString("tls_ciphers", XQC_TLS_CIPHERS);
		sockConfig.PutString("tls_groups", MY_TLS_GROUPS);
		sockConfig.PutInt("pacing_on", 0);
		*pConfig += sockConfig;
		result = m_pConnector->Create(pConfig, this);
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(m_pConnector);
			return result;
		}
        m_pLogHandler = std::move(pLocalLogHandler);
		return BC_R_SUCCESS;
	}
	return BC_R_EXISTS;
}

jlong SMPConnectorWrap::_New(JNIEnv* env, jobject obj, jobject selfObj, jobject config)
{
	std::unique_ptr<BCFObject> pConfig(SMPConfig::ConvertFromJava(env, config));
	if (!pConfig)
	{
		return 0;
	}
	LogHandler *pLogHandler = SMPConfig::GetLogHandler(env, config);
	auto pSMPConnector = new SMPConnectorWrap();
	BCRESULT result = pSMPConnector->Create(pConfig.get(), pLogHandler);
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(pSMPConnector);
		JniUtils::ThrowException(env, "Failed to create SMPConnector, check the arguments.");
		return 0;
	}
	pSMPConnector->m_pSelf = env->NewGlobalRef(selfObj);

	return (jlong)pSMPConnector;
}

jlong SMPConnectorWrap::_CreateConnection(
	JNIEnv* env, jobject,
	jlong connectorHandle,
	jobject config)
{
	std::unique_ptr<BCFObject> pConfig(SMPConfig::ConvertFromJava(env, config));
	if (!pConfig)
	{
		return 0;
	}
	SMPConnectorWrap *_this = (SMPConnectorWrap*)connectorHandle;
	if (_this && _this->m_pConnector)
	{
		jlong lConnWrap = SMPConnectionWrap::New(pConfig.get());
		SMPConnectionWrap* pConnWrap = reinterpret_cast<SMPConnectionWrap*>(lConnWrap);
		SMPConnection* pConn = _this->m_pConnector->CreateConnection(pConnWrap, pConfig.get());
		pConnWrap->SetConnection(pConn);
		return lConnWrap;
	}
	return 0;
}

jobject SMPConnectorWrap::_GetStats(JNIEnv* env, jobject obj, jlong handle)
{
	SMPConnectorWrap* _this = reinterpret_cast<SMPConnectorWrap*>(handle);
	if (_this && _this->m_pConnector)
	{
		ConnStatS stats;
		_this->m_pConnector->GetStats(stats);
		return StlStringSizeMapToJavaHashMap(env, stats.ToMap());
	}
	return NULL;
}

void SMPConnectorWrap::_Close(JNIEnv* env, jobject, jlong handle)
{
	SMPConnectorWrap *_this = reinterpret_cast<SMPConnectorWrap*>(handle);
	if (_this && _this->m_pConnector)
	{
		_this->m_pConnector->Close();
	}
}

void SMPConnectorWrap::_Destroy(JNIEnv* env, jobject, jlong handle)
{
	SMPConnectorWrap *_this = reinterpret_cast<SMPConnectorWrap*>(handle);
	if (_this)
	{
		delete _this;
	}
}

void SMPConnectorWrap::_Cleanup()
{
	JNIEnv* pEnv = GetThreadJNIEnv();
	if (m_pSelf && pEnv)
	{
		pEnv->DeleteGlobalRef(m_pSelf);
		m_pSelf = NULL;
	}
	BC_SAFE_DELETE_PTR(m_pConnector);
	m_sLocalStubMgr.Clear();
}

void SMPConnectorWrap::OnExecDone(IRPCStub* pStub)
{
}

void SMPConnectorWrap::OnClosed()
{
	_Cleanup();
	// After deref java object, its jvm's responsibility to call finalize
	// then destroy to delete this agent.
}

void SMPConnectorWrap::OnException(BCException& except)
{
	//
}

void SMPConnectorWrap::OnLog(int level, LPCSTR lpszMsg)
{
	if (m_pLogHandler)
	{
		m_pLogHandler->Log(level, lpszMsg);
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI
