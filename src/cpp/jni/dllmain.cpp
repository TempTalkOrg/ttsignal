



#include "StdAfx.h"
#ifdef __LINUX__
#include <sys/resource.h>
#endif 
#include <openssl/ssl.h>
#include "BC/BCLog.h"
#include "Runtime.h"
#include "Utils.h"
#include "jni_utils.h"
#include "JNI_SMPConnectorWrap.h"
#include "JNI_SMPServerWrap.h"
#include "JNI_SMPacketWrap.h"
#include "JNI_HttpConnectorWrap.h"






namespace JNI {













} 


JNIEXPORT 
jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
	JNIEnv* env;
	if (JNI_OK != vm->GetEnv(reinterpret_cast<void**> (&env), JNI_VERSION_1_6)) {
		LogWarn(_LOCAL_, "JNI_OnLoad could not get JNI env");
		return JNI_ERR;
	}

	OPENSSL_init_ssl(
		OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
	InitGlobalJniVariables(vm);
	
	JNI::SMPConnectorWrap::Initialize(env);
	JNI::SMPConnectionWrap::Initialize(env);
	
	
	JNI::SMPacketWrap::Initialize(env);
	JNI::HttpConnectorWrap::Initialize(env);

	return JNI_VERSION_1_6;
}

JNIEXPORT 
void JNI_OnUnLoad(JavaVM * vm, void* reserved) {
}


























