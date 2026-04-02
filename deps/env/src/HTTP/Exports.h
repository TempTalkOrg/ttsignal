///////////////////////////////////////////////////////////////////////////////
// file : Exports.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_EXPORTS_H_INCLUDED__
#define HTTP_EXPORTS_H_INCLUDED__

#include <BC/Exports.h>

#if defined(_MSC_VER)
#define HTTPEXPORT __declspec(dllexport)
#define HTTPIMPORT __declspec(dllimport)
#else // compiler doesn't support __declspec()
#define HTTPEXPORT
#define HTTPIMPORT
#endif

// for other platforms/compilers we don't anything
#ifndef HTTPEXPORT
#define HTTPEXPORT
#define HTTPIMPORT
#endif

// HTTPDLLEXPORT maps to export declaration when building the DLL, to import
// declaration if using it or to nothing at all if we don't use wxWin DLL
#ifdef HTTPMAKINGDLL
#define HTTPDLLEXPORT HTTPEXPORT
#define HTTPDLLEXPORT_DATA(type) HTTPEXPORT type
#define HTTPDLLEXPORT_CTORFN
#define	HTTP_API		HTTPDLLEXPORT
#elif defined(HTTPUSINGDLL)
#define HTTPDLLEXPORT HTTPIMPORT
#define HTTPDLLEXPORT_DATA(type) HTTPIMPORT type
#define HTTPDLLEXPORT_CTORFN
#define	HTTP_API		HTTPIMPORT
#else // not making nor using DLL
#define HTTPDLLEXPORT
#define HTTPDLLEXPORT_DATA(type) type
#define HTTPDLLEXPORT_CTORFN
#define	HTTP_API
#endif



///////////////////////////////////////////////////////////////////////////////
// Option keys
///////////////////////////////////////////////////////////////////////////////


#define HTTP_OPTION_DEFINE(opt)	\
	extern HTTPDLLEXPORT_DATA(LPCSTR)	opt;

#define HTTP_OPTION_IMPLEMENT(opt)	\
	HTTPDLLEXPORT_DATA(LPCSTR)	opt = #opt;


#endif //HTTP_EXPORTS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : Exports.h
///////////////////////////////////////////////////////////////////////////////
