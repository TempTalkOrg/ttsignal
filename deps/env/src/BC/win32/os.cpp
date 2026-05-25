
#include "BC/Utils.h"
#include "BC/win32/os.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// win32os
///////////////////////////////////////////////////////////////////////////////

static BOOL bInit = FALSE;
static OSVERSIONINFOEX osVer;

static void
initialize_action(void)
{
	BOOL bSuccess;

	if (bInit)
		return;
	/*
	 * NOTE: VC++ 6.0 gets this function declaration wrong
	 * so we compensate by casting the argument
	 */
	osVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bSuccess = GetVersionEx((OSVERSIONINFO *) &osVer);

	/*
	 * Versions of NT before NT4.0 SP6 did not return the
	 * extra info that the EX structure provides and returns
	 * a failure so we need to retry with the old structure.
	 */
	if(!bSuccess)
	{
		osVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		bSuccess = GetVersionEx((OSVERSIONINFO *) &osVer);
	}
	bInit = TRUE;
}

unsigned int
bc_win32os_majorversion(void)
{
	initialize_action();
	return ((unsigned int)osVer.dwMajorVersion);
}

unsigned int
bc_win32os_minorversion(void)
{
	initialize_action();
	return ((unsigned int)osVer.dwMinorVersion);
}

unsigned int
bc_win32os_servicepackmajor(void)
{
	initialize_action();
	return ((unsigned int)osVer.wServicePackMajor);
}

unsigned int
bc_win32os_servicepackminor(void)
{
	initialize_action();
	return ((unsigned int)osVer.wServicePackMinor);
}

int
bc_win32os_versioncheck(
	unsigned int major,
	unsigned int minor,
	unsigned int spmajor,
	unsigned int spminor)
{

	initialize_action();

	if (major < bc_win32os_majorversion())
		return (1);
	if (major > bc_win32os_majorversion())
		return (-1);
	if (minor < bc_win32os_minorversion())
		return (1);
	if (minor > bc_win32os_minorversion())
		return (-1);
	if (spmajor < bc_win32os_servicepackmajor())
		return (1);
	if (spmajor > bc_win32os_servicepackmajor())
		return (-1);
	if (spminor < bc_win32os_servicepackminor())
		return (1);
	if (spminor > bc_win32os_servicepackminor())
		return (-1);

	/* Exact */
	return (0);
}

static BOOL bInit2 = FALSE;
static SYSTEM_INFO SystemInfo;

static void
initialize_action2(void)
{
	if (bInit2)
		return;

	GetSystemInfo(&SystemInfo);
	bInit2 = TRUE;
}

unsigned int
bc_os_ncpus(void)
{
	long ncpus = 1;
	initialize_action2();
	ncpus = SystemInfo.dwNumberOfProcessors;
	if (ncpus <= 0)
		ncpus = 1;

	return ((unsigned int)ncpus);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
