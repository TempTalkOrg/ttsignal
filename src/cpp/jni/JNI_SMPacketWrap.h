


#ifndef JNI_SMPACKETWRAP_H_INCLUDED__
#define JNI_SMPACKETWRAP_H_INCLUDED__

#include <jni.h>
#include "BC/Utils.h"
#include "SMPacket.h"


using namespace BC;
using namespace SMP;





namespace JNI
{





class SMPacketWrap
{
	DECLARE_FIXED_ALLOC(SMPacketWrap);
public:
	SMPacketWrap();
	~SMPacketWrap();

	SMPacketPtr				packet_;

	BCRESULT			Build(JNIEnv* env, jobject jselfObj);
	static BCRESULT		Initialize(JNIEnv *env);
protected:
	static	jlong		_New(
							JNIEnv *env, 
							jobject obj);
	static	void		_Build(
							JNIEnv *env, 
							jobject obj, 
							jlong handle);
	static	void		_Destroy(JNIEnv *env, jobject obj, jlong handle);
private:
	DECLARE_NO_COPY_CLASS(SMPacketWrap);
};





} 

#endif 




