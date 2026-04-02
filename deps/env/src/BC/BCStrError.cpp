
#include <BC/Utils.h>
#include <BC/BCThread.h>
#include <BC/BCLog.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// strerror
///////////////////////////////////////////////////////////////////////////////

/*
 * We need to do this this way for profiled locks.
 */

static BCMutex bc_strerror_lock;

#ifdef _WIN32

/*
 * Forward declarations
 */

char *
FormatError(int error);

const char *
GetWSAErrorMessage(int errval);

char *
NTstrerror(int err, BOOL *bfreebuf);

/*
 * This routine needs to free up any buffer allocated by FormatMessage
 * if that routine gets used.
 */

void
bc_strerror(int num, char *buf, size_t size)
{
	char *msg;
	BOOL freebuf;
	unsigned int unum = num;

	ASSERT(buf != NULL);

	bc_strerror_lock.Lock();
	freebuf = FALSE;
	msg = NTstrerror(num, &freebuf);
	if (msg != NULL)
		snprintf(buf, size, "%s", msg);
	else
		snprintf(buf, size, "Unknown error: %u", unum);
	if (freebuf && msg != NULL)
	{
		LocalFree(msg);
	}
	bc_strerror_lock.Unlock();
}

/*
 * Note this will cause a memory leak unless the memory allocated here
 * is freed by calling LocalFree.  bc__strerror does this before unlocking.
 * This only gets called if there is a system type of error and will likely
 * be an unusual event.
 */
char *
FormatError(int error)
{
	LPVOID lpMsgBuf = NULL;
	FormatMessage(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER |
	    FORMAT_MESSAGE_FROM_SYSTEM |
	    FORMAT_MESSAGE_IGNORE_INSERTS,
	    NULL,
	    error,
	    /* Default language */
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	    (LPTSTR) &lpMsgBuf,
	    0,
	    NULL);

	return (LPSTR)(lpMsgBuf);
}

/*
 * This routine checks the error value and calls the WSA Windows Sockets
 * Error message function GetWSAErrorMessage below if it's within that range
 * since those messages are not available in the system error messages.
 */
char *
NTstrerror(int err, BOOL *bfreebuf)
{
	const char *retmsg = NULL;

	/* Copy the error value first in case of other errors */
	DWORD errval = err;

	*bfreebuf = FALSE;

	/* Get the Winsock2 error messages */
	if (errval >= WSABASEERR && errval <= (WSABASEERR + 1015))
	{
		retmsg = GetWSAErrorMessage(errval);
		if (retmsg != NULL)
			return (LPSTR)(retmsg);
	}
	/*
	 * If it's not one of the standard Unix error codes,
	 * try a system error message
	 */
	if (errval > (DWORD) _sys_nerr)
	{
		*bfreebuf = TRUE;
		return (FormatError(errval));
	}
	else
	{
		return (strerror(errval));
	}
}

/*
 * This is a replacement for perror
 */
void __cdecl
NTperror(char *errmsg)
{
	/* Copy the error value first in case of other errors */
	int errval = errno;
	BOOL bfreebuf = FALSE;
	char *msg;

	msg = NTstrerror(errval, &bfreebuf);
	fprintf(stderr, "%s: %s\n", errmsg, msg);
	if (bfreebuf)
	{
		LocalFree(msg);
	}

}

/*
 * Return the error string related to Winsock2 errors.
 * This function is necessary since FormatMessage knows nothing about them
 * and there is no function to get them.
 */
const char *
GetWSAErrorMessage(int errval)
{
	const char *msg;

	switch (errval)
	{

	case WSAEINTR:
		msg = "Interrupted system call";
		break;

	case WSAEBADF:
		msg = "Bad file number";
		break;

	case WSAEACCES:
		msg = "Permission denied";
		break;

	case WSAEFAULT:
		msg = "Bad address";
		break;

	case WSAEINVAL:
		msg = "Invalid argument";
		break;

	case WSAEMFILE:
		msg = "Too many open sockets";
		break;

	case WSAEWOULDBLOCK:
		msg = "Operation would block";
		break;

	case WSAEINPROGRESS:
		msg = "Operation now in progress";
		break;

	case WSAEALREADY:
		msg = "Operation already in progress";
		break;

	case WSAENOTSOCK:
		msg = "Socket operation on non-socket";
		break;

	case WSAEDESTADDRREQ:
		msg = "Destination address required";
		break;

	case WSAEMSGSIZE:
		msg = "Message too long";
		break;

	case WSAEPROTOTYPE:
		msg = "Protocol wrong type for socket";
		break;

	case WSAENOPROTOOPT:
		msg = "Bad protocol option";
		break;

	case WSAEPROTONOSUPPORT:
		msg = "Protocol not supported";
		break;

	case WSAESOCKTNOSUPPORT:
		msg = "Socket type not supported";
		break;

	case WSAEOPNOTSUPP:
		msg = "Operation not supported on socket";
		break;

	case WSAEPFNOSUPPORT:
		msg = "Protocol family not supported";
		break;

	case WSAEAFNOSUPPORT:
		msg = "Address family not supported";
		break;

	case WSAEADDRINUSE:
		msg = "Address already in use";
		break;

	case WSAEADDRNOTAVAIL:
		msg = "Can't assign requested address";
		break;

	case WSAENETDOWN:
		msg = "Network is down";
		break;

	case WSAENETUNREACH:
		msg = "Network is unreachable";
		break;

	case WSAENETRESET:
		msg = "Net connection reset";
		break;

	case WSAECONNABORTED:
		msg = "Software caused connection abort";
		break;

	case WSAECONNRESET:
		msg = "Connection reset by peer";
		break;

	case WSAENOBUFS:
		msg = "No buffer space available";
		break;

	case WSAEISCONN:
		msg = "Socket is already connected";
		break;

	case WSAENOTCONN:
		msg = "Socket is not connected";
		break;

	case WSAESHUTDOWN:
		msg = "Can't send after socket shutdown";
		break;

	case WSAETOOMANYREFS:
		msg = "Too many references: can't splice";
		break;

	case WSAETIMEDOUT:
		msg = "Connection timed out";
		break;

	case WSAECONNREFUSED:
		msg = "Connection refused";
		break;

	case WSAELOOP:
		msg = "Too many levels of symbolic links";
		break;

	case WSAENAMETOOLONG:
		msg = "File name too long";
		break;

	case WSAEHOSTDOWN:
		msg = "Host is down";
		break;

	case WSAEHOSTUNREACH:
		msg = "No route to host";
		break;

	case WSAENOTEMPTY:
		msg = "Directory not empty";
		break;

	case WSAEPROCLIM:
		msg = "Too many processes";
		break;

	case WSAEUSERS:
		msg = "Too many users";
		break;

	case WSAEDQUOT:
		msg = "Disc quota exceeded";
		break;

	case WSAESTALE:
		msg = "Stale NFS file handle";
		break;

	case WSAEREMOTE:
		msg = "Too many levels of remote in path";
		break;

	case WSASYSNOTREADY:
		msg = "Network system is unavailable";
		break;

	case WSAVERNOTSUPPORTED:
		msg = "Winsock version out of range";
		break;

	case WSANOTINITIALISED:
		msg = "WSAStartup not yet called";
		break;

	case WSAEDISCON:
		msg = "Graceful shutdown in progress";
		break;
		/*
			case WSAHOST_NOT_FOUND:
				msg = "Host not found";
				break;

			case WSANO_DATA:
				msg = "No host data of that type was found";
				break;
		*/
	default:
		msg = NULL;
		break;
	}
	return (msg);
}

/*
 * These error messages are more informative about CryptAPI Errors than the
 * standard error messages
 */

const char *
GetCryptErrorMessage(int errval)
{
	const char *msg;

	switch (errval)
	{

	case NTE_BAD_FLAGS:
		msg = "The dwFlags parameter has an illegal value.";
		break;
	case NTE_BAD_KEYSET:
		msg = "The Registry entry for the key container "
		      "could not be opened and may not exist.";
		break;
	case NTE_BAD_KEYSET_PARAM:
		msg = "The pszContainer or pszProvider parameter "
		      "is set to an illegal value.";
		break;
	case NTE_BAD_PROV_TYPE:
		msg = "The value of the dwProvType parameter is out "
		      "of range. All provider types must be from "
		      "1 to 999, inclusive.";
		break;
	case NTE_BAD_SIGNATURE:
		msg = "The provider DLL signature did not verify "
		      "correctly. Either the DLL or the digital "
		      "signature has been tampered with.";
		break;
	case NTE_EXISTS:
		msg = "The dwFlags parameter is CRYPT_NEWKEYSET, but the key"
		      " container already exists.";
		break;
	case NTE_KEYSET_ENTRY_BAD:
		msg = "The Registry entry for the pszContainer key container "
		      "was found (in the HKEY_CURRENT_USER window), but is "
		      "corrupt. See the section System Administration for "
		      " etails about CryptoAPI's Registry usage.";
		break;
	case NTE_KEYSET_NOT_DEF:
		msg = "No Registry entry exists in the HKEY_CURRENT_USER "
		      "window for the key container specified by "
		      "pszContainer.";
		break;
	case NTE_NO_MEMORY:
		msg = "The CSP ran out of memory during the operation.";
		break;
	case NTE_PROV_DLL_NOT_FOUND:
		msg = "The provider DLL file does not exist or is not on the "
		      "current path.";
		break;
	case NTE_PROV_TYPE_ENTRY_BAD:
		msg = "The Registry entry for the provider type specified by "
		      "dwProvType is corrupt. This error may relate to "
		      "either the user default CSP list or the machine "
		      "default CSP list. See the section System "
		      "Administration for details about CryptoAPI's "
		      "Registry usage.";
		break;
	case NTE_PROV_TYPE_NO_MATCH:
		msg = "The provider type specified by dwProvType does not "
		      "match the provider type found in the Registry. Note "
		      "that this error can only occur when pszProvider "
		      "specifies an actual CSP name.";
		break;
	case NTE_PROV_TYPE_NOT_DEF:
		msg = "No Registry entry exists for the provider type "
		      "specified by dwProvType.";
		break;
	case NTE_PROVIDER_DLL_FAIL:
		msg = "The provider DLL file could not be loaded, and "
		      "may not exist. If it exists, then the file is "
		      "not a valid DLL.";
		break;
	case NTE_SIGNATURE_FILE_BAD:
		msg = "An error occurred while loading the DLL file image, "
		      "prior to verifying its signature.";
		break;

	default:
		msg = NULL;
		break;
	}
	return msg;
}

///////////////////////////////////////////////////////////////////////////////
// errno2result
///////////////////////////////////////////////////////////////////////////////

/*
 * Convert a POSIX errno value into an BCRESULT.  The
 * list of supported errno values is not complete; new users
 * of this function should add any expected errors that are
 * not already there.
 */
BCRESULT
bc_errno2resultx(int posixerrno, const char *file, int line)
{
	char strbuf[BC_STRERRORSIZE];

	switch (posixerrno)
	{
	case ENOTDIR:
	case WSAELOOP:
	case WSAEINVAL:
	case EINVAL:		/* XXX sometimes this is not for files */
	case ENAMETOOLONG:
	case WSAENAMETOOLONG:
	case EBADF:
	case WSAEBADF:
		return (BC_R_INVALIDFILE);
	case ENOENT:
		return (BC_R_FILENOTFOUND);
	case EACCES:
	case WSAEACCES:
	case EPERM:
		return (BC_R_NOPERM);
	case EEXIST:
		return (BC_R_FILEEXISTS);
	case EIO:
		return (BC_R_IOERROR);
	case ENOMEM:
		return (BC_R_NOMEMORY);
	case ENFILE:
	case EMFILE:
	case WSAEMFILE:
		return (BC_R_TOOMANYOPENFILES);
	case ERROR_CANCELLED:
		return (BC_R_CANCELED);
	case ERROR_CONNECTION_REFUSED:
	case WSAECONNREFUSED:
		return (BC_R_CONNREFUSED);
	case WSAENOTCONN:
	case ERROR_CONNECTION_INVALID:
		return (BC_R_NOTCONNECTED);
	case ERROR_HOST_UNREACHABLE:
	case WSAEHOSTUNREACH:
		return (BC_R_HOSTUNREACH);
	case ERROR_NETWORK_UNREACHABLE:
	case WSAENETUNREACH:
		return (BC_R_NETUNREACH);
	case ERROR_NO_NETWORK:
		return (BC_R_NETUNREACH);
	case ERROR_PORT_UNREACHABLE:
		return (BC_R_HOSTUNREACH);
	case ERROR_SEM_TIMEOUT:
		return (BC_R_TIMEDOUT);
	case WSAECONNRESET:
	case WSAENETRESET:
	case WSAECONNABORTED:
	case WSAEDISCON:
	case ERROR_OPERATION_ABORTED:
	case ERROR_CONNECTION_ABORTED:
	case ERROR_REQUEST_ABORTED:
		return (BC_R_CONNECTIONRESET);
	case WSAEADDRNOTAVAIL:
		return (BC_R_ADDRNOTAVAIL);
	case ERROR_NETNAME_DELETED:
	case WSAENETDOWN:
		return (BC_R_NETUNREACH);
	case WSAEHOSTDOWN:
		return (BC_R_HOSTUNREACH);
	case WSAENOBUFS:
		return (BC_R_NORESOURCES);
	default:
		bc_strerror(posixerrno, strbuf, sizeof(strbuf));
#ifdef _DEBUG
		LogError(file, line, "", "unable to convert errno "
				 "to BCRESULT: %d: %s", posixerrno, strbuf);
#endif
		/*
		 * XXXDCL would be nice if perhaps this function could
		 * return the system's error string, so the caller
		 * might have something more descriptive than "unexpected
		 * error" to log with.
		 */
		return (BC_R_UNEXPECTED);
	}
}

#else // !_WIN32

#define HAVE_STRERROR

void
bc_strerror(int num, char *buf, size_t size)
{
#ifdef HAVE_STRERROR
	char *msg;
	unsigned int unum = (unsigned int)num;

	REQUIRE(buf != NULL);

	bc_strerror_lock.Lock();
	msg = strerror(num);
	if (msg != NULL)
		snprintf(buf, size, "%s", msg);
	else
		snprintf(buf, size, "Unknown error: %u", unum);
	bc_strerror_lock.Unlock();
#else
	unsigned int unum = (unsigned int)num;

	REQUIRE(buf != NULL);

	if (num >= 0 && num < sys_nerr)
		snprintf(buf, size, "%s", sys_errlist[num]);
	else
		snprintf(buf, size, "Unknown error: %u", unum);
#endif
}

/*%
 * Convert a POSIX errno value into an BCRESULT.  The
 * list of supported errno values is not complete; new users
 * of this function should add any expected errors that are
 * not already there.
 */
BCRESULT
bc__errno2resultx(int posixerrno)
{
	char strbuf[BC_STRERRORSIZE];

	switch (posixerrno)
	{
	case ENOTDIR:
	case ELOOP:
	case EINVAL:		/* XXX sometimes this is not for files */
	case ENAMETOOLONG:
	case EBADF:
		return (BC_R_INVALIDFILE);
	case ENOENT:
		return (BC_R_FILENOTFOUND);
	case EACCES:
	case EPERM:
		return (BC_R_NOPERM);
	case EEXIST:
		return (BC_R_FILEEXISTS);
	case EIO:
		return (BC_R_IOERROR);
	case ENOMEM:
		return (BC_R_NOMEMORY);
	case ENFILE:
	case EMFILE:
		return (BC_R_TOOMANYOPENFILES);
	case EPIPE:
#ifdef ECONNRESET
	case ECONNRESET:
#endif
#ifdef ECONNABORTED
	case ECONNABORTED:
#endif
		return (BC_R_CONNECTIONRESET);
#ifdef ENOTCONN
	case ENOTCONN:
		return (BC_R_NOTCONNECTED);
#endif
#ifdef ETIMEDOUT
	case ETIMEDOUT:
		return (BC_R_TIMEDOUT);
#endif
#ifdef ENOBUFS
	case ENOBUFS:
		return (BC_R_NORESOURCES);
#endif
#ifdef EAFNOSUPPORT
	case EAFNOSUPPORT:
		return (BC_R_FAMILYNOSUPPORT);
#endif
#ifdef ENETDOWN
	case ENETDOWN:
		return (BC_R_NETDOWN);
#endif
#ifdef EHOSTDOWN
	case EHOSTDOWN:
		return (BC_R_HOSTDOWN);
#endif
#ifdef ENETUNREACH
	case ENETUNREACH:
		return (BC_R_NETUNREACH);
#endif
#ifdef EHOSTUNREACH
	case EHOSTUNREACH:
		return (BC_R_HOSTUNREACH);
#endif
#ifdef EADDRINUSE
	case EADDRINUSE:
		return (BC_R_ADDRINUSE);
#endif
	case EADDRNOTAVAIL:
		return (BC_R_ADDRNOTAVAIL);
	case ECONNREFUSED:
		return (BC_R_CONNREFUSED);
	default:
		bc_strerror(posixerrno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
				 "unable to convert errno "
				 "to bc_result: %d: %s",
				 posixerrno, strbuf);
		/*
		 * XXXDCL would be nice if perhaps this function could
		 * return the system's error string, so the caller
		 * might have something more descriptive than "unexpected
		 * error" to log with.
		 */
		return (BC_R_UNEXPECTED);
	}
}

#endif // _WIN32

LPCSTR bc_result2string(BCRESULT result)
{
	switch(result)
	{
	case    BC_R_SUCCESS			: return "success";
	case    BC_R_NOMEMORY			: return "out of memory";
	case    BC_R_TIMEDOUT			: return "timed out";
	case    BC_R_NOTHREADS			: return "no available threads";
	case    BC_R_ADDRNOTAVAIL		: return "address not available";
	case    BC_R_ADDRINUSE			: return "address in use";
	case    BC_R_NOPERM			    : return "permission denied";
	case    BC_R_NOCONN				: return "no pending connections";
	case    BC_R_NETUNREACH			: return "network unreachable";
	case    BC_R_HOSTUNREACH		: return "host unreachable";
	case    BC_R_NETDOWN			: return "network down";
	case    BC_R_HOSTDOWN			: return "host down";
	case    BC_R_CONNREFUSED		: return "connection refused";
	case    BC_R_NORESOURCES		: return "not enough free resources";
	case    BC_R_EOF				: return "end of file";
	case    BC_R_BOUND				: return "socket already bound";
	case    BC_R_RELOAD				: return "reload";
	case    BC_R_LOCKBUSY			: return "lock busy";
	case    BC_R_EXISTS				: return "already exists";
	case    BC_R_NOSPACE			: return "ran out of space";
	case    BC_R_CANCELED			: return "operation canceled";
	case    BC_R_NOTBOUND			: return "socket is not bound";
	case    BC_R_SHUTTINGDOWN		: return "shutting down";
	case    BC_R_NOTFOUND			: return "not found";
	case    BC_R_UNEXPECTEDEND		: return "unexpected end of input";
	case    BC_R_FAILURE			: return "generic failure";
	case    BC_R_IOERROR			: return "I/O error";
	case    BC_R_NOTIMPLEMENTED		: return "not implemented";
	case    BC_R_UNBALANCED			: return "unbalanced parentheses";
	case    BC_R_NOMORE				: return "no more";
	case    BC_R_INVALIDFILE		: return "invalid file";
	case    BC_R_BADBASE64			: return "bad base64 encoding";
	case    BC_R_UNEXPECTEDTOKEN	: return "unexpected token";
	case    BC_R_QUOTA				: return "quota reached";
	case    BC_R_UNEXPECTED			: return "unexpected error";
	case    BC_R_ALREADYRUNNING		: return "already running";
	case    BC_R_IGNORE				: return "ignore";
	case    BC_R_MASKNONCONTIG      : return "addr mask not contiguous";
	case    BC_R_FILENOTFOUND		: return "file not found";
	case    BC_R_FILEEXISTS			: return "file already exists";
	case    BC_R_NOTCONNECTED		: return "socket is not connected";
	case    BC_R_RANGE				: return "out of range";
	case    BC_R_NOENTROPY			: return "out of entropy";
	case    BC_R_MULTICAST			: return "invalid use of multicast";
	case    BC_R_NOTFILE			: return "not a file";
	case    BC_R_NOTDIRECTORY		: return "not a directory";
	case    BC_R_QUEUEFULL			: return "queue is full";
	case    BC_R_FAMILYMISMATCH		: return "address family mismatch";
	case    BC_R_FAMILYNOSUPPORT	: return "AF not supported";
	case    BC_R_BADHEX				: return "bad hex encoding";
	case    BC_R_TOOMANYOPENFILES	: return "too many open files";
	case    BC_R_NOTBLOCKING		: return "not blocking";
	case    BC_R_UNBALANCEDQUOTES	: return "unbalanced quotes";
	case    BC_R_INPROGRESS			: return "operation in progress";
	case    BC_R_CONNECTIONRESET	: return "connection reset";
	case    BC_R_SOFTQUOTA			: return "soft quota reached";
	case    BC_R_BADNUMBER			: return "not a valid number";
	case    BC_R_DISABLED			: return "disabled";
	case    BC_R_MAXSIZE			: return "max size";
	case    BC_R_BADADDRESSFORM		: return "invalid address format";
	case    BC_R_BADBASE32			: return "bad base32 encoding";
	case    BC_R_DBERROR			: return "database error";
	case    BC_R_INVALIDPTR			: return "invalid pointer";
	case    BC_R_INVALIDARG			: return "invalid arguments";
	default                         : return "out of result range";
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace :BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
