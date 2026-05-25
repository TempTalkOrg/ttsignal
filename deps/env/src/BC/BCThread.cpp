
/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include <BC/win32/BCThread.cpp>
#else
	#include <BC/linux/BCThread.cpp>
#endif
