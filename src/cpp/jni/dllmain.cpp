///////////////////////////////////////////////////////////////////////////////
// file : dllmain.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#ifdef __LINUX__
#include <sys/resource.h>
#endif // __LINUX__
#include <openssl/ssl.h>
#include "BC/BCLog.h"
#include "Runtime.h"
#include "Utils.h"
#include "jni_utils.h"
#include "JNI_SMPConnectorWrap.h"
#include "JNI_SMPServerWrap.h"
#include "JNI_SMPacketWrap.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI {



///////////////////////////////////////////////////////////////////////////////
// Functions
///////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI


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
	// AddFileLogAppender("smp.log", _FINEST_, true);
	JNI::SMPConnectorWrap::Initialize(env);
	JNI::SMPConnectionWrap::Initialize(env);
	// JNI::SMPServerWrap::Initialize(env);
	// JNI::SMPServerConnectionWrap::Initialize(env);
	JNI::SMPacketWrap::Initialize(env);

	return JNI_VERSION_1_6;
}

JNIEXPORT 
void JNI_OnUnLoad(JavaVM * vm, void* reserved) {
}


//#ifdef _MSC_VER
//BOOL APIENTRY DllMain( 
//	HMODULE hModule,
//	DWORD  ul_reason_for_call,
//	LPVOID lpReserved)
//{
//	switch (ul_reason_for_call)
//	{
//	case DLL_PROCESS_ATTACH:
//		::DisableThreadLibraryCalls(hModule);
//		break;
//	case DLL_THREAD_ATTACH:
//	case DLL_THREAD_DETACH:
//	case DLL_PROCESS_DETACH:
//		break;
//	}
//	return TRUE;
//}
//#endif // _MSC_VER


///////////////////////////////////////////////////////////////////////////////
// End of file : dllmain.cpp
///////////////////////////////////////////////////////////////////////////////
