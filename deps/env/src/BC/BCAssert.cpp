
#include "BC/Exports.h"
#include "BC/BCAssert.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class :
///////////////////////////////////////////////////////////////////////////////


/*
 * The maximum number of stack frames to dump on assertion failure.
 */
#ifndef BACKTRACE_MAXFRAME
#define BACKTRACE_MAXFRAME 128
#endif

/*%
 * Forward.
 */
static void
default_callback(const char *, int, bc_assertiontype_t, const char *);

static bc_assertioncallback_t bc_assertion_failed_cb = default_callback;

/*%
 * Public.
 */

/*% assertion failed handler */
/* coverity[+kill] */
void
bc_assertion_failed(
	const char *file,
	int line,
	bc_assertiontype_t type,
	const char *cond)
{
	bc_assertion_failed_cb(file, line, type, cond);
	abort();
	/* NOTREACHED */
}

/*% Set callback. */
void
bc_assertion_setcallback(bc_assertioncallback_t cb)
{
	if (cb == NULL)
		bc_assertion_failed_cb = default_callback;
	else
		bc_assertion_failed_cb = cb;
}

/*% Type to Text */
const char *
bc_assertion_typetotext(bc_assertiontype_t type)
{
	const char *result;

	/*
	 * These strings have purposefully not been internationalized
	 * because they are considered to essentially be keywords of
	 * the BC development environment.
	 */
	switch (type) {
	case bc_assertiontype_require:
		result = "REQUIRE";
		break;
	case bc_assertiontype_ensure:
		result = "ENSURE";
		break;
	case bc_assertiontype_insist:
		result = "INSIST";
		break;
	case bc_assertiontype_invariant:
		result = "INVARIANT";
		break;
	default:
		result = NULL;
	}
	return (result);
}

/*
 * Private.
 */

static void
default_callback(
	const char *file,
	int line,
	bc_assertiontype_t type,
	const char *cond)
{
	const char *logsuffix = ".";

	fprintf(stderr, "%s:%d: %s(%s) %s%s\n",	file, line,
		bc_assertion_typetotext(type), cond, "failed", logsuffix);
	fflush(stderr);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
