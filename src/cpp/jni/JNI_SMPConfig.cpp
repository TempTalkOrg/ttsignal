///////////////////////////////////////////////////////////////////////////////
// file : JNI_SMPConfig.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "Utils.h"
#include "jni_utils.h"
#include "JNI_SMPConfig.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

///////////////////////////////////////////////////////////////////////////////
// class : SMPConfig
///////////////////////////////////////////////////////////////////////////////

SMPConfig::SMPConfig()
{
	//
}

SMPConfig::~SMPConfig()
{
}

BCFObject *SMPConfig::ConvertFromJava(JNIEnv *env, jobject obj)
{
	jclass cls = env->FindClass("org/difft/android/smp/Config");
	if (!env->IsInstanceOf(obj, cls))
	{
		env->DeleteLocalRef(cls);
		return NULL;
	}
	BCFObject* pConfig = new BCFObject();
	if (!pConfig)
	{
		return NULL;
	}
	std::string strValue;
	pConfig->PutInt("taskThreads", 
		JniUtils::GetIntField(env, obj, cls, "taskThreads"));
	pConfig->PutInt("timerThreads",
		JniUtils::GetIntField(env, obj, cls, "timerThreads"));
	pConfig->PutInt("idle_time_out",
		JniUtils::GetIntField(env, obj, cls, "idleTimeOut"));
	pConfig->PutString("alpn",
		JniUtils::GetStringField(env, obj, cls, "alpn", strValue));
	pConfig->PutString("hostname",
		JniUtils::GetStringField(env, obj, cls, "hostname", strValue));
	pConfig->PutInt("port",
		JniUtils::GetIntField(env, obj, cls, "port"));
	pConfig->PutInt("backlog",
		JniUtils::GetIntField(env, obj, cls, "backlog"));
	pConfig->PutBool("reuse_port",
		JniUtils::GetBooleanField(env, obj, cls, "reusePort"));
	pConfig->PutInt("max_conn",
		JniUtils::GetIntField(env, obj, cls, "maxConnections"));
	pConfig->PutInt("c_cong_ctl",
		JniUtils::GetIntField(env, obj, cls, "congestCtrl"));
	pConfig->PutBool("ping_on",
		JniUtils::GetBooleanField(env, obj, cls, "pingOn"));
	pConfig->PutInt("ping_interval",
		JniUtils::GetIntField(env, obj, cls, "pingInterval"));
	// SSL
	pConfig->PutBool("ssl",
		JniUtils::GetBooleanField(env, obj, cls, "ssl"));
	pConfig->PutString("private_key_file",
		JniUtils::GetStringField(env, obj, cls, "privateKeyFile", strValue));
	pConfig->PutString("certificate_file",
		JniUtils::GetStringField(env, obj, cls, "certificateFile", strValue));
    JniUtils::GetStringField(env, obj, cls, "logFile", strValue);
    if (!strValue.empty()) {
        pConfig->PutString("log_file", strValue);
    }
	pConfig->PutInt("log_level",
		JniUtils::GetIntField(env, obj, cls, "logLevel"));
	env->DeleteLocalRef(cls);
	return pConfig;
}

LogHandler *SMPConfig::GetLogHandler(JNIEnv *env, jobject obj)
{ 
	jclass cls = env->FindClass("org/difft/android/smp/Config");
	if (!env->IsInstanceOf(obj, cls))
	{
		env->DeleteLocalRef(cls);
		return NULL;
	}
	jobject logHandler = JniUtils::GetObjectField(env, obj, cls, "logHandler", 
		"Lorg/difft/android/smp/Config$LogHandler;");
	env->DeleteLocalRef(cls);
	if (logHandler == NULL)
	{
		return NULL;
	}
	return new LogHandler(env, logHandler);
}

///////////////////////////////////////////////////////////////////////////////
// Class : LogHandler
///////////////////////////////////////////////////////////////////////////////

LogHandler::LogHandler(JNIEnv *env, jobject obj)
	: m_env(env)
	, m_logHandler(NULL)
	, m_logHandlerClass(NULL)
	, m_logMethod(NULL)
{
	m_logHandler = env->NewGlobalRef(obj);
	jclass handlerCls = env->FindClass("org/difft/android/smp/Config$LogHandler");
	if (handlerCls)
	{
		m_logHandlerClass = (jclass)env->NewGlobalRef(handlerCls);
		env->DeleteLocalRef(handlerCls);
	}
	m_logMethod = env->GetMethodID(m_logHandlerClass, "log", "(ILjava/lang/String;)V");
}

LogHandler::~LogHandler()
{
	if (m_logHandler)
	{
		m_env->DeleteGlobalRef(m_logHandler);
		m_logHandler = NULL;
	}
	if (m_logHandlerClass)
	{
		m_env->DeleteGlobalRef(m_logHandlerClass);
		m_logHandlerClass = NULL;
	}
	m_logMethod = NULL;
}

void LogHandler::Log(int level, const char *message)
{
	if (m_logHandler && m_logMethod && m_env)
	{
		JNIEnv* pEnv = GetThreadJNIEnv();
        KBPool pool;
        char *msg = (char*)pool.Strdup(message);
        char *s = msg;
        while (*s) {
            if ((unsigned char)*s > 127) *s = '?';
            s++;
        }
		jstring jmessage = pEnv->NewStringUTF(msg);
		pEnv->CallVoidMethod(m_logHandler, m_logMethod, level, jmessage);
		pEnv->DeleteLocalRef(jmessage);
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI
