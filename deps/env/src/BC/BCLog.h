
#ifndef BC_BCLOG_H_INCLUDED__
#define BC_BCLOG_H_INCLUDED__

#include <list>

#include "BC/Exports.h"
#include "BC/BCPString.h"
#include "BC/BCThread.h"
#include "BC/BCEventQueue.h"

namespace BC
{
	class BCBuffer;
}


///////////////////////////////////////////////////////////////////////////////
// Functions defined in this file...
///////////////////////////////////////////////////////////////////////////////

#define _FATAL_		0
#define _ERROR_		1
#define _WARNING_	2
#define _INFO_		3
#define _DEBUG_		4
#define _FINE_		5
#define _FINEST_	6

#define _LOCAL_		__FILE__, __LINE__, __FUNCTION__

BC_API void
LogFatal(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogError(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogWarn(const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogInfo(const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogDebug(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogFine(const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogFinest(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogAssert(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogFatal(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogError(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogWarn(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogInfo(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogDebug(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogFine(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
        const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogFinest(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogAssert(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, ...);
BC_API void
LogCustom(const void* logger_ctx, const char * szFmtStr, ...);
BC_API void
LogCustomWithTime(const void* logger_ctx, const char * szFmtStr, ...);
BC_API void
LogBin(const char * szFileName, uint32_t nLineNO,
       const char * szFuncName, const void *pData, uint32_t dataLen);
BC_API void
LogBin(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
       const char * szFuncName, const void *pData, uint32_t dataLen);
BC_API void
LogBuffer(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, BC::BCBuffer *pBuffer);
BC_API void
LogBuffer(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, BC::BCBuffer *pBuffer);

BC_API void
LogFatalV(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogErrorV(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogWarnV(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogInfoV(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogDebugV(const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogFineV(const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogFinestV(const char * szFileName, uint32_t nLineNO,
           const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogAssertV(const char * szFileName, uint32_t nLineNO,
           const char * szFuncName, const char * szFmtStr, va_list args);

BC_API void
LogFatalV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogErrorV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogWarnV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogInfoV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogDebugV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
          const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogFineV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
         const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogFinestV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
           const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogAssertV(const void* logger_ctx, const char * szFileName, uint32_t nLineNO,
           const char * szFuncName, const char * szFmtStr, va_list args);
BC_API void
LogCustomV(const void *logger_ctx, const char* szFmtStr, va_list args);

#define NYI LogWarn(_LOCAL_, "%s not yet implemented",__func__);
#define NYIR NYI;return false;

BC_API void *
AddFileLogAppender(
	const char * filename, 
	int level, 
	bool bCWD = false,
	bool bExclusive = false);

typedef void(*LPFN_BCLogExternal)(void *data, int level, LPCSTR lpszMsg);
typedef LPFN_BCLogExternal		LPFN_BCLogExternalPtr;

BC_API void *
AddExternalLogAppender(
	LPFN_BCLogExternalPtr lpfnCallback, 
	void *data, 
	int nLevel,
	bool bExclusive = false);

BC_API bool
RemoveLogAppender(void *&pLogBase);

BC_API void
RemoveAllLogAppenders();

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

class BCLogBase;

///////////////////////////////////////////////////////////////////////////////
// class : BCLogger
///////////////////////////////////////////////////////////////////////////////

class BCLogger
{
public:
	static BCLogger * GetInstance();
	virtual ~BCLogger();

	void				Log(
							const void *logger_ctx, 
							int32_t level,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szFMT,
							...);
	void				Log(
							const void *logger_ctx, 
							int32_t level,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szFMT,
							va_list args);
	void				Log(
							const void *logger_ctx, 
							LPCSTR szFMT, 
							va_list args);
	void				LogBinary(
							const void *logger_ctx, 
							LPCSTR szFileName,
							uint32_t lineNumber,
							LPCSTR szFuncName,
							const void *pData,
							uint32_t dataLen);
	void				LogBuffer(
							const void* logger_ctx, 
							const char * szFileName, 
							uint32_t nLineNO,
							const char * szFuncName, 
							BCBuffer *pBuffer);
	void				AddLogLocation(BCLogBase *pLogLocation);
	void				RemoveLogLocation(BCLogBase *pLocation);
	void				RemoveAllLogLocations();
private:
	BCLogger();
private:
	DECLARE_NO_COPY_CLASS(BCLogger);
	static BCLogger	*	s_pLogger;
	BCSpinMutex			s_lock;
	typedef std::list<BCLogBase *>	BCLogBaseList;
	BCLogBaseList		s_logLocations;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCLogBase
///////////////////////////////////////////////////////////////////////////////

class BCLogBase
{
	friend class BCLogger;
public:
	BCLogBase(bool bExclusive = FALSE);
	virtual ~BCLogBase();

	int32_t				GetLevel();
	void				SetLevel(int32_t level);

	LPCSTR				GetName();
	void				SetName(LPCSTR name);

	virtual void		Log(
							int32_t nLevel,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szMsg)			= 0;
	virtual void		Log(LPCSTR szMsg)			= 0;
	virtual void		LogBinary(
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							const void *pData,
							uint32_t dataLen)
							{
								UNUSED(szFileName);
								UNUSED(nLineNumber);
								UNUSED(szFuncName);
								UNUSED(pData);
								UNUSED(dataLen);
							};
	virtual void		Close()						= 0;
	static uint32_t		GetTime(char *buf, uint32_t bufsize);
protected:
	int32_t				m_nLevel;
	BCPString			m_strName;
	bool				m_bExclusive;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCLogFile
///////////////////////////////////////////////////////////////////////////////

class BCLogFile 
	: public BCLogBase
	, public BCEventFactory
{
public:
	BCLogFile(bool bExclusive = false);
	virtual ~BCLogFile();

	BCRESULT			Create(
							const char *szLogFileName = NULL, 
							bool bCWD = false);
	virtual void		Log(
							int32_t nLevel,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szMsg);
	virtual void		Log(LPCSTR szMsg);
	virtual void		LogBinary(
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							const void *pData,
							uint32_t dataLen);
	void				Close();
protected:
	inline void		_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_eNewState = eState;
		m_nStateLineNo = nLineNumber;
	}
	void				GetDefLogFilePath(
							BCPString& location, 
							const char *filename,
							bool bCWD = false);
	void				LogInternal(
							int32_t nLevel,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szMsg);
	void				LogInternal(LPCSTR szMsg);
	void				LogBinaryInternal(
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							const void *pData,
							uint32_t dataLen);

	virtual bool		OnEventProcess(BCEventItemS& refEvent);
	virtual void		OnEventProcShutdown();
	bool				CloseCheck();
private:
	DECLARE_NO_COPY_CLASS(BCLogFile);
	BCSpinMutex			m_sLock;
	BCPString			m_strLogFileName;
	FILE			*	m_pLogFile;
	BCTaskMgr		*	m_pTaskMgr;
	uint32_t			m_eState;
	uint32_t			m_eNewState;
	uint32_t			m_nStateLineNo;
	uint32_t			m_eCloseStatus;
	BCMutex				m_sExitLock;
	BCCondition			m_sExitCond;
	bool				m_bPrintPrefix;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCLogExternal
///////////////////////////////////////////////////////////////////////////////

class BCLogExternal : public BCLogBase
{
public:
	BCLogExternal(
		LPFN_BCLogExternalPtr lpfnCallback, 
		void *data, 
		bool bExclusive = FALSE);
	virtual ~BCLogExternal();

	virtual void		Log(
							int32_t nLevel,
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							LPCSTR szMsg);
	virtual void		Log(LPCSTR szMsg);
	virtual void		LogBinary(
							LPCSTR szFileName,
							uint32_t nLineNumber,
							LPCSTR szFuncName,
							const void *pData,
							uint32_t dataLen);
	virtual void		Close();

private:
	DECLARE_NO_COPY_CLASS(BCLogExternal);
	BCSpinMutex					m_sLock;
	LPFN_BCLogExternalPtr		m_lpfnCallback;
	void					*	m_pData;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BC_BCLOG_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
