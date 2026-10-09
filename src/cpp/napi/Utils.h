///////////////////////////////////////////////////////////////////////////////
// file : napi/Utils.h
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#ifndef NAPI_UTILS_H_INCLUDED__
#define NAPI_UTILS_H_INCLUDED__

#include <string>
#include <napi.h>
#include <BC/BCFCodec.h>
// ResultName 已提升到 src/cpp/Utils.h，供各绑定层共用 —— iOS 目标的源码 glob
// 只收 cpp/*.cpp，够不到 napi/ 这边，而 ios_bridge 要用它填 errName。
// 同在 namespace node 下，NAPI 这边的调用处无需改动。
#include "../Utils.h"

namespace BC
{
	class BCBuffer;
}

using namespace BC;



///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{


///////////////////////////////////////////////////////////////////////////////
// global constant variables
///////////////////////////////////////////////////////////////////////////////

extern std::string 		internalCallback_sym;
extern std::string		close_sym;
extern std::string		error_sym;
extern std::string		command_sym;
extern std::string		data_sym;
extern std::string		filename_sym;
extern std::string 		mode_sym;
extern std::string 		handshake_finished_sym;
extern std::string 		accept_sym;
extern std::string 		connect_sym;
extern std::string 		connection_sym;
extern std::string 		stream_created_sym;
extern std::string 		stream_closed_sym;
extern std::string 		restart_sym;
extern std::string 		stream_data_acked_sym;
extern std::string 		stream_data_sent_sym;


void						
InitSymbols(Napi::Env env);

///////////////////////////////////////////////////////////////////////////////
// DrainPendingException —— **每次调完 JS 都必须走一遍**
//
// ⚠️ 漏掉会把整个 node 进程打死（100% 可复现）。机制与后果见 Utils.cpp 里的实现
// 注释，那段别删。所有 TRY_CATCH_CALL 之后紧跟一行 DrainPendingException(env)。
///////////////////////////////////////////////////////////////////////////////

void
DrainPendingException(Napi::Env env);

BCFVar *	
ConvertBCFFromJS(Napi::Env env, const Napi::Value &refValue);

Napi::Value		
ConvertJSFromBCF(Napi::Env env, BCFVar *pVar);

Napi::Value 
GetPrototypeProperty(Napi::Env env, napi_value obj, const std::string &propertyName);

Napi::Value 
GetPrototype(Napi::Env env, napi_value obj);

bool
IsElectron();

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

#endif // NAPI_UTILS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : napi/Utils.h
///////////////////////////////////////////////////////////////////////////////
