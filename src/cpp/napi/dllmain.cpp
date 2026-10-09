///////////////////////////////////////////////////////////////////////////////
// file : dllmain.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "../StdAfx.h"
#include "../Runtime.h"
#include "JsExchanger.h"
#include "macros.h"
#include "Utils.h"
#include "JsSMPConnectorWrap.h"
#include "JsSMPServerWrap.h"
#include "JsHttpConnectorWrap.h"
#include "JsWSConnectorWrap.h"
#ifdef __LINUX__
#include <sys/resource.h>
#endif // __LINUX__


///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node {



///////////////////////////////////////////////////////////////////////////////
// Functions
///////////////////////////////////////////////////////////////////////////////

Napi::Value CreateConnector(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_WITH_RESULT(env, "Invalid config arguments", env.Undefined());
	}
	Napi::Object hConnector = JsSMPConnectorWrap::constructor_template.New({info[0]});
	return scope.Escape(hConnector);
}

Napi::Value CreateServer(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_WITH_RESULT(env, "Invalid config arguments", env.Undefined());
	}
	Napi::Object hServer = JsSMPServerWrap::constructor_template.New({info[0]});
	return scope.Escape(hServer);
}

// HTTP 客户端。和 QUIC/SMP 那两个连接器完全独立：自己的 TCP/TLS 栈，自己的
// vpnPolicy 解析，不共享 engine。返回的原始对象由 src/js/index.js 包成 Promise
// 形态，接口细节见 JsHttpConnectorWrap.h。
Napi::Value CreateHttpConnector(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_WITH_RESULT(env, "Invalid config arguments", env.Undefined());
	}
	Napi::Object hConnector =
		JsHttpConnectorWrap::constructor_template.New({info[0]});
	return scope.Escape(hConnector);
}

// WebSocket 客户端。与 HTTP 客户端同一条 TCP/TLS 栈（TcpChannel + DnsResolver +
// TlsContext），自己的 vpnPolicy 解析，与 QUIC/SMP 那两个连接器完全独立、不共享
// engine。返回的原始对象由 src/js/index.js 包成 EventEmitter + Promise 形态，
// 接口细节见 JsWSConnectorWrap.h。
Napi::Value CreateWsConnector(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();
	Napi::EscapableHandleScope scope(env);

	if (info.Length() == 0 || !info[0].IsObject()) {
		THROW_ERROR_WITH_RESULT(env, "Invalid config arguments", env.Undefined());
	}
	Napi::Object hConnector =
		JsWSConnectorWrap::constructor_template.New({info[0]});
	return scope.Escape(hConnector);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node


Napi::Object RegisterModule(Napi::Env env, Napi::Object exports)
{
#ifdef __LINUX__
	struct rlimit limit;
	memset(&limit, 0, sizeof(limit));

	limit.rlim_cur = RLIM_INFINITY;
	limit.rlim_max = RLIM_INFINITY;

	if (setrlimit(RLIMIT_CORE, &limit) < 0)
	{
		LogError(_LOCAL_, "Failed to call setrlimit(RLIMIT_CORE)");
	}
	memset(&limit, 0, sizeof(limit));

	limit.rlim_cur = 100000;
	limit.rlim_max = 100000;

	if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
	{
		LogError(_LOCAL_, "Failed to call setrlimit(RLIMIT_NOFILE)");
	}
#endif // __LINUX__
	//BCFObject config;

	//config.PutInt("workerThreads", 1);
	//config.PutInt("taskThreads", 2);
	//config.PutInt("timerThreads", 1);
	//Runtime::Initialize(&config);
	node::InitSymbols(env);
	JsExchanger::CreateInstance();
	JsSMPConnectorWrap::Initialize(env, exports);
	JsSMPConnectionWrap::Initialize(env, exports);
	JsSMPServerWrap::Initialize(env, exports);
	JsSMPServerConnectionWrap::Initialize(env, exports);
	JsHttpConnectorWrap::Initialize(env, exports);
	JsWSConnectorWrap::Initialize(env, exports);
	JsWSConnectionWrap::Initialize(env, exports);

	exports.Set("__createConnector__", Napi::Function::New(env, CreateConnector));
	exports.Set("__createServer__", Napi::Function::New(env, CreateServer));
	exports.Set("__createHttpConnector__",
				Napi::Function::New(env, CreateHttpConnector));
	exports.Set("__createWsConnector__",
				Napi::Function::New(env, CreateWsConnector));

	return exports;
}

#ifdef _MSC_VER
BOOL APIENTRY DllMain( 
	HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		::DisableThreadLibraryCalls(hModule);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
#endif // _MSC_VER

NODE_API_MODULE(ttsignal, RegisterModule);

///////////////////////////////////////////////////////////////////////////////
// End of file : dllmain.cpp
///////////////////////////////////////////////////////////////////////////////
