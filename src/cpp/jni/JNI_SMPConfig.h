


#ifndef SMPCONFIG_H_INCLUDED__
#define SMPCONFIG_H_INCLUDED__

#include <jni.h>
#include <BC/BCFCodec.h>





namespace JNI
{

class LogHandler;





class SMPConfig
{
public:
	SMPConfig();
    virtual ~SMPConfig();

	static BCFObject	*	ConvertFromJava(JNIEnv *env, jobject obj);
    static LogHandler   *	GetLogHandler(JNIEnv *env, jobject obj);
private:
    DECLARE_NO_COPY_CLASS(SMPConfig);
};





class LogHandler
{
public:
	LogHandler(JNIEnv *env, jobject obj);
    virtual ~LogHandler();

	void	Log(int level, const char *message);
private:
    JNIEnv      *   m_env;
    jobject         m_logHandler;
    jclass          m_logHandlerClass;
    jmethodID       m_logMethod;
};






} 


#endif 
