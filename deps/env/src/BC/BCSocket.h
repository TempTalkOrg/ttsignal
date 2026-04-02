
#ifndef BC_BCSOCKET_H_INCLUDED__
#define BC_BCSOCKET_H_INCLUDED__

/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include "BC/win32/BCSocket.h"
#else
	#include "BC/linux/BCSocket.h"
#endif

#endif // BC_BCSOCKET_H_INCLUDED__
