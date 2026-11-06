
#if defined (_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#ifndef BC_EXPORTS_INCLUDED__
#define BC_EXPORTS_INCLUDED__

#include <BC/Config.h>

/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#define BC_PLATFORM_NORETURN_PRE __declspec(noreturn)
#else
	#define BC_PLATFORM_NORETURN_PRE
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#define BCEXPORT __declspec(dllexport)
#define BCIMPORT __declspec(dllimport)
#else // compiler doesn't support __declspec()
#define BCEXPORT
#define BCIMPORT
#endif

// for other platforms/compilers we don't anything
#ifndef BCEXPORT
#define BCEXPORT
#define BCIMPORT
#endif

// BCDLLEXPORT maps to export declaration when building the DLL, to import
// declaration if using it or to nothing at all if we don't use wxWin DLL
#ifdef BCMAKINGDLL
#define BCDLLEXPORT BCEXPORT
#define BCDLLEXPORT_DATA(type) BCEXPORT type
#define BCDLLEXPORT_CTORFN
#define	BC_API		BCDLLEXPORT
#elif defined(BCUSINGDLL)
#define BCDLLEXPORT BCIMPORT
#define BCDLLEXPORT_DATA(type) BCIMPORT type
#define BCDLLEXPORT_CTORFN
#define	BC_API		BCIMPORT
#else // not making nor using DLL
#define BCDLLEXPORT
#define BCDLLEXPORT_DATA(type) type
#define BCDLLEXPORT_CTORFN
#define	BC_API
#endif


#ifndef ASSERT
	#ifdef WIN32
		#include <crtdbg.h>
		#define ASSERT(f) _ASSERTE((f))
	#else
		#include <BC/BCAssert.h>
		#define ASSERT(f) BC_INSIST(f)
	#endif
#endif

/*
 * Assertions
 */
#include <BC/BCAssert.h>	/* Contractual promise. */

/*% Require Assertion */
#define REQUIRE(e)				BC_REQUIRE(e)
/*% Ensure Assertion */
#define ENSURE(e)				BC_ENSURE(e)
/*% Insist Assertion */
#define INSIST(e)				BC_INSIST(e)
/*% Invariant Assertion */
#define INVARIANT(e)			BC_INVARIANT(e)

/*
 * Errors
 */
#include <BC/BCError.h>

/*% Runtime Check */
#define BC_RUNTIME_CHECK(cond)		BC_ERROR_RUNTIMECHECK(cond)

#ifndef BC_LANG_BEGINDECLS
	#define BC_LANG_BEGINDECLS		extern "C"	{
#endif

#ifndef BC_LANG_ENDDECLS
	#define BC_LANG_ENDDECLS	};
#endif

#ifndef BC_NAMESPACE_BEGINDECLS
#define BC_NAMESPACE_BEGINDECLS(ns)		namespace ns { // namespace ns begin
#endif

#ifndef BC_NAMESPACE_ENDDECLS
#define BC_NAMESPACE_ENDDECLS(ns)		}; // namespace ns end
#endif


#endif //BC_EXPORTS_INCLUDED__
