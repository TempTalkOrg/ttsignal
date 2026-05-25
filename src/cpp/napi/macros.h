///////////////////////////////////////////////////////////////////////////////
// file : macros.h
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#ifndef MACROS_H_INCLUDED__
#define MACROS_H_INCLUDED__

#include <exception>
#include <initializer_list>
#include <napi.h>

typedef std::initializer_list<napi_value> ArgsList;

#define EMIT_EVENT(env, obj, argv)                                          \
    TRY_CATCH_CALL((env), (obj),                                            \
        obj.Get(Napi::String::New(env, "emit")).As<Function>(),             \
        argv                                                                \
    );

#define TRY_CATCH_CALL(env, context, callback, argv)                        \
        try {                                                               \
            callback.Call(context, argv);                                   \
        } catch (const std::exception& e) {                                 \
            Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();   \
        }


#define EMIT_ERROR(env, context, errmsg)                                    \
{   ArgsList args = {                                                       \
        Napi::String::New(env, "errno"),                                    \
        Napi::Error::New(env, errmsg).Value()                               \
    };                                                                      \
    EMIT_EVENT(env, context, args);                                         }

#define ERROR_CALL(env, context, callback, errmsg)                          \
{   ArgsList args = {                                                       \
        Napi::Error::New(env, errmsg).Value()                               \
    };                                                                      \
    TRY_CATCH_CALL(env, context, callback, args);                           }

#define THROW_ERROR_WITH_RESULT(env, errmsg, retval)                        \
{   Napi::Error::New(env, errmsg).ThrowAsJavaScriptException();             \
    return retval;                                                          }

#define THROW_ERROR_VOID(env, errmsg)                                       \
{   Napi::Error::New(env, errmsg).ThrowAsJavaScriptException();return;      }


#endif // MACROS_H_INCLUDED__


///////////////////////////////////////////////////////////////////////////////
// End of file : macros.h
///////////////////////////////////////////////////////////////////////////////
