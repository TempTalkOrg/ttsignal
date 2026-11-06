
#ifndef BC_BCASSERT_INCLUDED__
#define BC_BCASSERT_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class :
///////////////////////////////////////////////////////////////////////////////

/*% bc assertion type */
typedef enum {
	bc_assertiontype_require,
	bc_assertiontype_ensure,
	bc_assertiontype_insist,
	bc_assertiontype_invariant
} bc_assertiontype_t;

typedef void (*bc_assertioncallback_t)(const char *, int, bc_assertiontype_t,
					const char *);

/* coverity[+kill] */
BC_API
BC_PLATFORM_NORETURN_PRE
void bc_assertion_failed(const char *, int, bc_assertiontype_t, const char *);

BC_API
void
bc_assertion_setcallback(bc_assertioncallback_t);

BC_API
const char *
bc_assertion_typetotext(bc_assertiontype_t type);

#if defined(BC_CHECK_ALL) || defined(__COVERITY__)
#define BC_CHECK_REQUIRE		1
#define BC_CHECK_ENSURE		1
#define BC_CHECK_INSIST		1
#define BC_CHECK_INVARIANT		1
#endif

#if defined(BC_CHECK_NONE) && !defined(__COVERITY__)
#define BC_CHECK_REQUIRE		0
#define BC_CHECK_ENSURE			0
#define BC_CHECK_INSIST			0
#define BC_CHECK_INVARIANT		0
#endif

#ifndef BC_CHECK_REQUIRE
#define BC_CHECK_REQUIRE		1
#endif

#ifndef BC_CHECK_ENSURE
#define BC_CHECK_ENSURE			1
#endif

#ifndef BC_CHECK_INSIST
#define BC_CHECK_INSIST			1
#endif

#ifndef BC_CHECK_INVARIANT
#define BC_CHECK_INVARIANT		1
#endif

#if BC_CHECK_REQUIRE != 0
#define BC_REQUIRE(cond) \
	((void) ((cond) || \
		 ((bc_assertion_failed)(__FILE__, __LINE__, \
					 bc_assertiontype_require, \
					 #cond), 0)))
#else
#define BC_REQUIRE(cond)	((void) 0)
#endif /* BC_CHECK_REQUIRE */

#if BC_CHECK_ENSURE != 0
#define BC_ENSURE(cond) \
	((void) ((cond) || \
		 ((bc_assertion_failed)(__FILE__, __LINE__, \
					 bc_assertiontype_ensure, \
					 #cond), 0)))
#else
#define BC_ENSURE(cond)	((void) 0)
#endif /* BC_CHECK_ENSURE */

#if BC_CHECK_INSIST != 0
#define BC_INSIST(cond) \
	((void) ((cond) || \
		 ((bc_assertion_failed)(__FILE__, __LINE__, \
					 bc_assertiontype_insist, \
					 #cond), 0)))
#else
#define BC_INSIST(cond)	((void) 0)
#endif /* BC_CHECK_INSIST */

#if BC_CHECK_INVARIANT != 0
#define BC_INVARIANT(cond) \
	((void) ((cond) || \
		 ((bc_assertion_failed)(__FILE__, __LINE__, \
					 bc_assertiontype_invariant, \
					 #cond), 0)))
#else
#define BC_INVARIANT(cond)	((void) 0)
#endif /* BC_CHECK_INVARIANT */

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
