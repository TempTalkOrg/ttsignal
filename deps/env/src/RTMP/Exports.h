///////////////////////////////////////////////////////////////////////////////
// file : Exports.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef RTMP_EXPORTS_H_INCLUDED__
#define RTMP_EXPORTS_H_INCLUDED__

#include <BC/Exports.h>

#if defined(_MSC_VER)
#define RTMPEXPORT __declspec(dllexport)
#define RTMPIMPORT __declspec(dllimport)
#else // compiler doesn't support __declspec()
#define RTMPEXPORT
#define RTMPIMPORT
#endif

// for other platforms/compilers we don't anything
#ifndef RTMPEXPORT
#define RTMPEXPORT
#define RTMPIMPORT
#endif

// RTMPDLLEXPORT maps to export declaration when building the DLL, to import
// declaration if using it or to nothing at all if we don't use wxWin DLL
#ifdef RTMPMAKINGDLL
#define RTMPDLLEXPORT RTMPEXPORT
#define RTMPDLLEXPORT_DATA(type) RTMPEXPORT type
#define RTMPDLLEXPORT_CTORFN
#define	RTMP_API		RTMPDLLEXPORT
#elif defined(RTMPUSINGDLL)
#define RTMPDLLEXPORT RTMPIMPORT
#define RTMPDLLEXPORT_DATA(type) RTMPIMPORT type
#define RTMPDLLEXPORT_CTORFN
#define	RTMP_API		RTMPIMPORT
#else // not making nor using DLL
#define RTMPDLLEXPORT
#define RTMPDLLEXPORT_DATA(type) type
#define RTMPDLLEXPORT_CTORFN
#define	RTMP_API
#endif



///////////////////////////////////////////////////////////////////////////////
// Option keys
///////////////////////////////////////////////////////////////////////////////


#define RTMP_OPTION_DEFINE(opt)	\
	extern RTMPDLLEXPORT_DATA(LPCSTR)	opt;

#define RTMP_OPTION_IMPLEMENT(opt)	\
	RTMPDLLEXPORT_DATA(LPCSTR)	opt = #opt;


#endif //RTMP_EXPORTS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : Exports.h
///////////////////////////////////////////////////////////////////////////////
