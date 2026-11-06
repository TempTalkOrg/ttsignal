
#ifndef WIN32OS_INCLUDED__
#define WIN32OS_INCLUDED__

#include "BC/Exports.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// win32os
///////////////////////////////////////////////////////////////////////////////
/*
 * Return the number of CPUs available on the system, or 1 if this cannot
 * be determined.
 */

BC_API
unsigned int
bc_win32os_majorversion(void);
/*
 * Major Version of the O/S.
 */

BC_API
unsigned int
bc_win32os_minorversion(void);
/*
 * Minor Version of the O/S.
 */

BC_API
unsigned int
bc_win32os_servicepackmajor(void);
/*
 * Major Version of the Service Pack for O/S.
 */

BC_API
unsigned int
bc_win32os_servicepackminor(void);
/*
 * Minor Version of the Service Pack for O/S.
 */

BC_API
int
bc_win32os_versioncheck(unsigned int major, unsigned int minor,
		     unsigned int updatemajor, unsigned int updateminor);

/*
 * Checks the current version of the operating system with the
 * supplied version information.
 * Returns:
 * -1	if less than the version information supplied
 *  0   if equal to all of the version information supplied
 * +1   if greater than the version information supplied
 */

BC_API
unsigned int
bc_os_ncpus(void);
/*%<
 * Return the number of CPUs available on the system, or 1 if this cannot
 * be determined.
 */

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

};

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////