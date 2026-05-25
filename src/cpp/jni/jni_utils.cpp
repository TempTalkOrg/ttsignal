///////////////////////////////////////////////////////////////////////////////
// file : jni_utils.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include "jni_utils.h"
//#include <unistd.h>
#include <pthread.h>
#include <BC/BCLog.h>
#include "SMPParser.h"


static JavaVM* g_jvm = nullptr;
static pthread_once_t g_jni_ptr_once = PTHREAD_ONCE_INIT;

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////


// Key for per-thread JNIEnv* data.  Non-NULL in threads attached to |g_jvm| by
// AttachCurrentThreadIfNeeded(), NULL in unattached threads and threads that
// were attached by the JVM because of a Java->native call.
static pthread_key_t g_jni_ptr;

JavaVM* GetJVM() {
	return g_jvm;
}

// Return a |JNIEnv*| usable on this thread or NULL if this thread is detached.
JNIEnv* GetEnv() {
	void* env = nullptr;
	jint status = g_jvm->GetEnv(&env, JNI_VERSION_1_4);
	if (status != JNI_OK || env == nullptr)
	{
		return nullptr;
	}
	return reinterpret_cast<JNIEnv*>(env);
}

static void ThreadDestructor(void* prev_jni_ptr) {
	JVMScoped* scope = (JVMScoped*)prev_jni_ptr;
	if (scope)
	{
		delete scope;
	}
}

static void CreateJNIPtrKey() {
	pthread_key_create(&g_jni_ptr, &ThreadDestructor);
}

jint InitGlobalJniVariables(JavaVM* jvm) {
	g_jvm = jvm;

	pthread_once(&g_jni_ptr_once, &CreateJNIPtrKey);

	JNIEnv* jni = nullptr;
	if (jvm->GetEnv(reinterpret_cast<void**>(&jni), JNI_VERSION_1_4) != JNI_OK)
		return -1;

	JniUtils::CacheClass(jni);

	return JNI_VERSION_1_4;
}

JNIEnv* GetThreadJNIEnv()
{
	if (g_jni_ptr)
	{
		JVMScoped* scope = (JVMScoped*)pthread_getspecific(g_jni_ptr);
		if (!scope)
		{
			scope = new JVMScoped(NULL);
			pthread_setspecific(g_jni_ptr, scope);
		}
		return scope->GetEnv();
	}
	return NULL;
}

// std::string JStrToCStr(jstring jstr)
// {
// 	if (jstr)
// 	{
// 		JNIEnv *env;
// 		JVMScoped js(&env);
// 		const char* jchars = env->GetStringUTFChars(jstr, NULL);
// 		size_t jstrLen = env->GetStringUTFLength(jstr);
// 		std::string cstr(jchars, jstrLen);
// 		env->ReleaseStringUTFChars(jstr, jchars);
// 		return cstr;
// 	}
// 	return "";
// }

std::string JStrToCStr(JNIEnv *pEnv, jstring jstr)
{
	if (jstr && pEnv)
	{
		const char* jchars = pEnv->GetStringUTFChars(jstr, NULL);
		size_t jstrLen = pEnv->GetStringUTFLength(jstr);
		std::string cstr(jchars, jstrLen);
		pEnv->ReleaseStringUTFChars(jstr, jchars);
		return cstr;
	} else {
		LogError(_LOCAL_, "QuicPro error: pEnv: %p, str: %p\n", pEnv, jstr);
	}

	return "";
}

jobject StlStringStringMapToJavaHashMap(
	JNIEnv* env, 
	const std::map<std::string, std::string>& map) 
{
	jclass mapClass = env->FindClass("java/util/HashMap");
	if (mapClass == NULL)
		return NULL;

	jmethodID init = env->GetMethodID(mapClass, "<init>", "()V");
	jobject hashMap = env->NewObject(mapClass, init);
	jmethodID put = env->GetMethodID(mapClass, "put", 
						"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

	for (auto &citr : map) {
		jstring keyJava = env->NewStringUTF(citr.first.c_str());
		jstring valueJava = env->NewStringUTF(citr.second.c_str());

		env->CallObjectMethod(hashMap, put, keyJava, valueJava);

		env->DeleteLocalRef(keyJava);
		env->DeleteLocalRef(valueJava);
	}

	jobject hashMapGobal = static_cast<jobject>(env->NewGlobalRef(hashMap));
	env->DeleteLocalRef(hashMap);
	env->DeleteLocalRef(mapClass);

	return hashMapGobal;
}

jobject StlStringSizeMapToJavaHashMap(
	JNIEnv* env, 
	const std::map<std::string, size_t>& map) 
{
	jclass mapClass = env->FindClass("java/util/HashMap");
	jclass integerCls = env->FindClass("java/lang/Integer");
	if (mapClass == NULL || integerCls == NULL)
		return NULL;

	jmethodID init = env->GetMethodID(mapClass, "<init>", "()V");
	jobject hashMap = env->NewObject(mapClass, init);
	jmethodID put = env->GetMethodID(mapClass, "put", 
						"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
	jmethodID integerCls_valueOf = env->GetStaticMethodID(integerCls, "valueOf", 
		"(I)Ljava/lang/Integer;");
	if (!put)
	{
		return NULL;
	}
	for (auto &citr : map) {
		jstring keyJava = env->NewStringUTF(citr.first.c_str());
		jobject valueJava = env->CallStaticObjectMethod(integerCls, integerCls_valueOf, citr.second);

		env->CallObjectMethod(hashMap, put, keyJava, valueJava);

		env->DeleteLocalRef(keyJava);
		env->DeleteLocalRef(valueJava);
	}

	jobject hashMapGobal = static_cast<jobject>(env->NewGlobalRef(hashMap));
	env->DeleteLocalRef(hashMap);
	env->DeleteLocalRef(mapClass);

	return hashMapGobal;
}

// Based on android platform code from: /media/jni/android_media_MediaMetadataRetriever.cpp
void JavaHashMapToStlStringStringMap(
	JNIEnv* env, 
	jobject hashMap, 
	std::map<std::string, std::string>& mapOut) 
{
	// Get the Map's entry Set.
	jclass mapClass = env->FindClass("java/util/Map");
	if (mapClass == NULL) {
		return;
	}
	jmethodID entrySet =
		env->GetMethodID(mapClass, "entrySet", "()Ljava/util/Set;");
	if (entrySet == NULL) {
		return;
	}
	jobject set = env->CallObjectMethod(hashMap, entrySet);
	if (set == NULL) {
		return;
	}
	// Obtain an iterator over the Set
	jclass setClass = env->FindClass("java/util/Set");
	if (setClass == NULL) {
		return;
	}
	jmethodID iterator =
		env->GetMethodID(setClass, "iterator", "()Ljava/util/Iterator;");
	if (iterator == NULL) {
		return;
	}
	jobject iter = env->CallObjectMethod(set, iterator);
	if (iter == NULL) {
		return;
	}
	// Get the Iterator method IDs
	jclass iteratorClass = env->FindClass("java/util/Iterator");
	if (iteratorClass == NULL) {
		return;
	}
	jmethodID hasNext = env->GetMethodID(iteratorClass, "hasNext", "()Z");
	if (hasNext == NULL) {
		return;
	}
	jmethodID next =
		env->GetMethodID(iteratorClass, "next", "()Ljava/lang/Object;");
	if (next == NULL) {
		return;
	}
	// Get the Entry class method IDs
	jclass entryClass = env->FindClass("java/util/Map$Entry");
	if (entryClass == NULL) {
		return;
	}
	jmethodID getKey =
		env->GetMethodID(entryClass, "getKey", "()Ljava/lang/Object;");
	if (getKey == NULL) {
		return;
	}
	jmethodID getValue =
		env->GetMethodID(entryClass, "getValue", "()Ljava/lang/Object;");
	if (getValue == NULL) {
		return;
	}
	// Iterate over the entry Set
	while (env->CallBooleanMethod(iter, hasNext)) {
		jobject entry = env->CallObjectMethod(iter, next);
		jstring key = (jstring)env->CallObjectMethod(entry, getKey);
		jstring value = (jstring)env->CallObjectMethod(entry, getValue);
		const char* keyStr = env->GetStringUTFChars(key, NULL);
		if (!keyStr) {  // Out of memory
			return;
		}
		const char* valueStr = env->GetStringUTFChars(value, NULL);
		if (!valueStr) {  // Out of memory
			env->ReleaseStringUTFChars(key, keyStr);
			return;
		}

		mapOut.insert(std::make_pair(std::string(keyStr), std::string(valueStr)));

		env->DeleteLocalRef(entry);
		env->ReleaseStringUTFChars(key, keyStr);
		env->DeleteLocalRef(key);
		env->ReleaseStringUTFChars(value, valueStr);
		env->DeleteLocalRef(value);
	}
}

//void TestConversions(JNIEnv* env) {
//
//	// Empty test
//	{
//		std::map<std::string, std::string> map, mapTest;
//		jobject hashMap = StlStringStringMapToJavaHashMap(env, map);
//		JavaHashMapToStlStringStringMap(env, hashMap, mapTest);
//		assert(map == mapTest);
//	}
//
//	// One element test
//	{
//		std::map<std::string, std::string> map, mapTest;
//		map["one"] = "uno";
//		jobject hashMap = StlStringStringMapToJavaHashMap(env, map);
//		JavaHashMapToStlStringStringMap(env, hashMap, mapTest);
//		assert(map == mapTest);
//	}
//
//	// Two element test
//	{
//		std::map<std::string, std::string> map, mapTest;
//		map["one"] = "uno";
//		map["two"] = "duo";
//		jobject hashMap = StlStringStringMapToJavaHashMap(env, map);
//		JavaHashMapToStlStringStringMap(env, hashMap, mapTest);
//		assert(map == mapTest);
//	}
//
//	// Huge number of elements test
//	{
//		std::map<std::string, std::string> map, mapTest;
//		for (int n = 0; n < 10000; ++n) {
//			map[std::to_string(n)] = std::to_string(n);
//		}
//		jobject hashMap = StlStringStringMapToJavaHashMap(env, map);
//		JavaHashMapToStlStringStringMap(env, hashMap, mapTest);
//		assert(map == mapTest);
//	}
//}

std::string GetJavaClassName(JNIEnv* env, jobject obj) {
	jclass cls = env->GetObjectClass(obj);
	if (cls == NULL) {
		return NULL;
	}

	jmethodID mid = env->GetMethodID(cls, "getClass", "()Ljava/lang/Class;");
	if (mid == NULL) {
		env->DeleteLocalRef(cls);
		return NULL;
	}

	jobject classObject = env->CallObjectMethod(obj, mid);
	if (classObject == NULL) {
		env->DeleteLocalRef(cls);
		return NULL;
	}

	mid = env->GetMethodID(env->GetObjectClass(classObject), "getName", "()Ljava/lang/String;");
	if (mid == NULL) {
		env->DeleteLocalRef(cls);
		env->DeleteLocalRef(classObject);
		return NULL;
	}

	jstring classNameStr = (jstring)env->CallObjectMethod(classObject, mid);
	if (classNameStr == NULL) {
		env->DeleteLocalRef(cls);
		env->DeleteLocalRef(classObject);
		return NULL;
	}

	const char* rawClassName = env->GetStringUTFChars(classNameStr, NULL);

	std::string strClassName(rawClassName);
	env->ReleaseStringUTFChars(classNameStr, rawClassName);
	env->DeleteLocalRef(classNameStr);
	env->DeleteLocalRef(classObject);
	env->DeleteLocalRef(cls);

	return strClassName;
}

std::shared_ptr<BCBuffer> MakeBufferFromByteArray(
	JNIEnv* env, 
	jbyteArray jbytearray)
{
	if (jbytearray)
	{
		size_t len = env->GetArrayLength(jbytearray);
		std::shared_ptr<BCBuffer> payload(new BCBuffer);
		uint32_t nWriteSize = 0, nBufSize = len - nWriteSize, nDataSize;
		jbyte* buf;
		while (nWriteSize < len &&
			(buf = (jbyte*)payload->GetWritableBlock(nBufSize)) && nBufSize > 0)
		{
			nDataSize = BCMIN(nBufSize, len - nWriteSize);
			env->GetByteArrayRegion(jbytearray, nWriteSize, nDataSize, buf);
			payload->UngetWritableBlock(nDataSize);
			nWriteSize += nDataSize;
		}
		return payload;
	}
	return NULL;
}

std::shared_ptr<std::vector<uint8_t>> MakeArrayFromByteArray(
	JNIEnv* env, 
	jbyteArray jbytearray)
{
	if (jbytearray)
	{
		size_t len = env->GetArrayLength(jbytearray);
		auto payload = std::make_shared<std::vector<uint8_t>>(len);
		env->GetByteArrayRegion(jbytearray, 0, len, (jbyte*)&(*payload)[0]);
		return payload;
	}
	return NULL;
}

std::shared_ptr<BCBuffer> MakeBufferFromByteBuffer(
	JNIEnv* env, 
	jobject jbytebuffer)
{
	if (jbytebuffer)
	{
		void* buffer = (void*)env->GetDirectBufferAddress(jbytebuffer);
		size_t size = env->GetDirectBufferCapacity(jbytebuffer);
		std::shared_ptr<BCBuffer> payload(new BCBuffer);
		payload->Write(buffer, size);
		return payload;
	}
	return NULL;
}

std::shared_ptr<BCBuffer> MakeBufferFromString(
	JNIEnv* env, 
	jstring jstr)
{
	if (jstr)
	{
		std::string str = JStrToCStr(env, jstr);
		std::shared_ptr<BCBuffer> payload(new BCBuffer);
		payload->Write(str.c_str(), str.length());
		return payload;
	}
	return NULL;
}

SMPacketPtr MakePacketFromJava(JNIEnv* env, jobject obj)
{
	if (!env->IsInstanceOf(obj, env->FindClass("org/difft/android/smp/Packet")))
	{
		return NULL;
	}
	RetryInterval intervals;
	std::string strValue;
	jclass cls = env->GetObjectClass(obj);

	jbyte pkt_type = JniUtils::GetByteField(env, obj, cls, "type");
	jlong timestamp = JniUtils::GetLongField(env, obj, cls, "timestamp");
	jint trans_id = JniUtils::GetIntField(env, obj, cls, "transId");
	jint stream_id = JniUtils::GetIntField(env, obj, cls, "streamId");
	jbyteArray jpayload = (jbyteArray)JniUtils::GetObjectField(env, obj, cls, "payload", "[B");

	SMPacketPtr pkt(new SMPacket);
	pkt->type = pkt_type;
	pkt->timestamp = timestamp ? timestamp : bc_time_now();
	pkt->trans_id = trans_id;
	pkt->stream_id = stream_id;
	auto payload = MakeBufferFromByteArray(env, jpayload);
	pkt->Create(payload);
	return pkt;
}

///////////////////////////////////////////////////////////////////////////////
// Class : JVMScoped
///////////////////////////////////////////////////////////////////////////////

#ifdef OS_ANDROID
typedef JNIEnv JNIENV;
#else
typedef void JNIENV;
#endif

JVMScoped::JVMScoped(JNIEnv **ppEnv)
	: _attached(false)
	, _env(NULL)
{
	JavaVM *jvm = GetJVM();
	// get the JNI env for this thread
	JNIEnv *env = NULL;

	// get the JNI env for this thread
	if (jvm && jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		// try to attach the thread and get the env
		// Attach this thread to JVM
		jint res = jvm->AttachCurrentThread((JNIENV **)&env, NULL);
		if ((res < 0) || !env)
		{
			LogError(_LOCAL_, "%s: Could not attach thread to JVM (%d, %p)",
				__FUNCTION__, res, env);
			return;
		}
		_attached = true;
		_env = env;
	}
    else
    {
        _env = env;
    }
	if (ppEnv)
	{
		*ppEnv = env;
	}
}

JVMScoped::JVMScoped()
	: _attached(false)
	, _env(NULL)
{
}

bool JVMScoped::Attach(JNIEnv** ppEnv)
{
	if (_env)
	{
		if (ppEnv)
		{
			*ppEnv = _env;
		}
		return true;
	}
	JavaVM *jvm = GetJVM();
	// get the JNI env for this thread
	JNIEnv *env = NULL;

	// get the JNI env for this thread
	if (jvm && jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		// try to attach the thread and get the env
		// Attach this thread to JVM
		jint res = jvm->AttachCurrentThread((JNIENV **)&env, NULL);
		if ((res < 0) || !env)
		{
			LogError(_LOCAL_, "%s: Could not attach thread to JVM (%d, %p)",
				__FUNCTION__, res, env);
			return false;
		}
		_attached = true;
	}
	if (ppEnv)
	{
		*ppEnv = env;
	}
	_env = env;
	return true;
}

JVMScoped::~JVMScoped()
{
	// Detach this thread if it was attached
	if (_attached)
	{
		JavaVM *jvm = GetJVM();
		if (jvm && jvm->DetachCurrentThread() < 0)
		{
			LogError(_LOCAL_, "%s: Could not detach thread from JVM", __FUNCTION__);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : JniUtils
///////////////////////////////////////////////////////////////////////////////

JniUtils::ClassMap		JniUtils::globalClsMap;

JniUtils::JniUtils()
{
}

JniUtils::~JniUtils()
{
}

jchar JniUtils::GetCharField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "C");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetCharField(jobj, fid);
}

jbyte JniUtils::GetByteField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "B");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetByteField(jobj, fid);
}

jshort JniUtils::GetShortField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "S");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetShortField(jobj, fid);
}

jint JniUtils::GetIntField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "I");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetIntField(jobj, fid);
}

 jint JniUtils::GetIntArrayField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp,
	std::vector<int32_t> &vec)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "[I");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	jintArray jintarray = (jintArray)pEnv->GetObjectField(jobj, fid);
	if (!jintarray)
	{
		return -1;
	}
	jint length = pEnv->GetArrayLength(jintarray);
	vec.resize(length);
	jint *buffer = pEnv->GetIntArrayElements(jintarray, NULL);
	for (int i = 0; i < length; i++) {
		vec[i] = buffer[i];
	}
	pEnv->ReleaseIntArrayElements(jintarray, buffer, JNI_ABORT);

	if (pEnv->ExceptionCheck()) {
		pEnv->ExceptionDescribe();
		pEnv->ExceptionClear();
		return -1;
	}
	return 0;
}

jmethodID JniUtils::GetMethodID (
	JNIEnv* jni, 
	jclass c, 
	const char* name, 
	const char* signature) 
{
	jmethodID m = jni->GetMethodID(c, name, signature);
	if(!m)
	{
		return NULL;
	}
	return m;
}

jfieldID JniUtils::GetFieldID(
    JNIEnv* jni, 
	jclass c, 
	const char* name, 
	const char* signature) 
{
    jfieldID f = jni->GetFieldID(c, name, signature);
    if(!f)
    {
        return NULL;

    }
    return f;
}

jfloat JniUtils::GetFloatField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "F");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetFloatField(jobj, fid);
}

jdouble JniUtils::GetDoubleField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "D");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetDoubleField(jobj, fid);
}

jlong JniUtils::GetLongField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "J");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetLongField(jobj, fid);
}

jboolean JniUtils::GetBooleanField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "Z");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return -1;
	}
	return pEnv->GetBooleanField(jobj, fid);
}

const char* JniUtils::GetStringField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp,
	std::string &outStr)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "Ljava/lang/String;");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return NULL;
	}
	jstring jstr = (jstring)pEnv->GetObjectField(jobj, fid);
	if (!jstr)
	{
		LogError(_LOCAL_, "%s: could not get name string", __FUNCTION__);
		return NULL;
	}
	// Get command name char string
	const char* str = pEnv->GetStringUTFChars(jstr, 0);
	outStr = str;
	pEnv->ReleaseStringUTFChars(jstr, str);
	return outStr.c_str();
}

jobject JniUtils::GetObjectField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp,
	const char* lpszPropType)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, lpszPropType);
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get %s fid", __FUNCTION__, lpszProp);
		return NULL;
	}
	return pEnv->GetObjectField(jobj, fid);
}

int JniUtils::SetIntField(
	JNIEnv *pEnv,
	jobject jobj, 
	jclass cls, 
	const char* lpszProp,
	int nValue)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "I");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get callback fid", __FUNCTION__);
		return -1;
	}
	pEnv->SetIntField(jobj, fid, nValue);
	return 0;
}

int JniUtils::SetBooleanField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp,
	bool bValue)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "Z");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get callback fid", __FUNCTION__);
		return -1;
	}
	pEnv->SetBooleanField(jobj, fid, bValue);
	return 0;
}

int JniUtils::SetStringField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp,
	const char* lpszValue)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, "Ljava/lang/String;");
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get callback fid", __FUNCTION__);
		return -1;
	}
	jstring jstr = pEnv->NewStringUTF(lpszValue);
	pEnv->SetObjectField(jobj, fid, jstr);
	pEnv->DeleteLocalRef(jstr);
	return 0;
}

int JniUtils::SetObjectField(
	JNIEnv *pEnv,
	jobject jobj,
	jclass cls,
	const char* lpszProp,
	const char* lpszPropType,
	jobject jobjValue)
{
	jfieldID fid = pEnv->GetFieldID(cls, lpszProp, lpszPropType);
	if (!fid)
	{
		LogError(_LOCAL_, "%s: could not get callback fid", __FUNCTION__);
		return -1;
	}
	pEnv->SetObjectField(jobj, fid, jobjValue);
	return 0;
}

jclass JniUtils::FindClass(JNIEnv *pEnv, const char* lpszCls)
{
#ifdef __ANDROID__
	return (jclass)globalClsMap[lpszCls];
#else
	return pEnv->FindClass(lpszCls);
#endif
}

void JniUtils::UnCacheClass(JNIEnv *pEnv)
{
	for(auto &it : globalClsMap) {
		pEnv->DeleteGlobalRef(it.second);
	}
	globalClsMap.clear();
}

void JniUtils::CacheClass(JNIEnv *pEnv)
{
	_CacheClass(pEnv, "java/nio/ByteBuffer");
	_CacheClass(pEnv, "java/util/HashMap");
	_CacheClass(pEnv, "org/difft/android/smp/Connector");
    _CacheClass(pEnv, "org/difft/android/smp/Connection$IHandler");
    _CacheClass(pEnv, "org/difft/android/smp/Config");
    _CacheClass(pEnv, "org/difft/android/smp/Config$LogHandler");
}

void JniUtils::ThrowException(JNIEnv *pEnv, BCRESULT err)
{
	jclass exClass = pEnv->FindClass("java/lang/RuntimeException");
	if (exClass != NULL) {
		// 2. 抛出新的异常实例
		pEnv->ThrowNew(exClass, bc_result2string(err));
	}
}

void JniUtils::ThrowException(JNIEnv *pEnv, const char* lpszMsg)
{
	jclass exClass = pEnv->FindClass("java/lang/RuntimeException");
	if (exClass != NULL) {
		// 2. 抛出新的异常实例
		pEnv->ThrowNew(exClass, lpszMsg);
	}
}

void JniUtils::_CacheClass(JNIEnv *pEnv, const char* lpszCls)
{
	jclass cls = pEnv->FindClass(lpszCls);
	if (cls)
	{
		globalClsMap[lpszCls] = pEnv->NewGlobalRef(cls);
		pEnv->DeleteLocalRef(cls);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : JniByteBuffer
///////////////////////////////////////////////////////////////////////////////

JniByteBuffer::JniByteBuffer(JNIEnv* env)
	: env_(env)
	, byteArrayObj_(NULL)
	, buffer_(NULL)
	, size_(0)
{
	//
}

JniByteBuffer::JniByteBuffer(JNIEnv *env, const void* data, size_t size)
	: env_(env)
	, byteArrayObj_(NULL)
	, buffer_(NULL)
	, size_(0)
{
	SetSize(size);
	assert(buffer_);
	memcpy(buffer_, data, size);
}

JniByteBuffer::~JniByteBuffer()
{
	if (buffer_ && env_)
	{
		env_->DeleteGlobalRef(byteArrayObj_);
	}
}

size_t JniByteBuffer::SetSize(size_t size)
{
	if (env_ && byteArrayObj_ && size_ != size)
	{
		env_->DeleteGlobalRef(byteArrayObj_);
		byteArrayObj_ = NULL;
		size_ = 0;
	}
	if (env_ && !byteArrayObj_)
	{
		jobject javaObjLocal;
		jclass jcls = JniUtils::FindClass(env_, "java/nio/ByteBuffer");
		// get the method ID for the allocateDirect(int) constructor
		jmethodID mid = env_->GetStaticMethodID(jcls, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
		if (!mid)
		{
			goto delete_local_ref;
		}

		// construct the object
		javaObjLocal = env_->CallStaticObjectMethod(jcls, mid, size);
		if (!javaObjLocal)
		{
			goto delete_local_ref;
		}
		// make global reference of 
		byteArrayObj_ = (jbyteArray)env_->NewGlobalRef(javaObjLocal);
		env_->DeleteLocalRef(javaObjLocal);
		// get byte[]
		buffer_ = (uint8_t *)env_->GetDirectBufferAddress(byteArrayObj_);
		size_ = env_->GetDirectBufferCapacity(byteArrayObj_);
	delete_local_ref:
		env_->DeleteLocalRef(jcls);
	}
	return size_;
}

void JniByteBuffer::Rewind()
{
	if (env_ && byteArrayObj_)
	{
		jclass jcls = JniUtils::FindClass(env_, "java/nio/ByteBuffer");
		// get the method ID for the allocateDirect(int) constructor
		jmethodID mid = env_->GetMethodID(jcls, "rewind", "()Ljava/nio/ByteBuffer;");
		if (!mid)
		{
			return; /* exception thrown */
		}

		// construct the object
		env_->CallObjectMethod(byteArrayObj_, mid);
	}
}

size_t JniByteBuffer::Update(const void* data, size_t size)
{
	SetSize(size);
	Rewind();
	memcpy(buffer_, data, size);

	return size_;
}

///////////////////////////////////////////////////////////////////////////////
// class : JniByteArray
///////////////////////////////////////////////////////////////////////////////

JniByteArray::JniByteArray(JNIEnv* env)
	: env_(env)
	, byteArrayObj_(NULL)
{
	//
}

JniByteArray::JniByteArray(JNIEnv *env, const void* data, size_t size)
	: env_(env)
	, byteArrayObj_(NULL)
{
	Update(data, size);
}

JniByteArray::~JniByteArray()
{
	if (byteArrayObj_)
	{
		env_->DeleteGlobalRef(byteArrayObj_);
	}
}

size_t JniByteArray::Update(const void* data, size_t size)
{
	if (env_ && byteArrayObj_)
	{
		env_->DeleteGlobalRef(byteArrayObj_);
		byteArrayObj_ = NULL;
	}
	if (env_ && !byteArrayObj_)
	{
		// allocate new ByteArray
		jobject javaObjLocal = env_->NewByteArray(size);
		if (!javaObjLocal)
		{
			return 0;
		}
		// make global reference of 
		byteArrayObj_ = (jbyteArray)env_->NewGlobalRef(javaObjLocal);
		env_->DeleteLocalRef(javaObjLocal);
		// set byte[]
		env_->SetByteArrayRegion(byteArrayObj_, 0, size, (jbyte*)data);
	}
	return size;
}

///////////////////////////////////////////////////////////////////////////////
// class : JniStrStrHashMap
///////////////////////////////////////////////////////////////////////////////

JniStrStrHashMap::JniStrStrHashMap(JNIEnv *env)
	: env_(NULL)
	, hashMapObj_(NULL)
{
	//
}

JniStrStrHashMap::~JniStrStrHashMap()
{
	if (hashMapObj_ && env_)
	{
		env_->DeleteGlobalRef(hashMapObj_);
	}
}

BCRESULT JniStrStrHashMap::Put(const char* key, const char* value)
{
	JNIEnv* pEnv = env_;
	jclass mapClass = JniUtils::FindClass(pEnv, "java/util/HashMap");
	if (mapClass == NULL)
		return BC_R_UNEXPECTED;
	jmethodID put = pEnv->GetMethodID(mapClass, "put",
		"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
	if (!hashMapObj_)
	{
		jmethodID init = pEnv->GetMethodID(mapClass, "<init>", "()V");
		jobject hashMap = pEnv->NewObject(mapClass, init);

		hashMapObj_ = static_cast<jobject>(pEnv->NewGlobalRef(hashMap));
		pEnv->DeleteLocalRef(hashMap);
	}
	if (hashMapObj_ && key && value)
	{
		jstring keyJava = pEnv->NewStringUTF(key);
		jstring valueJava = pEnv->NewStringUTF(value);

		pEnv->CallObjectMethod(hashMapObj_, put, keyJava, valueJava);
		pEnv->DeleteLocalRef(keyJava);
		pEnv->DeleteLocalRef(valueJava);
	}

	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// End of file : jni_utils.cpp
///////////////////////////////////////////////////////////////////////////////
