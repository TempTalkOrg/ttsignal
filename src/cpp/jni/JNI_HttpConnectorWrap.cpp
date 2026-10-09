#include "StdAfx.h"

#include "BC/BCFCodec.h"
#include "Runtime.h"
#include "TTErrors.h"
#include "JNI_HttpConnectorWrap.h"
#include "jni_utils.h"

namespace
{

jclass gHttpResponseClass = NULL;
jmethodID gHttpResponseConstructor = NULL;
jclass gHttpCallbackClass = NULL;
jmethodID gOnResponse = NULL;
jmethodID gOnError = NULL;

const char* ResultName(BCRESULT result)
{
    switch (result)
    {
    case BC_R_NO_PHYSICAL_INTERFACE: return "BC_R_NO_PHYSICAL_INTERFACE";
    case BC_R_PIN_FAILED: return "BC_R_PIN_FAILED";
    case BC_R_ROUTE_MISMATCH: return "BC_R_ROUTE_MISMATCH";
    case BC_R_DNS_FAILED: return "BC_R_DNS_FAILED";
    case BC_R_TLS_VERIFY_FAILED: return "BC_R_TLS_VERIFY_FAILED";
    case BC_R_RESPONSE_TOO_LARGE: return "BC_R_RESPONSE_TOO_LARGE";
    case BC_R_INVALIDARG: return "BC_R_INVALIDARG";
    case BC_R_NOTCONNECTED: return "BC_R_NOTCONNECTED";
    case BC_R_SHUTTINGDOWN: return "BC_R_SHUTTINGDOWN";
    default: return "BC_R_FAILURE";
    }
}

void PutStringField(
    JNIEnv* env,
    jobject object,
    jclass clazz,
    const char* field,
    const char* key,
    BCFObject& config)
{
    std::string value;
    JniUtils::GetStringField(env, object, clazz, field, value);
    if (!value.empty())
    {
        config.PutString(key, value.c_str());
    }
}

} // namespace

namespace JNI
{

HttpRequestWrap::HttpRequestWrap(
    JNIEnv* env,
    jobject callback,
    HttpConnectorWrap* owner)
    : owner_(owner)
    , callback_(env->NewGlobalRef(callback))
{
}

HttpRequestWrap::~HttpRequestWrap()
{
    if (callback_)
    {
        JNIEnv* env = GetThreadJNIEnv();
        if (env)
        {
            env->DeleteGlobalRef(callback_);
        }
        callback_ = NULL;
    }
}

void HttpRequestWrap::OnHttpResponse(const HttpResponse& response)
{
    owner_->RemoveTask(this);
    owner_ = NULL;

    JNIEnv* env = GetThreadJNIEnv();
    if (env && callback_)
    {
        jstring reason = env->NewStringUTF(response.reason.c_str());
        jobject headers =
            StlStringStringMapToJavaHashMap(env, response.headers);
        jbyteArray body = env->NewByteArray(response.body.size());
        if (body && !response.body.empty())
        {
            env->SetByteArrayRegion(
                body,
                0,
                response.body.size(),
                reinterpret_cast<const jbyte*>(response.body.data()));
        }
        jstring peerIp = env->NewStringUTF(response.peerIp.c_str());
        jstring pinMethod = env->NewStringUTF(response.pinMethod.c_str());
        jobject result = env->NewObject(
            gHttpResponseClass,
            gHttpResponseConstructor,
            response.status,
            reason,
            headers,
            body,
            peerIp,
            static_cast<jlong>(response.boundIfIndex),
            pinMethod);

        env->CallVoidMethod(callback_, gOnResponse, result);
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
        }

        env->DeleteLocalRef(result);
        env->DeleteLocalRef(pinMethod);
        env->DeleteLocalRef(peerIp);
        env->DeleteLocalRef(body);
        env->DeleteGlobalRef(headers);
        env->DeleteLocalRef(reason);
    }

    delete this;
}

void HttpRequestWrap::OnHttpError(
    BCRESULT result,
    const std::string& message)
{
    DeliverError(result, message);
}

void HttpRequestWrap::DeliverError(
    BCRESULT result,
    const std::string& message)
{
    owner_->RemoveTask(this);
    owner_ = NULL;

    JNIEnv* env = GetThreadJNIEnv();
    if (env && callback_)
    {
        jstring errorName = env->NewStringUTF(ResultName(result));
        jstring errorMessage = env->NewStringUTF(message.c_str());
        env->CallVoidMethod(
            callback_,
            gOnError,
            static_cast<jint>(result),
            errorName,
            errorMessage);
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(errorMessage);
        env->DeleteLocalRef(errorName);
    }

    delete this;
}

HttpConnectorWrap::HttpConnectorWrap()
    : connector_(NULL)
{
}

HttpConnectorWrap::~HttpConnectorWrap()
{
    delete connector_;
    connector_ = NULL;

    std::lock_guard<std::mutex> guard(tasks_lock_);
    for (HttpRequestWrap* task : tasks_)
    {
        delete task;
    }
    tasks_.clear();
}

BCRESULT HttpConnectorWrap::Initialize(JNIEnv* env)
{
    jclass responseClass =
        env->FindClass("org/difft/android/smp/HttpResponse");
    gHttpResponseClass =
        static_cast<jclass>(env->NewGlobalRef(responseClass));
    gHttpResponseConstructor = env->GetMethodID(
        gHttpResponseClass,
        "<init>",
        "(ILjava/lang/String;Ljava/util/Map;[BLjava/lang/String;JLjava/lang/String;)V");
    env->DeleteLocalRef(responseClass);

    jclass callbackClass =
        env->FindClass("org/difft/android/smp/HttpCallback");
    gHttpCallbackClass =
        static_cast<jclass>(env->NewGlobalRef(callbackClass));
    gOnResponse = env->GetMethodID(
        gHttpCallbackClass,
        "onResponse",
        "(Lorg/difft/android/smp/HttpResponse;)V");
    gOnError = env->GetMethodID(
        gHttpCallbackClass,
        "onError",
        "(ILjava/lang/String;Ljava/lang/String;)V");
    env->DeleteLocalRef(callbackClass);

    jclass clazz = env->FindClass("org/difft/android/smp/HttpConnector");
    static JNINativeMethod methods[] = {
        {
            (char*)"initialize",
            (char*)"(Lorg/difft/android/smp/HttpConfig;)J",
            reinterpret_cast<void*>(_Initialize)
        },
        {
            (char*)"request",
            (char*)"(JLorg/difft/android/smp/HttpRequest;Lorg/difft/android/smp/HttpCallback;)V",
            reinterpret_cast<void*>(_Request)
        },
        {
            (char*)"destroy",
            (char*)"(J)V",
            reinterpret_cast<void*>(_Destroy)
        },
    };
    if (env->RegisterNatives(
            clazz,
            methods,
            sizeof(methods) / sizeof(methods[0])) < 0)
    {
        env->DeleteLocalRef(clazz);
        return BC_R_UNEXPECTED;
    }
    env->DeleteLocalRef(clazz);
    return BC_R_SUCCESS;
}

jlong HttpConnectorWrap::_Initialize(
    JNIEnv* env,
    jobject,
    jobject config)
{
    HttpConnectorWrap* wrapper = new HttpConnectorWrap();
    if (wrapper->Create(env, config) != BC_R_SUCCESS)
    {
        delete wrapper;
        return 0;
    }
    return reinterpret_cast<jlong>(wrapper);
}

BCRESULT HttpConnectorWrap::Create(JNIEnv* env, jobject configObject)
{
    jclass clazz = env->GetObjectClass(configObject);
    BCFObject config;
    PutStringField(
        env, configObject, clazz, "vpnPolicy", "vpnPolicy", config);
    PutStringField(env, configObject, clazz, "caCerts", "caCerts", config);
    PutStringField(env, configObject, clazz, "spkiPin", "spkiPin", config);
    PutStringField(
        env, configObject, clazz, "dnsServers", "dnsServers", config);

    config.PutBool(
        "insecureSkipVerify",
        JniUtils::GetBooleanField(
            env, configObject, clazz, "insecureSkipVerify"));
    config.PutInt(
        "dnsTimeoutMs",
        JniUtils::GetIntField(env, configObject, clazz, "dnsTimeoutMs"));
    config.PutInt(
        "drainTimeoutMs",
        JniUtils::GetIntField(env, configObject, clazz, "drainTimeoutMs"));
    config.PutInt(
        "logLevel",
        JniUtils::GetIntField(env, configObject, clazz, "logLevel"));
    config.PutInt(
        "maxResponseBytes",
        JniUtils::GetIntField(
            env, configObject, clazz, "maxResponseBytes"));
    config.PutInt(
        "androidNetHandle",
        JniUtils::GetLongField(
            env, configObject, clazz, "androidNetHandle"));
    env->DeleteLocalRef(clazz);

    // HTTP can run before the QUIC connector. Initialize the shared runtime
    // with the same defaults as the Android Config class so this path does not
    // change the later QUIC connector's thread configuration.
    BCFObject runtimeConfig;
    runtimeConfig.PutInt("workerThreads", 1);
    runtimeConfig.PutInt("taskThreads", 16);
    runtimeConfig.PutInt("timerThreads", 4);
    BCRESULT result = Runtime::Initialize(&runtimeConfig);
    if (result != BC_R_SUCCESS)
    {
        return result;
    }

    connector_ = new HttpConnector();
    result = connector_->Create(&config, this);
    if (result != BC_R_SUCCESS)
    {
        delete connector_;
        connector_ = NULL;
    }
    return result;
}

void HttpConnectorWrap::_Request(
    JNIEnv* env,
    jobject,
    jlong handle,
    jobject requestObject,
    jobject callback)
{
    HttpConnectorWrap* wrapper =
        reinterpret_cast<HttpConnectorWrap*>(handle);
    if (!wrapper || !wrapper->connector_)
    {
        return;
    }

    jclass clazz = env->GetObjectClass(requestObject);
    HttpRequest request;
    std::string value;
    request.method = JniUtils::GetStringField(
        env, requestObject, clazz, "method", value);
    request.url = JniUtils::GetStringField(
        env, requestObject, clazz, "url", value);
    request.timeoutMs = JniUtils::GetIntField(
        env, requestObject, clazz, "timeoutMs");
    request.resolvedIp = JniUtils::GetStringField(
        env, requestObject, clazz, "resolvedIp", value);

    jobject headers = JniUtils::GetObjectField(
        env,
        requestObject,
        clazz,
        "headers",
        "Ljava/util/Map;");
    if (headers)
    {
        JavaHashMapToStlStringStringMap(env, headers, request.headers);
        env->DeleteLocalRef(headers);
    }
    jbyteArray body = static_cast<jbyteArray>(JniUtils::GetObjectField(
        env,
        requestObject,
        clazz,
        "body",
        "[B"));
    if (body)
    {
        std::shared_ptr<std::vector<uint8_t>> bytes =
            MakeArrayFromByteArray(env, body);
        if (bytes)
        {
            request.body.assign(
                reinterpret_cast<const char*>(bytes->data()),
                bytes->size());
        }
        env->DeleteLocalRef(body);
    }
    env->DeleteLocalRef(clazz);

    HttpRequestWrap* task =
        new HttpRequestWrap(env, callback, wrapper);
    {
        std::lock_guard<std::mutex> guard(wrapper->tasks_lock_);
        wrapper->tasks_.insert(task);
    }
    std::string error;
    BCRESULT result = wrapper->connector_->Request(request, task, &error);
    if (result != BC_R_SUCCESS)
    {
        task->DeliverError(result, error);
    }
}

void HttpConnectorWrap::_Destroy(JNIEnv*, jobject, jlong handle)
{
    delete reinterpret_cast<HttpConnectorWrap*>(handle);
}

void HttpConnectorWrap::RemoveTask(HttpRequestWrap* task)
{
    std::lock_guard<std::mutex> guard(tasks_lock_);
    tasks_.erase(task);
}

void HttpConnectorWrap::OnLog(int, LPCSTR)
{
}

void HttpConnectorWrap::OnClosed()
{
}

} // namespace JNI
