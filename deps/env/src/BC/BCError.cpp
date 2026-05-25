
#include "BC/Exports.h"
#include "BC/BCError.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class :
///////////////////////////////////////////////////////////////////////////////


/*% Default unexpected callback. */
static void
default_unexpected_callback(const char *, int, const char *, va_list);

/*% Default fatal callback. */
static void
default_fatal_callback(const char *, int, const char *, va_list);

/*% unexpected_callback */
static bc_errorcallback_t unexpected_callback = default_unexpected_callback;
static bc_errorcallback_t fatal_callback = default_fatal_callback;

void
bc_error_setunexpected(bc_errorcallback_t cb)
{
	if (cb == NULL)
		unexpected_callback = default_unexpected_callback;
	else
		unexpected_callback = cb;
}

void
bc_error_setfatal(bc_errorcallback_t cb)
{
	if (cb == NULL)
		fatal_callback = default_fatal_callback;
	else
		fatal_callback = cb;
}

void
bc_error_unexpected(
	const char *file,
	int line,
	const char *format,
	...)
{
	va_list args;

	va_start(args, format);
	(unexpected_callback)(file, line, format, args);
	va_end(args);
}

void
bc_error_fatal(
	const char *file,
	int line,
	const char *format,
	...)
{
	va_list args;

	va_start(args, format);
	(fatal_callback)(file, line, format, args);
	va_end(args);
	abort();
}

void
bc_error_runtimecheck(
	const char *file,
	int line,
	const char *expression)
{
	bc_error_fatal(file, line, "RUNTIME_CHECK(%s) %s", expression, "failed");
}

static void
default_unexpected_callback(
	const char *file,
	int line,
	const char *format,
	va_list args)
{
	fprintf(stderr, "%s:%d: ", file, line);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static void
default_fatal_callback(
	const char *file,
	int line, const char *format,
		       va_list args)
{
	fprintf(stderr, "%s:%d: %s: ", file, line, "fatal error");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
