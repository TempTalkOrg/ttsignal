
#ifndef BC_BCTHREAD_H_INCLUDED__
#define BC_BCTHREAD_H_INCLUDED__

#include "BC/base/spinning_mutex.h"

/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include <BC/win32/BCThread.h>
#else
	#include <BC/linux/BCThread.h>
#endif

//using BCSpinMutex = BC::BCMutex;

using BCSpinMutex = Base::SpinningMutex;

#endif // BC_BCTHREAD_H_INCLUDED__
