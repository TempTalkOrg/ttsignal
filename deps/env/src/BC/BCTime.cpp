
/*
 * Defines for the noreturn attribute.
 */
#ifdef _WIN32
	#include <BC/win32/BCTime.cpp>
#else
	#include <BC/linux/BCTime.cpp>
#endif

uint32_t GetShortTimeMicroseconds()
{
	bc_time_t tmNow = bc_time_now();
	return (tmNow & 0xFFFFFFFF);
}
