
/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include "BC/win32/BCSocket.cpp"
#else
	#include "BC/linux/BCSocket.cpp"
#endif
