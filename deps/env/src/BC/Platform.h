
#ifndef BC_PLATFORM_H_INCLUDED__
#define BC_PLATFORM_H_INCLUDED__

/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include <BC/win32/Platform.h>
#else
	#include <BC/linux/Platform.h>
#endif

#endif // BC_PLATFORM_H_INCLUDED__
