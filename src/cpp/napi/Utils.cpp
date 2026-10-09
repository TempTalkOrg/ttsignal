///////////////////////////////////////////////////////////////////////////////
// file : napi/Utils.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include <inttypes.h>
#include "../TTErrors.h"
#include "Utils.h"
#include <BC/Utils.h>
#include <BC/BCFCodec.h>
#include <BC/BCJson.h>
#include <BC/BCPString.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

///////////////////////////////////////////////////////////////////////////////
// global constant variables
///////////////////////////////////////////////////////////////////////////////

std::string internalCallback_sym("_internalCallback");
std::string error_sym("error");
std::string close_sym("close");
std::string command_sym("command");
std::string data_sym("data");
std::string filename_sym("filename");
std::string mode_sym("mode");
std::string handshake_finished_sym("handshakeFinished");
std::string accept_sym("accept");
std::string connect_sym("connect");
std::string connection_sym("connection");
std::string stream_created_sym("streamCreated");
std::string stream_closed_sym("streamClosed");
std::string restart_sym("restart");
std::string stream_data_acked_sym("streamDataAcked");
std::string stream_data_sent_sym("streamDataSent");

std::atomic<bool> G_isElectron(false);

bool IsElectron()
{
	return G_isElectron.load();
}

void InitSymbols(Napi::Env env)
{
	Napi::HandleScope scope(env);

	// check if we are running in electron
    Napi::Object process = env.Global().Get("process").As<Napi::Object>();
    if (process.IsObject()) {
        Napi::Object versions = process.Get("versions").As<Napi::Object>();
        if (versions.IsObject()) {
            G_isElectron.store(!versions.Get("electron").IsUndefined());
        }
    }
}


///////////////////////////////////////////////////////////////////////////////
// DrainPendingException —— 每次调完 JS 都必须走一遍
//
// ⚠️ 不清 pending exception 会把**整个 node 进程打死**，而且机制不在调用方那个
// 文件里：
//
//   macros.h:21-26 的 TRY_CATCH_CALL 抓到 C++ 异常之后调
//   ThrowAsJavaScriptException()，把一个 pending exception 留在 env 里
//   **没人清**；而 JsExchanger::OnEventProcess 与
//   UVEventFactory::_EventCallback（UVExchanger.cpp:192-213）都没有 catch。
//   于是同一批 uv_async 里的**下一个**事件一旦调到 Napi::String::New /
//   Object::New / Buffer::Copy 这类"在 TRY_CATCH_CALL 之外"的 napi 调用
//   （各绑定构造回调参数时全是），node-addon-api 就因为 env 里挂着异常而直接抛
//   C++ 异常，逃出事件泵 -> libc++abi: terminating。
//
// 这条对两套绑定都是"必现"级别：HTTP 侧实测 6 条并发请求、第一条回调 throw，
// 修复前 100% SIGABRT（exit 134）；WS 侧一次 read 里的多个帧就是同一批多个事件，
// 更容易撞上（实测服务端一次 write 送 6 个文本帧，第一个回调 throw 即中）。
// 只发一条时更阴 —— 异常会从**后面某次毫不相干的原生调用里**抛出来，此后所有
// 回调静默丢失，而 uv_ref 又让进程不退出，变成永久静默卡住。
//
// napi_fatal_exception 把它按 Node 的 uncaughtException 上报（业务的
// process.on('uncaughtException') 收得到，默认行为也和普通 JS 异常一致），同时
// 把 env 清干净，后续事件不受影响。
///////////////////////////////////////////////////////////////////////////////

void DrainPendingException(Napi::Env env)
{
	if (env.IsExceptionPending())
	{
		Napi::Error hError = env.GetAndClearPendingException();
		napi_fatal_exception(env, hError.Value());
	}
}

Napi::Value ConvertJSFromBCF(Napi::Env env, BCFVar *pVar)
{
	Napi::EscapableHandleScope scope(env);

	if (!pVar)
	{
		return env.Null();
	}
	uint32_t eAmfType = pVar->GetType();
	if (eAmfType == BCF_TYPE_ARRAY)
	{
		BCFArray * pArray = static_cast<BCFArray *>(pVar);
		Napi::Array outArray = Napi::Array::New(env, pArray->Size());
		for (uint32_t i = 0; i < outArray.Length(); i++)
		{
			outArray.Set(i, ConvertJSFromBCF(env, pArray->Get(i)));
		}
		return scope.Escape(outArray);
	}
	else if (eAmfType == BCF_TYPE_OBJECT)
	{
		BCFObject * pObject = static_cast<BCFObject *>(pVar);
		Napi::Object outObject = Napi::Object::New(env);
		BCFTableEntry *pIter, *pEnd = pObject->EndEntry();
		for (pIter = pObject->BeginEntry();
			pIter != pEnd;
			pIter = pObject->NextEntry(pIter))
		{
			outObject.Set(
				Napi::String::New(env, pIter->GetKey().c_str()),
				ConvertJSFromBCF(env, pIter->GetValue()));
		}
		return scope.Escape(outObject);
	}
	else if (eAmfType == BCF_TYPE_DOUBLE)
	{
		BCFDouble * pNumber = static_cast<BCFDouble *>(pVar);
		Napi::Number outNumber = Napi::Number::New(env, pNumber->GetValue());
		return scope.Escape(outNumber);
	}
	else if (eAmfType == BCF_TYPE_INT)
	{
		BCFInt * pInt = static_cast<BCFInt *>(pVar);
		Napi::Number outInt = Napi::Number::New(env, (int32_t)pInt->GetValue());
		return scope.Escape(outInt);
	}
	else if (eAmfType == BCF_TYPE_TRUE)
	{
		return scope.Escape(Napi::Boolean::New(env, true));
	}
	else if (eAmfType == BCF_TYPE_FALSE)
	{
		return scope.Escape(Napi::Boolean::New(env, false));
	}
	else if (eAmfType == BCF_TYPE_STRING)
	{
		BCFString * pString = static_cast<BCFString *>(pVar);
		Napi::String outString = Napi::String::New(env, pString->GetValue().c_str());
		return scope.Escape(outString);
	}
	else if (eAmfType == BCF_TYPE_NULL)
	{
		return env.Null();
	}
	else if (eAmfType == BCF_TYPE_BYTEARRAY)
	{
		BCFByteArray * pArray = static_cast<BCFByteArray *>(pVar);
		Napi::Buffer<char> outBuffer = Napi::Buffer<char>::New(env, pArray->GetLength());
		char *pData = outBuffer.Data();
		pArray->GetBuffer()->Peek(pData, pArray->GetLength());
		return scope.Escape(outBuffer);
	}
	else
	{
		return env.Undefined();
	}
}

BCFVar * ConvertBCFFromJS(Napi::Env env, const Napi::Value &refValue)
{
	Napi::HandleScope scope(env);

	if (refValue.IsArray())
	{
		BCFArray * pVar = static_cast<BCFArray *>(
			BCFCodec::NewVariant(BCF_TYPE_ARRAY));
		Napi::Array inArray = refValue.As<Napi::Array>();
		for (uint32_t i = 0; i < inArray.Length(); i++)
		{
			Napi::Value item = inArray.Get(i);
			pVar->Push(ConvertBCFFromJS(env, item));
		}
		return pVar;
	}
	else if (refValue.IsObject())
	{
		BCFObject * pVar = static_cast<BCFObject *>(
			BCFCodec::NewVariant(BCF_TYPE_OBJECT));
		Napi::Object inObject = refValue.As<Napi::Object>();
		Napi::Array propertyNames = inObject.GetPropertyNames();
		for (uint32_t i = 0; i < propertyNames.Length(); i++)
		{
			Napi::Value propertyName = propertyNames.Get(i);
			Napi::String attrName = propertyName.As<Napi::String>();
			pVar->Put(attrName.Utf8Value().c_str(), ConvertBCFFromJS(env, inObject.Get(propertyName)));
		}
		return pVar;
	}
	else if (refValue.IsBigInt())
	{
		Napi::BigInt bigInt = refValue.As<Napi::BigInt>();
		BCFInt * pVal = static_cast<BCFInt *>(
			BCFCodec::NewVariant(BCF_TYPE_INT));
		ASSERT(pVal);
		bool lossless = false;
		pVal->SetValue(bigInt.Uint64Value(&lossless));
		return pVal;
	}
	else if (refValue.IsNumber())
	{
		Napi::Number inNumber = refValue.As<Napi::Number>();
		BCFDouble * pVal = static_cast<BCFDouble *>(
			BCFCodec::NewVariant(BCF_TYPE_DOUBLE));
		ASSERT(pVal);
		pVal->SetValue(inNumber.Int64Value());
		return pVal;
	}
	else if (refValue.IsBoolean())
	{
		if (refValue.As<Napi::Boolean>())
		{
			return BCFCodec::NewVariant(BCF_TYPE_TRUE);
		}
		else
		{
			return BCFCodec::NewVariant(BCF_TYPE_FALSE);
		}
	}
	else if (refValue.IsString())
	{
		Napi::String str = refValue.As<Napi::String>();
		BCFString * pVal = static_cast<BCFString *>(
			BCFCodec::NewVariant(BCF_TYPE_STRING));
		ASSERT(pVal);
		pVal->SetValue(str);
		return pVal;
	}
	else if (refValue.IsNull())
	{
		BCFNull * pVal = static_cast<BCFNull *>(
			BCFCodec::NewVariant(BCF_TYPE_NULL));
		ASSERT(pVal);
		return pVal;
	}
	else if (refValue.IsUndefined())
	{
		BCFUndefined * pVal = static_cast<BCFUndefined *>(
			BCFCodec::NewVariant(BCF_TYPE_UNDEFINED));
		ASSERT(pVal);
		return pVal;
	}
	else if (refValue.IsBuffer())
	{
		Napi::Buffer<char> inBuffer = refValue.As<Napi::Buffer<char>>();
		BCFByteArray * pVal = static_cast<BCFByteArray *>(
			BCFCodec::NewVariant(BCF_TYPE_BYTEARRAY));
		ASSERT(pVal);
		pVal->GetBuffer()->Write(inBuffer.Data(), inBuffer.Length());
		return pVal;
	}
	return NULL;
}


Napi::Value 
GetPrototypeProperty(Napi::Env env, napi_value obj, const std::string &propertyName) {
	Napi::EscapableHandleScope scope(env);
    napi_status status;

    napi_value prototype;
    status = napi_get_prototype(env, obj, &prototype);
    
    if (status != napi_ok) {
        napi_throw_error(env, NULL, "Failed to get prototype");
        return env.Null();
    }
    napi_value prototype_prop_value;

	napi_value prototype_prop_key;
	status = napi_create_string_utf8(env, propertyName.c_str(), propertyName.size(), &prototype_prop_key);
	if (status != napi_ok) {
        napi_throw_error(env, NULL, "Failed to create property key");
        return env.Null();
    }
    
    status = napi_get_property(env, prototype, prototype_prop_key, &prototype_prop_value);
    
    if (status != napi_ok) {
        napi_get_undefined(env, &prototype_prop_value);
    }
    
    return scope.Escape(prototype_prop_value);
}

Napi::Value 
GetPrototype(Napi::Env env, napi_value obj) {
	Napi::EscapableHandleScope scope(env);
    napi_status status;

    napi_value prototype;
    status = napi_get_prototype(env, obj, &prototype);
    
    if (status != napi_ok) {
        napi_throw_error(env, NULL, "Failed to get prototype");
        return env.Null();
    }
    return scope.Escape(prototype);
}

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file : napi/Utils.cpp
///////////////////////////////////////////////////////////////////////////////
