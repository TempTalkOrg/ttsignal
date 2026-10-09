#ifndef JNI_HTTPCONNECTORWRAP_H_INCLUDED__
#define JNI_HTTPCONNECTORWRAP_H_INCLUDED__

#include <jni.h>
#include <mutex>
#include <set>

#include "HttpConnector.h"

namespace JNI
{

class HttpConnectorWrap;

class HttpRequestWrap : public IHttpRequestHandler
{
public:
    HttpRequestWrap(JNIEnv* env, jobject callback, HttpConnectorWrap* owner);
    ~HttpRequestWrap();

    void OnHttpResponse(const HttpResponse& response) override;
    void OnHttpError(BCRESULT result, const std::string& message) override;
    void DeliverError(BCRESULT result, const std::string& message);

private:
    HttpConnectorWrap* owner_;
    jobject callback_;
};

class HttpConnectorWrap : public IHttpConnectorHandler
{
public:
    HttpConnectorWrap();
    ~HttpConnectorWrap();

    static BCRESULT Initialize(JNIEnv* env);

    void RemoveTask(HttpRequestWrap* task);

    void OnLog(int level, LPCSTR message) override;
    void OnClosed() override;

private:
    static jlong _Initialize(JNIEnv* env, jobject, jobject config);
    static void _Request(
        JNIEnv* env,
        jobject,
        jlong handle,
        jobject request,
        jobject callback);
    static void _Destroy(JNIEnv* env, jobject, jlong handle);

    BCRESULT Create(JNIEnv* env, jobject config);

    HttpConnector* connector_;
    std::mutex tasks_lock_;
    std::set<HttpRequestWrap*> tasks_;
};

} // namespace JNI

#endif // JNI_HTTPCONNECTORWRAP_H_INCLUDED__
