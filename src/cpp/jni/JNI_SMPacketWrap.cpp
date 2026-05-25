
///////////////////////////////////////////////////////////////////////////////
// file : JNI_SMPacketWrap.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "JNI_SMPacketWrap.h"
#include "Runtime.h"
#include "jni_utils.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : JNI
///////////////////////////////////////////////////////////////////////////////

namespace JNI
{

///////////////////////////////////////////////////////////////////////////////
// Class : SMPacketWrap
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(SMPacketWrap, 32);

SMPacketWrap::SMPacketWrap()
{
	//
}

SMPacketWrap::~SMPacketWrap()
{
}

BCRESULT SMPacketWrap::Build(JNIEnv* env, jobject jselfObj)
{
	packet_ = MakePacketFromJava(env, jselfObj);
	return BC_R_SUCCESS;
}

BCRESULT SMPacketWrap::Initialize(JNIEnv* env)
{
	jclass clazz = env->FindClass("org/difft/android/smp/Packet");

	static JNINativeMethod methods[] = {
		{
			(char*)"initialize",
			(char*)"()J",
			reinterpret_cast<void*>(_New)
		},
		{
			(char*)"build",
			(char*)"(J)V",
			reinterpret_cast<void*>(_Build)
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

jlong SMPacketWrap::_New(
	JNIEnv* env,
	jobject obj)
{
	SMPacketWrap* pPacket = new SMPacketWrap();
	if (!pPacket)
	{
		return 0;
	}

	return (jlong)pPacket;
}

void SMPacketWrap::_Build(JNIEnv* env, jobject obj, jlong handle)
{
	SMPacketWrap* _this = reinterpret_cast<SMPacketWrap*>(handle);
	if (_this)
	{
		_this->packet_ = MakePacketFromJava(env, obj);
	}
}

void SMPacketWrap::_Destroy(JNIEnv* env, jobject obj, jlong handle)
{
	SMPacketWrap* _this = reinterpret_cast<SMPacketWrap*>(handle);
	if (_this)
	{
		delete _this;
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : JNI
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : JNI

///////////////////////////////////////////////////////////////////////////////
// End of file : JNI_SMPacketWrap.cpp
///////////////////////////////////////////////////////////////////////////////
