///////////////////////////////////////////////////////////////////////////////
// file : jni_utils.h
// author : anto
///////////////////////////////////////////////////////////////////////////////

#ifndef JNI_UTILS_H_INCLUDED__
#define JNI_UTILS_H_INCLUDED__

#include <jni.h>
#include <map>
#include <string>
#include <memory>
#include <BC/Utils.h>
#include <BC/BCBuffer.h>
#include "SMPacket.h"


using namespace BC;
using namespace SMP;

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////

#define LOGTAG			"*QUICPRO*"

jint InitGlobalJniVariables(JavaVM* jvm);

JavaVM* GetJVM();

JNIEnv* GetThreadJNIEnv();

// std::string JStrToCStr(jstring jstr);
std::string JStrToCStr(JNIEnv *pEnv, jstring jstr);

void JavaHashMapToStlStringStringMap(
	JNIEnv* env,
	jobject hashMap,
	std::map<std::string, std::string>& mapOut);

jobject StlStringStringMapToJavaHashMap(
	JNIEnv* env,
	const std::map<std::string, std::string>& map);

jobject StlStringSizeMapToJavaHashMap(
	JNIEnv* env,
	const std::map<std::string, size_t>& map);

std::string GetJavaClassName(JNIEnv* env, jobject obj);

std::shared_ptr<BCBuffer> MakeBufferFromByteArray(
	JNIEnv* env,
	jbyteArray jbytearray);

std::shared_ptr<std::vector<uint8_t>> MakeArrayFromByteArray(
	JNIEnv* env,
	jbyteArray jbytearray);

std::shared_ptr<BCBuffer> MakeBufferFromByteBuffer(
	JNIEnv* env,
	jobject jbytebuffer);

std::shared_ptr<BCBuffer> MakeBufferFromString(
	JNIEnv* env,
	jstring jstr);

SMPacketPtr MakePacketFromJava(JNIEnv* env, jobject obj);

///////////////////////////////////////////////////////////////////////////////
// Class : JVMScoped
///////////////////////////////////////////////////////////////////////////////

class JVMScoped
{
public:
	JVMScoped();
	JVMScoped(JNIEnv **env);
	~JVMScoped();

	bool		Attach(JNIEnv** env);
	JNIEnv	*	GetEnv() {
		return _env;
	}
private:
	DECLARE_NO_COPY_CLASS(JVMScoped);
	bool			_attached;
	JNIEnv		*	_env;
};

///////////////////////////////////////////////////////////////////////////////
// Class : JniGlobalRef
///////////////////////////////////////////////////////////////////////////////

class JniGlobalRef
{
public:
	JniGlobalRef(JNIEnv *env, jobject obj) : env_(env), obj_(NULL) {
		if (env && obj) {
			obj_ = env->NewGlobalRef(obj);
		}
	}
	JniGlobalRef(const JniGlobalRef &other) : obj_(NULL) {
		operator=(other);
	}
	~JniGlobalRef() {
		if (obj_ && env_) {
			env_->DeleteGlobalRef(obj_);
		}
	}

	JniGlobalRef& operator=(const JniGlobalRef& other) {
		if (env_ && obj_) {
			env_->DeleteGlobalRef(obj_);
			obj_ = NULL;
		}
		if (other.obj_) {
			obj_ = env_->NewGlobalRef(other.obj_);
		}
		return *this;
	}

	operator bool() const {
		return obj_ != NULL;
	}

	operator jobject() const {
		return obj_;
	}

	operator jclass() const {
		return static_cast<jclass>(obj_);
	}

	jobject GetObject() const {
		return obj_;
	}

private:
	JNIEnv		*	env_;
	jobject			obj_;
};

///////////////////////////////////////////////////////////////////////////////
// class : JniUtils
///////////////////////////////////////////////////////////////////////////////

class JniUtils
{
public:
    JniUtils();
    ~JniUtils();

	static jchar	GetCharField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jbyte	GetByteField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jshort	GetShortField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jint		GetIntField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jint		GetIntArrayField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						std::vector<int32_t> &out);
	static jlong	GetLongField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jfloat	GetFloatField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jdouble	GetDoubleField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static jboolean	GetBooleanField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp);
	static const char*	GetStringField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						std::string &outStr);
	static jobject	GetObjectField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						const char* lpszPropType);
	static int		SetIntField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						int nValue);
	static int		SetBooleanField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						bool bValue);
	static int		SetStringField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						const char* lpszValue);
	static int		SetObjectField(
						JNIEnv *pEnv, 
						jobject jobj, 
						jclass cls, 
						const char* lpszProp,
						const char* lpszPropType,
						jobject jobjValue);

	static jmethodID GetMethodID (
						JNIEnv* jni, 
						jclass c, 
						const char* name, 
						const char* signature);

	static jfieldID	GetFieldID(
						JNIEnv* jni, 
						jclass c, 
						const char* name, 
						const char* signature);
	static void		UnCacheClass(JNIEnv *pEnv);
	static jclass	FindClass(JNIEnv *pEnv, const char* lpszCls);
	static void		CacheClass(JNIEnv *pEnv);
	static void		ThrowException(JNIEnv *pEnv, BCRESULT err);
	static void		ThrowException(JNIEnv *pEnv, const char* lpszMsg);
protected:
	static void		_CacheClass(JNIEnv *pEnv, const char* lpszCls);
private:
	DECLARE_NO_COPY_CLASS(JniUtils);
	typedef std::map<std::string, jobject>	ClassMap;
	static ClassMap		globalClsMap;
};

///////////////////////////////////////////////////////////////////////////////
// class : JniByteBuffer
///////////////////////////////////////////////////////////////////////////////

class JniByteBuffer
{
public:
	JniByteBuffer(JNIEnv* env);
	JniByteBuffer(JNIEnv* env, const void *data, size_t size);
	~JniByteBuffer();

	size_t			SetSize(size_t size);
	void			Rewind();
	size_t			Update(const void* data, size_t size);

	JNIEnv		*	env_;
	jobject         byteArrayObj_;
	uint8_t		*	buffer_;
	size_t			size_;
};

///////////////////////////////////////////////////////////////////////////////
// class : JniByteArray
///////////////////////////////////////////////////////////////////////////////

class JniByteArray
{
public:
	JniByteArray(JNIEnv* env);
	JniByteArray(JNIEnv* env, const void *data, size_t size);
	~JniByteArray();

	size_t			Update(const void* data, size_t size);

	JNIEnv		*	env_;
	jbyteArray      byteArrayObj_;
};

///////////////////////////////////////////////////////////////////////////////
// class : JniStrStrHashMap
///////////////////////////////////////////////////////////////////////////////

class JniStrStrHashMap
{
public:
	JniStrStrHashMap(JNIEnv *env);
	~JniStrStrHashMap();

	BCRESULT		Put(const char* key, const char *value);

	JNIEnv		*	env_;
	jobject         hashMapObj_;
};


#endif // JNI_UTILS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : jni_utils.h
///////////////////////////////////////////////////////////////////////////////

