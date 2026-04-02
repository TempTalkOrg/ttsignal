
#ifndef BC_TIME_H_INCLUDED__
#define BC_TIME_H_INCLUDED__

/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include <BC/win32/BCTime.h>
#else
	#include <BC/linux/BCTime.h>
#endif

uint32_t	GetShortTimeMicroseconds();

#endif // BC_TIME_H_INCLUDED__
