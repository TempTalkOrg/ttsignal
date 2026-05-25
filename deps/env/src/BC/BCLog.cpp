
#include <BC/Utils.h>
#include <BC/BCPString.h>
#include <sys/stat.h>
#include <BC/BCBuffer.h>
#include "BC/BCLog.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif // __ANDROID__

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Global log utilities
///////////////////////////////////////////////////////////////////////////////

static void
vlog(const void* logger_ctx, int32_t level, const char * szFileName, uint32_t nLineNO,
	 const char * szFuncName, const char * szFmtStr, va_list args)
{
	BCLogger::GetInstance()->Log(logger_ctx, level, szFileName, nLineNO, 
		szFuncName, szFmtStr, args);
}
void
LogFatal(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogError(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _ERROR_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogWarn(const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _WARN_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogInfo(const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _INFO_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogDebug(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _DEBUG_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogFine(const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _FINE_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogFinest(const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _FINEST_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogAssert(const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(NULL, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
	assert(false);
	abort();
}
void
LogFatal(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogError(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _ERROR_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogWarn(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _WARN_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogInfo(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _INFO_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogDebug(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _DEBUG_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogFine(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _FINE_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogFinest(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _FINEST_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
}
void
LogAssert(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	vlog(logger_ctx, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	va_end(args);
	assert(false);
	abort();
}

void
LogCustom(const void *logger_ctx, int32_t nLevel, const char* szFmtStr, ...)
{
	va_list args;
	va_start(args, szFmtStr);
	BCLogger::GetInstance()->Log(logger_ctx, nLevel, szFmtStr, args);
	va_end(args);
}

void
LogCustomWithTime(const void *logger_ctx, int32_t nLevel, const char* szFmtStr, ...)
{
	char timestr[50] = { 0 };
	BCLogBase::GetTime(timestr, sizeof(timestr));
	BCPString strFmt;
	strFmt.Format("[%s]%s", timestr, szFmtStr);
	va_list args;
	va_start(args, szFmtStr);
	BCLogger::GetInstance()->Log(logger_ctx, nLevel, strFmt.c_str(), args);
	va_end(args);
}
void
LogBin(const char * szFileName, uint32_t nLineNO,
	   const char * szFuncName, const void *pData, uint32_t dataLen)
{
	BCLogger::GetInstance()->LogBinary(NULL, szFileName, nLineNO,
		szFuncName, pData, dataLen);
}
void
LogBin(const void *logger_ctx, const char * szFileName, uint32_t nLineNO,
	   const char * szFuncName, const void *pData, uint32_t dataLen)
{
	BCLogger::GetInstance()->LogBinary(logger_ctx, szFileName, nLineNO,
		szFuncName, pData, dataLen);
}

void
LogBuffer(const char * szFileName, uint32_t nLineNO,
	const char * szFuncName, BCBuffer *pBuffer)
{
	BCLogger::GetInstance()->LogBuffer(NULL, szFileName, nLineNO,
		szFuncName, pBuffer);
}
void
LogBuffer(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
	const char * szFuncName, BCBuffer *pBuffer)
{
	BCLogger::GetInstance()->LogBuffer(logger_ctx, szFileName, nLineNO,
		szFuncName, pBuffer);
}

void
LogFatalV(const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogErrorV(const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _ERROR_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogWarnV(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _WARN_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogInfoV(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _INFO_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogDebugV(const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _DEBUG_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogFineV(const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _FINE_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogFinestV(const char * szFileName, uint32_t nLineNO,
		   const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _FINEST_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogAssertV(const char * szFileName, uint32_t nLineNO,
		   const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(NULL, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	assert(0);
	abort();
}

void
LogFatalV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogErrorV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _ERROR_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogWarnV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _WARN_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogInfoV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _INFO_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogDebugV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		  const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _DEBUG_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogFineV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		 const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _FINE_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogFinestV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		   const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _FINEST_, szFileName, nLineNO, szFuncName, szFmtStr, args);
}
void
LogAssertV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
		   const char * szFuncName, const char * szFmtStr, va_list args)
{
	vlog(logger_ctx, _FATAL_, szFileName, nLineNO, szFuncName, szFmtStr, args);
	assert(0);
	abort();
}

void
LogCustomV(const void *logger_ctx, int32_t nLevel, const char* szFmtStr, va_list args)
{
	BCLogger::GetInstance()->Log(logger_ctx, nLevel, szFmtStr, args);
}

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : BCLogger
///////////////////////////////////////////////////////////////////////////////


BCLogger *BCLogger::s_pLogger = NULL;

BCLogger::BCLogger()
{
}

BCLogger::~BCLogger()
{
}

BCLogger * BCLogger::GetInstance()
{
	if (s_pLogger == NULL)
	{
		s_pLogger = new BCLogger();
	}
	return s_pLogger;
}

void BCLogger::Log(
	const void* logger_ctx,
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szFMT,
	va_list args)
{
	char *fmtStr = NULL;

	bc_vasprintf(&fmtStr, szFMT, args);
	if (fmtStr)
	{
		std::string strBaseFileName = GetBasePathName(szFileName);
		BCSpinMutex::Owner lock(s_lock);
		if (logger_ctx)
		{
			((BCLogBase*)logger_ctx)->Log(nLevel, strBaseFileName.c_str(), 
				nLineNumber, szFuncName, fmtStr);
		}
		else
		{
			for (auto& iter : s_logLocations)
			{
				if (!iter->m_bExclusive)
				{
					iter->Log(nLevel, strBaseFileName.c_str(), nLineNumber, 
						szFuncName, fmtStr);
				}
			}
		}
		free(fmtStr);
	}
}

void BCLogger::Log(
	const void* logger_ctx,
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szFMT,
	...)
{
	char *fmtStr = NULL;
	va_list args;
	va_start(args, szFMT);
	bc_vasprintf(&fmtStr, szFMT, args);
	va_end(args);

	if (fmtStr)
	{
		std::string strBaseFileName = GetBasePathName(szFileName);
		BCSpinMutex::Owner lock(s_lock);
		if (logger_ctx)
		{
			((BCLogBase*)logger_ctx)->Log(nLevel, strBaseFileName.c_str(), 
				nLineNumber, szFuncName, fmtStr);
		}
		else
		{
			for (auto& iter : s_logLocations)
			{
				if (!iter->m_bExclusive)
				{
					iter->Log(nLevel, strBaseFileName.c_str(), nLineNumber, 
						szFuncName, fmtStr);
				}
			}
		}
		free(fmtStr);
	}
}

void BCLogger::Log(
	const void* logger_ctx,
	int32_t nLevel,
	LPCSTR szFMT, 
	va_list args)
{
	char *fmtStr = NULL;

	bc_vasprintf(&fmtStr, szFMT, args);
	if (fmtStr)
	{
		BCSpinMutex::Owner lock(s_lock);
		if (logger_ctx)
		{
			((BCLogBase*)logger_ctx)->Log(nLevel, fmtStr);
		}
		else
		{
			for (auto& iter : s_logLocations)
			{
				if (!iter->m_bExclusive)
				{
					iter->Log(nLevel, fmtStr);
				}
			}
		}
		free(fmtStr);
	}
}

void BCLogger::LogBinary(
	const void* logger_ctx,
	LPCSTR fileName,
	uint32_t lineNumber,
	LPCSTR functionName,
	const void *pData,
	uint32_t dataLen)
{
	std::string strBaseFileName = GetBasePathName(fileName);
	BCSpinMutex::Owner lock(s_lock);
	if (logger_ctx)
	{
		((BCLogBase*)logger_ctx)->LogBinary(strBaseFileName.c_str(), lineNumber,
			functionName, pData, dataLen);
	}
	else
	{
		for (auto& iter : s_logLocations)
		{
			if (!iter->m_bExclusive)
			{
				iter->LogBinary(strBaseFileName.c_str(), lineNumber, functionName,
					pData, dataLen);
			}
		}
	}
}

void BCLogger::LogBuffer(
	const void* logger_ctx, 
	const char * szFileName, 
	uint32_t nLineNO,
	const char * szFuncName, 
	BCBuffer *pBuffer)
{
	KBPool sPool;
	LPVOID lpBuf = sPool.Calloc(pBuffer->RemainingLength());
	BCFBOStream sWriter(lpBuf, pBuffer->RemainingLength());
	LPVOID lpData;
	uint32_t nDataSize, nStartPos = pBuffer->ConsumedLength();
	while ((lpData = pBuffer->ReadBlock(INFINITE, nDataSize)) && nDataSize > 0)
	{
		sWriter.Write(lpData, nDataSize);
	}
	LogBinary(logger_ctx, szFileName, nLineNO, szFuncName, lpBuf, sWriter.UsedLength());
	pBuffer->Rewind(nStartPos);
}

void BCLogger::AddLogLocation(BCLogBase *pLogLocation)
{
	assert(pLogLocation != NULL);
	BCSpinMutex::Owner lock(s_lock);
	s_logLocations.push_back(pLogLocation);
}

void BCLogger::RemoveLogLocation(BCLogBase *pLocation)
{
	BCSpinMutex::Owner lock(s_lock);
	s_logLocations.remove(pLocation);
}

void BCLogger::RemoveAllLogLocations()
{
	BCSpinMutex::Owner lock(s_lock);
	for (BCLogBase* location : s_logLocations)
	{
		location->Close();
		delete location;
	}
	s_logLocations.clear();
}

///////////////////////////////////////////////////////////////////////////////
// class : BCLogBase
///////////////////////////////////////////////////////////////////////////////

BCLogBase::BCLogBase(bool bExclusive)
	: m_bExclusive(bExclusive)
{
	m_nLevel = -1;
	m_strName = "";
}

BCLogBase::~BCLogBase()
{
}

int32_t BCLogBase::GetLevel()
{
	return m_nLevel;
}

void BCLogBase::SetLevel(int32_t level)
{
	m_nLevel = level;
}

LPCSTR BCLogBase::GetName()
{
	return m_strName.c_str();
}

void BCLogBase::SetName(LPCSTR lpName)
{
	m_strName = lpName;
}

uint32_t BCLogBase::GetTime(char *buf, uint32_t bufsize)
{
#ifdef _WIN32
	SYSTEMTIME st;
	SYSTEMTIME ust;
	TIME_ZONE_INFORMATION tzi;

	GetTimeZoneInformation(&tzi);
	GetSystemTime(&ust);
	SystemTimeToTzSpecificLocalTime(&tzi, &ust, &st);
	return _snprintf(buf, bufsize, "%4d.%02d.%02d--%02d:%02d:%02d.%03d",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else // !_WIN32
	time_t currentTime;
	int64_t tmNowUSecs;
	int32_t tmMilliSecs;

	tmNowUSecs = bc_time_now();
	//time(&currentTime);
	currentTime = tmNowUSecs / 1000000;
	tmMilliSecs = (tmNowUSecs % 1000000) / 1000;
	struct tm *localTime = localtime(&currentTime);

	//return strftime(buf, bufsize, "%F %T", localTime);
	return snprintf(buf, bufsize, "%4d.%02d.%02d--%02d:%02d:%02d.%03d",
		localTime->tm_year + 1900, localTime->tm_mon+1, localTime->tm_mday,
		localTime->tm_hour, localTime->tm_min, localTime->tm_sec, tmMilliSecs);
#endif // _WIN32
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFileLogLocation
///////////////////////////////////////////////////////////////////////////////

#define _set_state(inst, _state, _status)	\
	(inst)->_SetState(_state, __LINE__);(inst)->m_eCloseStatus = _status

enum {
	LOGF_MSG_LOG			= 1,
	LOGF_MSG_LOG_CUSTOM		= 2,
	LOGF_MSG_BIN			= 3,
	LOGF_MSG_CLOSE			= 4,
};

enum
{
	LOGF_STATE_FREED	= 0,
	LOGF_STATE_WORKING	= 1,
	LOGF_STATE_MAX		= 2,
};

BCLogFile::BCLogFile(bool bExclusive)
	: BCLogBase(bExclusive)
	, m_pLogFile(NULL)
	, m_pTaskMgr(NULL)
	, m_eState(LOGF_STATE_WORKING)
	, m_eNewState(LOGF_STATE_MAX)
	, m_nStateLineNo(0)
	, m_eCloseStatus(BC_R_SUCCESS)
	, m_sExitCond(&m_sExitLock)
	, m_bPrintPrefix(false)
{
}

BCLogFile::~BCLogFile()
{
	Close();
}

BCRESULT BCLogFile::Create(const char* szLogFileName, bool bCWD)
{
	BCRESULT result;

	GetDefLogFilePath(m_strLogFileName, szLogFileName, bCWD);
	if (!m_strLogFileName.length())
	{
		return BC_R_INVALIDARG;
	}
	m_pLogFile = fopen(m_strLogFileName.c_str(), "a+b");
	if (!m_pLogFile)
	{
		return BC_R_INVALIDARG;
	}
	m_pTaskMgr = new BCTaskMgr();
	if (!m_pTaskMgr)
	{
		result = BC_R_NOMEMORY;
		goto close_file;
	}
	result = m_pTaskMgr->Create(1, 0, BCThread::PRIORITY_NORMAL, "BCLogFileThread");
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgr;
	}
	result = BCEventFactory::Create(m_pTaskMgr, "BCLogFile", this);
	if (result != BC_R_SUCCESS)
	{
		goto detach_task;
	}

	return BC_R_SUCCESS;

detach_task:
	BCEventFactory::Detach(true);
destroy_taskmgr:
	BCTaskMgr::Destroy(&m_pTaskMgr);
close_file:
	fclose(m_pLogFile);
	m_pLogFile = NULL;

	return result;
}

void BCLogFile::Close()
{
	BCMutex::Owner lock(m_sExitLock);
	PostEvent(MAKEEVENT(LOGF_MSG_CLOSE, 0, 0));
	m_sExitCond.Wait();
	if (m_pTaskMgr)
	{
		BCTaskMgr::Destroy(&m_pTaskMgr);
	}
}

void BCLogFile::Log(
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szMsg)
{
	BCEventItemS sEvent(MAKEEVENT(LOGF_MSG_LOG, 0, 0), nLevel, nLineNumber);
	sEvent.vParams[0] = (uint64_t)sEvent.CopyString(szFileName);
	sEvent.vParams[1] = (uint64_t)sEvent.CopyString(szFuncName);
	sEvent.vParams[2] = (uint64_t)sEvent.CopyString(szMsg);
	PostEvent(sEvent);
}

void BCLogFile::Log(int32_t nLevel, LPCSTR szMsg)
{
	if (m_nLevel >= 0 && nLevel <= m_nLevel)
	{
		BCEventItemS sEvent(MAKEEVENT(LOGF_MSG_LOG_CUSTOM, 0, 0), nLevel);
		sEvent.lParam = (uint64_t)sEvent.CopyString(szMsg);
		sEvent.vParams[7] = bc_time_now();
		PostEvent(sEvent);
	}
}

void BCLogFile::LogBinary(
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	const void* pData,
	uint32_t dataLen)
{
	BCEventItemS sEvent(MAKEEVENT(LOGF_MSG_BIN, 0, 0), nLineNumber, dataLen);
	sEvent.vParams[0] = (uint64_t)sEvent.CopyString(szFileName);
	sEvent.vParams[1] = (uint64_t)sEvent.CopyString(szFuncName);
	sEvent.vParams[2] = (uint64_t)sEvent.CopyBuffer(pData, dataLen);
	PostEvent(sEvent);
}

#ifdef WIN32

void BCLogFile::GetDefLogFilePath(
	BCPString& location, 
	const char *filename,
	bool bCWD)
{
	char app_dir[1024];
	struct stat st, *pst = &st;

	memzero(app_dir, sizeof(app_dir));
	if (IsPathAbsolute(filename))
	{
		location = filename;
		return;
	}
	if (bCWD)
	{
		::GetCurrentDirectoryA(sizeof(app_dir), app_dir);
	}
	else
	{
		char *lpFilePart = NULL;

		::GetModuleFileNameA(NULL, app_dir, sizeof(app_dir));
		GetFullPathNameA(app_dir, sizeof(app_dir), app_dir, &lpFilePart);
		*(--lpFilePart) = 0;
	}
	location = app_dir;
	location += "\\log\\";
	if (stat(location.c_str(), pst) != 0 && errno == ENOENT)
	{
		CreateDirectoryA(location.c_str(), NULL);
	}
	if (filename != NULL)
	{
		location += filename;
	}
	else
	{
		location += "bc_log.log";
	}
}
#else



int GetExeFullPath(char *lpBuf, int nBufLen)
{
    int result;

    ASSERT(lpBuf != NULL && nBufLen > 0);
    result = readlink("/proc/self/exe", lpBuf, nBufLen);
    if (result < 0 || result >= nBufLen)
    {
        return -1;
    }
    lpBuf[result] = '\0';
    return result;
}

int GetExeShortPath(const char *szFullPath, char *lpBuf, int nBufLen)
{
    int result;
    char szPath[MAX_PATH];

    ASSERT(lpBuf != NULL && nBufLen > 0);

    if (szFullPath == NULL)
    {
        result = GetExeFullPath(szPath, MAX_PATH);
        if (result < 0)
        {
            return -1;
        }
        szPath[result] = '\0';
        szFullPath = szPath;
    }
    ASSERT(szFullPath);

    const char *pPrev, *pNext;

    pNext = strchr(szFullPath, '/');
    pPrev = pNext;
    while (pNext != NULL && *pNext != '\x0')
    {
        pPrev = pNext;
        pNext = strchr(pNext + 1, '/');
    }
    if (pPrev && *pPrev != '\x0')
    {
        int nLen;

        nLen = pPrev - szFullPath;
        nLen = nLen > nBufLen?nBufLen:nLen;
        if (nLen > 0)
        {
            strncpy(lpBuf, szFullPath, nLen);
        }
        lpBuf[nLen] = '\x0';

        return nLen;
    }
    return -1;
}

void BCLogFile::GetDefLogFilePath(BCPString& location, LPCSTR szFileName, bool bCWD)
{
	char app_dir[1024];

	memzero(app_dir, sizeof(app_dir));
	if (IsPathAbsolute(szFileName))
	{
		location = szFileName;
		return;
	}
	if (bCWD)
	{
		getcwd(app_dir, sizeof(app_dir));
	}
	else
	{
		GetExeShortPath(NULL, app_dir, sizeof(app_dir));
	}
	location = BCPString(app_dir);
	location += BCPString("/log/");
	if (!bcMakDir(location.c_str()))
	{
		location = szFileName;
	}
	if (szFileName != NULL)
	{
		location += szFileName;
	}
	else
	{
		location += "bc_log.log";
	}
}
#endif

#ifdef __ANDROID__
static int bc_log_level_to_android_level(int level)
{
	int androidLevel = 0;
	switch(level)
	{
	case _FATAL_:
		androidLevel = ANDROID_LOG_FATAL;
		break;
	case _ERROR_:
		androidLevel = ANDROID_LOG_ERROR;
		break;
	case _WARN_:
		androidLevel = ANDROID_LOG_WARN;
		break;
	case _INFO_:
		androidLevel = ANDROID_LOG_INFO;
		break;
	case _DEBUG_:
		androidLevel = ANDROID_LOG_DEBUG;
		break;
	case _FINE_:
	case _FINEST_:
		androidLevel = ANDROID_LOG_VERBOSE;
		break;
	default:
		break;
	} 
	return androidLevel;
}

void BCLogFile::LogInternal(
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szMsg)
{
	int androidLevel = 0;
	char timestr[50] = {0};
	bool bLog = false;

	UNUSED(szFuncName);

	if (m_nLevel >= 0 && nLevel <= m_nLevel)
	{
		bLog = true;
	}
	if (!bLog)
	{
		return;
	}
	androidLevel = bc_log_level_to_android_level(nLevel);
	GetTime(timestr, 50);
	__android_log_print(androidLevel, "BCLog", "[%s][%s:%d]:%s", timestr,
		szFileName, nLineNumber, szMsg);
}
void BCLogFile::LogInternal(int32_t level, LPCSTR szMsg)
{
	int androidLevel = bc_log_level_to_android_level(_INFO_);
	__android_log_print(androidLevel, "BCLog", "%s", szMsg);
}
#else
void BCLogFile::LogInternal(
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szMsg)
{
	char typestr[50] = {0};
	char timestr[50] = {0};

	UNUSED(szFuncName);

	if (m_nLevel < 0 || nLevel > m_nLevel)
	{
		return;
	}
	switch(nLevel)
	{
	case _FATAL_:
		snprintf(typestr, 50, "fatal");
		break;
	case _ERROR_:
		snprintf(typestr, 50, "error");
		break;
	case _WARN_:
		snprintf(typestr, 50, "warn");
		break;
	case _INFO_:
		snprintf(typestr, 50, "info");
		break;
	case _DEBUG_:
		snprintf(typestr, 50, "debug");
		break;
	case _FINE_:
		snprintf(typestr, 50, "fine");
		break;
	case _FINEST_:
		snprintf(typestr, 50, "finest");
		break;
	default:
		return;
	}
	fseek(m_pLogFile, 0, SEEK_END);
	GetTime(timestr, 50);
	if (m_bPrintPrefix)
	{
		fprintf(m_pLogFile, "[%s][%s:%d][%s]:%s", timestr,
			szFileName, nLineNumber, typestr, szMsg);
	}
	else
	{
		fprintf(m_pLogFile, "\n[%s][%s:%d][%s]:%s", timestr,
			szFileName, nLineNumber, typestr, szMsg);
	}
//#ifdef _DEBUG
	fflush(m_pLogFile);
//#endif
	m_bPrintPrefix = strstr(szMsg, "\n") != NULL;
}
void BCLogFile::LogInternal(int32_t nLevel, LPCSTR szMsg)
{
	if (m_nLevel < 0 || nLevel > m_nLevel)
	{
		return;
	}
	fseek(m_pLogFile, 0, SEEK_END);
	if (m_bPrintPrefix)
	{
		fprintf(m_pLogFile, "%s", szMsg);
	}
	else
	{
		fprintf(m_pLogFile, "\n%s", szMsg);
	}
//#ifdef _DEBUG
	fflush(m_pLogFile);
//#endif
	m_bPrintPrefix = strstr(szMsg, "\n") != NULL;
}
#endif // __ANDROID__

#define CHECKCHAR(x) (((x)>=33&&(x)<=126)?(x):'`')

void BCLogFile::LogBinaryInternal(
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	const void *pData,
	uint32_t dataLen)
{
	char typestr[50] = {0};
	char timestr[50] = {0};
	char hexstr[3] = {0};
	static char hextab[16] =
	{'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
	unsigned char *p = (unsigned char *)pData;
	uint32_t i;

	UNUSED(szFuncName);

	if (pData == NULL || dataLen == 0)
	{
		return;
	}
	snprintf(typestr, 50, "Binary Data:%d bytes.", dataLen);
	fseek(m_pLogFile, 0, SEEK_END);
	GetTime(timestr, 50);
	if (m_bPrintPrefix)
	{
		fprintf(m_pLogFile, "[%s][%s:%d][%s]:\n", timestr,
			szFileName, nLineNumber, typestr);
	}
	else
	{
		fprintf(m_pLogFile, "\n[%s]:[%s:%d]:[%s]:\n", timestr,
			szFileName, nLineNumber, typestr);
	}
	for (i = 0;i < dataLen;i++, p++)
	{
		hexstr[0] = hextab[(*p)>>4];
		hexstr[1] = hextab[(*p)&0xf];
		fprintf(m_pLogFile, "%s ", hexstr);
		if ((i+1)%8 == 0)
		{
			if ((i+1)%16 == 0)
			{
				fprintf(m_pLogFile, "        |%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c|",
					CHECKCHAR(*(p-15)), CHECKCHAR(*(p-14)), CHECKCHAR(*(p-13)),
					CHECKCHAR(*(p-12)), CHECKCHAR(*(p-11)), CHECKCHAR(*(p-10)),
					CHECKCHAR(*(p-9)), CHECKCHAR(*(p-8)), CHECKCHAR(*(p-7)),
					CHECKCHAR(*(p-6)), CHECKCHAR(*(p-5)), CHECKCHAR(*(p-4)),
					CHECKCHAR(*(p-3)), CHECKCHAR(*(p-2)), CHECKCHAR(*(p-1)),
					CHECKCHAR(*p));
				if (i+1 != dataLen)
				{
					fprintf(m_pLogFile, "\n");
				}
			}
			else
			{
				fprintf(m_pLogFile, "  ");
			}
		}
	}
	i = dataLen%16;
	// printf printable character
	if(i > 0)
	{
		p -= i;
		for (uint32_t j = 0;j < 16-i;j++)
		{
			fprintf(m_pLogFile, "   ");
		}
		if (i < 7)
		{
			fprintf(m_pLogFile, "  ");
		}
		fprintf(m_pLogFile, "        |");
		for (uint32_t j = 0;j < i;j++,p++)
		{
			fprintf(m_pLogFile, "%c", CHECKCHAR(*p));
		}
		fprintf(m_pLogFile, "|\n");
	}
	fflush(m_pLogFile);
	m_bPrintPrefix = false;
}

bool BCLogFile::OnEventProcess(BCEventItemS& refEvent)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (CloseCheck())
	{
		return false;
	}

	switch (EVENTMAJOR(refEvent.eType))
	{
	case LOGF_MSG_LOG:
		LogInternal(refEvent.wParam, (LPCSTR)refEvent.vParams[0], 
			refEvent.lParam, (LPCSTR)refEvent.vParams[1], 
			(LPCSTR)refEvent.vParams[2]);
		break;
	case LOGF_MSG_LOG_CUSTOM:
		LogInternal(refEvent.wParam, (LPCSTR)refEvent.lParam);
		break;
	case LOGF_MSG_BIN:
		LogBinaryInternal((LPCSTR)refEvent.vParams[0], refEvent.wParam, 
			(LPCSTR)refEvent.vParams[1], (LPCSTR)refEvent.vParams[2], 
			refEvent.lParam);
		break;
	case LOGF_MSG_CLOSE:
		_set_state(this, LOGF_STATE_FREED, BC_R_SUCCESS);
		break;
	default:
		break;
	}
	CloseCheck();
	return true;
}

void BCLogFile::OnEventProcShutdown()
{
	BCMutex::Owner lock(m_sExitLock);
	m_sExitCond.Signal();
}

bool BCLogFile::CloseCheck()
{
	if (m_eState <= m_eNewState && m_eState > LOGF_STATE_FREED)
	{
		return false;
	}

	if (m_eState == LOGF_STATE_WORKING)
	{
		if (m_pLogFile)
		{
			fclose(m_pLogFile);
			m_pLogFile = NULL;
		}
		Detach();
		m_eState = LOGF_STATE_FREED;
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCExternalLog
///////////////////////////////////////////////////////////////////////////////

BCLogExternal::BCLogExternal(
	LPFN_BCLogExternalPtr lpfnCallback, 
	void *data,
	bool bExclusive)
	: BCLogBase(bExclusive)
	, m_lpfnCallback(lpfnCallback)
	, m_pData(data)
{
	//
}

BCLogExternal::~BCLogExternal()
{
	//
}

void BCLogExternal::Log(
	int32_t nLevel,
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	LPCSTR szMsg)
{
	char typestr[50] = {0};
	char timestr[50] = {0};
	size_t nSafeSize;
	KBPool pool;
	LPSTR szBuf;

	UNUSED(szFuncName);

	m_sLock.Lock();
	if (m_nLevel < 0 || nLevel > m_nLevel)
	{
		goto end;
	}
	switch(nLevel)
	{
	case _FATAL_:
		snprintf(typestr, 50, "fatal error!");
		break;
	case _ERROR_:
		snprintf(typestr, 50, "error!");
		break;
	case _WARN_:
		snprintf(typestr, 50, "warning!");
		break;
	case _INFO_:
		snprintf(typestr, 50, "information.");
		break;
	case _DEBUG_:
		snprintf(typestr, 50, "debug.");
		break;
	case _FINE_:
		snprintf(typestr, 50, "fine.");
		break;
	case _FINEST_:
		snprintf(typestr, 50, "finest.");
		break;
	default:
		goto end;
	}
	GetTime(timestr, 50);
	nSafeSize = strlen(timestr) + strlen(szFileName) + strlen(szMsg) + 100;
	szBuf = (LPSTR)pool.Calloc(nSafeSize);
	snprintf(szBuf, nSafeSize, "[%s][%s:%d][%s]:%s", timestr,
		szFileName, nLineNumber, typestr, szMsg);
	if (m_lpfnCallback)
	{
		(*m_lpfnCallback)(m_pData, nLevel, szBuf);
	}
end:
	m_sLock.Unlock();
}

void BCLogExternal::Log(int32_t nLevel, LPCSTR szMsg)
{
	m_sLock.Lock();
	if (m_nLevel < 0 || nLevel > m_nLevel)
	{
		goto end;
	}
	if (m_lpfnCallback)
	{
		(*m_lpfnCallback)(m_pData, nLevel, szMsg);
	}
end:
	m_sLock.Unlock();
}

#define CHECKCHAR(x) (((x)>=33&&(x)<=126)?(x):'`')

void BCLogExternal::LogBinary(
	LPCSTR szFileName,
	uint32_t nLineNumber,
	LPCSTR szFuncName,
	const void *pData,
	uint32_t dataLen)
{
	char typestr[50] = {0};
	char timestr[50] = {0};
	char hexstr[3] = {0};
	static char hextab[16] =
	{'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
	unsigned char *p = (unsigned char *)pData;
	uint32_t i, offset;
	size_t nSafeSize, nHexSize;
	KBPool pool;
	LPSTR szBuf;

	UNUSED(szFuncName);

	if (pData == NULL || dataLen == 0)
	{
		return;
	}
	m_sLock.Lock();
	snprintf(typestr, 50, "Binary Data:%d bytes.", dataLen);
	GetTime(timestr, 50);
	nHexSize = (dataLen + 16 - 1) / 16 * (32 + 30);
	nSafeSize = strlen(timestr) + strlen(szFileName) + nHexSize + 100;
	offset = 0;
	szBuf = (LPSTR)pool.Calloc(nSafeSize);
	offset += snprintf(szBuf, nSafeSize - offset, "[%s][%s:%d][%s]:\n", 
		timestr, szFileName, nLineNumber, typestr);
	for (i = 0;i < dataLen;i++, p++)
	{
		hexstr[0] = hextab[(*p)>>4];
		hexstr[1] = hextab[(*p)&0xf];
		offset += snprintf(szBuf + offset, nSafeSize - offset, "%s ", hexstr);
		if ((i+1)%8 == 0)
		{
			if ((i+1)%16 == 0)
			{
				offset += snprintf(szBuf + offset, nSafeSize - offset, 
					"        |%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c|",
					CHECKCHAR(*(p-15)), CHECKCHAR(*(p-14)), CHECKCHAR(*(p-13)),
					CHECKCHAR(*(p-12)), CHECKCHAR(*(p-11)), CHECKCHAR(*(p-10)),
					CHECKCHAR(*(p-9)), CHECKCHAR(*(p-8)), CHECKCHAR(*(p-7)),
					CHECKCHAR(*(p-6)), CHECKCHAR(*(p-5)), CHECKCHAR(*(p-4)),
					CHECKCHAR(*(p-3)), CHECKCHAR(*(p-2)), CHECKCHAR(*(p-1)),
					CHECKCHAR(*p));
				if (i+1 != dataLen)
				{
					offset += snprintf(szBuf + offset, nSafeSize - offset, "\n");
				}
			}
			else
			{
				offset += snprintf(szBuf + offset, nSafeSize - offset, "  ");
			}
		}
	}
	i = dataLen%16;
	// printf printable character
	if(i > 0)
	{
		p -= i;
		for (uint32_t j = 0;j < 16-i;j++)
		{
			offset += snprintf(szBuf + offset, nSafeSize - offset, "   ");
		}
		if (i < 7)
		{
			offset += snprintf(szBuf + offset, nSafeSize - offset, "  ");
		}
		offset += snprintf(szBuf + offset, nSafeSize - offset, "        |");
		for (uint32_t j = 0;j < i;j++,p++)
		{
			offset += snprintf(szBuf + offset, nSafeSize - offset, "%c", CHECKCHAR(*p));
		}
		offset += snprintf(szBuf + offset, nSafeSize - offset, "|\n");
	}
	if (m_lpfnCallback)
	{
		(*m_lpfnCallback)(m_pData, _FINEST_, szBuf);
	}
	m_sLock.Unlock();
}

void BCLogExternal::Close()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

void *
AddFileLogAppender(
	const char *szFileName, 
	int nLevel, 
	bool bCWD,
	bool bExclusive)
{
	BCLogger *pLogger = BCLogger::GetInstance();
	BCLogFile* pLogLocation = new BCLogFile(bExclusive);
	if (!pLogLocation)
	{
		return NULL;
	}
	BCRESULT result = pLogLocation->Create(szFileName, bCWD);
	if (result != BC_R_SUCCESS)
	{
		return NULL;
	}
	pLogLocation->SetLevel(nLevel);
	pLogger->AddLogLocation(pLogLocation);
	return pLogLocation;
}

BC_API 
void *
AddExternalLogAppender(
	LPFN_BCLogExternalPtr lpfnCallback, 
	void *data,
	int nLevel,
	bool bExclusive)
{
	BCLogger *pLogger = BCLogger::GetInstance();
	BCLogBase *pLogLocation = new BCLogExternal(lpfnCallback, data, bExclusive);
	pLogLocation->SetLevel(nLevel);
	pLogger->AddLogLocation(pLogLocation);
	return pLogLocation;
}

BC_API
bool
RemoveLogAppender(void *&pLogLocation)
{
	BCLogger *pLogger = BCLogger::GetInstance();
	pLogger->RemoveLogLocation((BCLogBase *&)pLogLocation);
	BC_SAFE_DELETE_PTR((BCLogBase *&)pLogLocation);
	return true;
}

BC_API
void
RemoveAllLogAppenders()
{
	BCLogger *pLogger = BCLogger::GetInstance();
	pLogger->RemoveAllLogLocations();
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
