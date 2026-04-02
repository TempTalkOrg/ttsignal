///////////////////////////////////////////////////////////////////////////////
// file : StringUtils.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_STRINGUTILS_H_INCLUDED__
#define HTTP_STRINGUTILS_H_INCLUDED__

#include <HTTP/Exports.h>
#include <BC/BCStrPtrLen.h>
#include <BC/BCMemPool.h>

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

typedef int32_t OS_Error;

enum
{
	OS_NoErr = (OS_Error) 0,
	OS_BadURLFormat = (OS_Error) -100,
	OS_NotEnoughSpace = (OS_Error) -101
};

/**********************************/
// Error Codes

enum
{
	ERR_NoErr              = 0,
	ERR_RequestFailed      = -1,
	ERR_Unimplemented      = -2,
	ERR_RequestArrived     = -3,
	ERR_OutOfState         = -4,
	ERR_NotAModule         = -5,
	ERR_WrongVersion       = -6,
	ERR_IllegalService     = -7,
	ERR_BadIndex           = -8,
	ERR_ValueNotFound      = -9,
	ERR_BadArgument        = -10,
	ERR_ReadOnly           = -11,
	ERR_NotPreemptiveSafe  = -12,
	ERR_NotEnoughSpace     = -13,
	ERR_WouldBlock         = -14,
	ERR_NotConnected       = -15,
	ERR_FileNotFound       = -16,
	ERR_NoMoreData         = -17,
	ERR_AttrDoesntExist    = -18,
	ERR_AttrNameExists     = -19,
	ERR_InstanceAttrsNotAllowed= -20
};
typedef int32_t PARSE_ERROR;


#define STRINGPARSERTESTING 		0
#define STRINGTRANSLATORTESTING 	0



///////////////////////////////////////////////////////////////////////////////
// Class : StringFormatter
//       - Utility class for formatting text to a buffer.
//         Construct object with a buffer, then call one
//         of many Put methods to write into that buffer.
///////////////////////////////////////////////////////////////////////////////

//Use a class like the ResizeableStringFormatter if you want a buffer that will dynamically grow
class HTTP_API StringFormatter
{
public:

    //pass in a buffer and length for writing
    StringFormatter(char *buffer, uint32_t length)
		: fCurrentPut(buffer)
		, fStartPut(buffer)
		, fEndPut(buffer + length)
		, fBytesWritten(0) {}

	StringFormatter(BCStrPtrLen &buffer)
		: fCurrentPut(buffer.Ptr)
		, fStartPut(buffer.Ptr)
		, fEndPut(buffer.Ptr + buffer.Len)
		, fBytesWritten(0) {}
    virtual ~StringFormatter() {}

    void		Set(char *buffer, uint32_t length)
	{
		fCurrentPut = buffer;
        fStartPut = buffer;
        fEndPut = buffer + length;
        fBytesWritten= 0;
    }

    //"erases" all data in the output stream save this number
    void        Reset(uint32_t inNumBytesToLeave = 0)
	{
		fCurrentPut = fStartPut + inNumBytesToLeave;
	}

    //Object does no bounds checking on the buffer. That is your responsibility!
    //Put truncates to the buffer size
    void        Put(const int32_t num);
    void        Put(const char* buffer, uint32_t bufferSize);
    void        Put(const char* str)		{ Put(str, strlen(str)); }
    void        Put(const BCStrPtrLen &str) { Put(str.Ptr, str.Len); }
    void        PutSpace()        			{ PutChar(' '); }
    void        PutEOL()          			{ Put(sEOL, sEOLLen); }
    void        PutChar(char c)   			{ Put(&c, 1); }
    void        PutTerminator()   			{ PutChar('\0'); }

	//Writes a printf style formatted string
	bool		PutFmtStr(const char *fmt,  ...);


	//the number of characters in the buffer
    inline uint32_t		GetCurrentOffset();
    inline uint32_t		GetSpaceLeft();
    inline uint32_t		GetTotalBufferSize();
    char*               GetCurrentPtr()     { return fCurrentPut; }
    char*               GetBufPtr()         { return fStartPut; }

    // Counts total bytes that have been written to this buffer (increments
    // even when the buffer gets reset)
    void                ResetBytesWritten() { fBytesWritten = 0; }
    uint32_t			GetBytesWritten()   { return fBytesWritten; }

    inline void         PutFilePath(BCStrPtrLen *inPath, BCStrPtrLen *inFileName);
    inline void         PutFilePath(char *inPath, char *inFileName);

	//Return a NEW'd copy of the buffer as a C string
	char			*	GetAsCString()
	{
		BCStrPtrLen str(fStartPut, this->GetCurrentOffset());
		return str.GetAsCString();
	}

protected:

    //If you fill up the StringFormatter buffer, this function will get called. By
    //default, the function simply returns false.  But derived objects can clear out the data,
	//reset the buffer, and then returns true.
    //Use the ResizeableStringFormatter if you want a buffer that will dynamically grow.
	//Returns true if the buffer has been resized.
    virtual bool    BufferIsFull(char* /*inBuffer*/, uint32_t /*inBufferLen*/) { return false; }

    char			*	fCurrentPut;
    char			*	fStartPut;
    char			*	fEndPut;

    // A way of keeping count of how many bytes have been written total
    uint32_t			fBytesWritten;

    static const char *	sEOL;
    static uint32_t	    sEOLLen;
};

inline uint32_t StringFormatter::GetCurrentOffset()
{
    ASSERT(fCurrentPut >= fStartPut);
    return (uint32_t)(fCurrentPut - fStartPut);
}

inline uint32_t StringFormatter::GetSpaceLeft()
{
    ASSERT(fEndPut >= fCurrentPut);
    return (uint32_t)(fEndPut - fCurrentPut);
}

inline uint32_t StringFormatter::GetTotalBufferSize()
{
    ASSERT(fEndPut >= fStartPut);
    return (uint32_t)(fEndPut - fStartPut);
}

inline void StringFormatter::PutFilePath(BCStrPtrLen *inPath, BCStrPtrLen *inFileName)
{
	if (inPath != NULL && inPath->Len > 0)
    {
        Put(inPath->Ptr, inPath->Len);
        if (kPathDelimiterChar != inPath->Ptr[inPath->Len -1] )
            Put(kPathDelimiterString);
    }
    if (inFileName != NULL && inFileName->Len > 0)
        Put(inFileName->Ptr, inFileName->Len);
}

inline void StringFormatter::PutFilePath(char *inPath, char *inFileName)
{
	BCStrPtrLen pathStr(inPath);
	BCStrPtrLen fileStr(inFileName);

	PutFilePath(&pathStr,&fileStr);
}

///////////////////////////////////////////////////////////////////////////////
// Class : ResizableStringFormatter
///////////////////////////////////////////////////////////////////////////////

class HTTP_API ResizeableStringFormatter : public StringFormatter
{
public:
	// Pass in inBuffer=NULL and inBufSize=0 to dynamically allocate the initial buffer.
	ResizeableStringFormatter(char* inBuffer = NULL, uint32_t inBufSize = 0)
		: StringFormatter(inBuffer, inBufSize){}

	// If we've been forced to increase the buffer size,
	// fStartPut WILL be a dynamically allocated buffer,
	// and it WON'T be equal to fOriginalBuffer (obviously).
	virtual ~ResizeableStringFormatter() {}

	void		Reset(char* inBuffer = NULL, uint32_t inBufSize = 0);
	void		Reset(BCStrPtrLen &refBuffer);
private:

	// This function will get called by StringFormatter if the current
	// output buffer is full. This object allocates a buffer that's twice
	// as big as the old one.
	virtual bool    BufferIsFull(char* inBuffer, uint32_t inBufferLen);

	KBPool			m_sPool;
};

///////////////////////////////////////////////////////////////////////////////
// Class : StringParser
//       - A couple of handy utilities for parsing a stream.
///////////////////////////////////////////////////////////////////////////////

class HTTP_API StringParser
{
public:

    StringParser(BCStrPtrLen *inStream)
        : fStartGet(inStream == NULL ? NULL : inStream->Ptr)
		, fEndGet(inStream == NULL ? NULL : inStream->Ptr + inStream->Len)
		, fCurLineNumber(1)
		, fStream(inStream) {}
    ~StringParser() {}

    // Built-in masks for common stop conditions
    static uint8_t sDigitMask[];      // stop when you hit a digit
    static uint8_t sWordMask[];       // stop when you hit a word
    static uint8_t sEOLMask[];        // stop when you hit an eol
    static uint8_t sEOLWhitespaceMask[]; // stop when you hit an EOL or whitespace
    static uint8_t sEOLWhitespaceQueryMask[]; // stop when you hit an EOL, ? or whitespace

    static uint8_t sWhitespaceMask[]; // skip over whitespace


    //GetBuffer:
    //Returns a pointer to the string object
    BCStrPtrLen		*	GetStream() { return fStream; }

    //Expect:
    //These functions consume the given token/word if it is in the stream.
    //If not, they return false.
    //In all other situations, true is returned.
    //NOTE: if these functions return an error, the object goes into a state where
    //it cannot be guarenteed to function correctly.
    bool				Expect(char stopChar);
    bool				ExpectEOL();

    //Returns the next word
    void				ConsumeWord(BCStrPtrLen* outString = NULL)
                        { ConsumeUntil(outString, sNonWordMask); }

    //Returns all the data before inStopChar
    void				ConsumeUntil(BCStrPtrLen* outString, char inStopChar);

    //Returns whatever integer is currently in the stream
    uint32_t			ConsumeInteger(BCStrPtrLen* outString = NULL);
    float32_t			ConsumeFloat();
    float32_t			ConsumeNPT();

    //Keeps on going until non-whitespace
    void				ConsumeWhitespace()
                        { ConsumeUntil(NULL, sWhitespaceMask); }

    //Assumes 'stop' is a 255-char array of booleans. Set this array
    //to a mask of what the stop characters are. true means stop character.
    //You may also pass in one of the many prepackaged masks defined above.
    void				ConsumeUntil(BCStrPtrLen* spl, uint8_t *stop);


    //+ rt 8.19.99
    //returns whatever is avaliable until non-whitespace
    void				ConsumeUntilWhitespace(BCStrPtrLen* spl = NULL)
                        { ConsumeUntil( spl, sEOLWhitespaceMask); }

    void				ConsumeUntilDigit(BCStrPtrLen* spl = NULL)
                        { ConsumeUntil( spl, sDigitMask); }

	void				ConsumeLength(BCStrPtrLen* spl, int32_t numBytes);

	void				ConsumeEOL(BCStrPtrLen* outString);

    //GetThru:
    //Works very similar to ConsumeUntil except that it moves past the stop token,
    //and if it can't find the stop token it returns false
    inline bool			GetThru(BCStrPtrLen* spl, char stop);
    inline bool			GetThruEOL(BCStrPtrLen* spl);
    inline bool			ParserIsEmpty(BCStrPtrLen* outString);
    //Returns the current character, doesn't move past it.
    inline char			PeekFast() { if (fStartGet) return *fStartGet; else return '\0'; }
    char				operator[](int i) { ASSERT((fStartGet+i) < fEndGet);return fStartGet[i]; }

    //Returns some info about the stream
    uint32_t			GetDataParsedLen()
    {
		ASSERT(fStartGet >= fStream->Ptr);
		return (uint32_t)(fStartGet - fStream->Ptr);
	}
    uint32_t			GetDataReceivedLen()
    {
		ASSERT(fEndGet >= fStream->Ptr);
		return (uint32_t)(fEndGet - fStream->Ptr);
	}
    uint32_t			GetDataRemaining()
    {
		ASSERT(fEndGet >= fStartGet);
		return (uint32_t)(fEndGet - fStartGet);
	}
    char			*	GetCurrentPosition() { return fStartGet; }
    int					GetCurrentLineNumber() { return fCurLineNumber; }

    // A utility for extracting quotes from the start and end of a parsed
    // string. (Warning: Do not call this method if you allocated your own
    // pointer for the Ptr field of the BCStrPtrLen class.) - [sfu]
    //
    // Not sure why this utility is here and not in the BCStrPtrLen class - [jm]
    static void			UnQuote(BCStrPtrLen* outString);


#if STRINGPARSERTESTING
    static bool			Test();
#endif

private:

    void				AdvanceMark();

    //built in masks for some common stop conditions
    static uint8_t sNonWordMask[];

    char			*	fStartGet;
    char			*	fEndGet;
    int					fCurLineNumber;
    BCStrPtrLen		*	fStream;

};

bool StringParser::GetThru(BCStrPtrLen* outString, char inStopChar)
{
    ConsumeUntil(outString, inStopChar);
    return Expect(inStopChar);
}

bool StringParser::GetThruEOL(BCStrPtrLen* outString)
{
    ConsumeUntil(outString, sEOLMask);
    return ExpectEOL();
}

bool StringParser::ParserIsEmpty(BCStrPtrLen* outString)
{
    if (NULL == fStartGet || NULL == fEndGet)
    {
        if (NULL != outString)
        {   outString->Ptr = NULL;
            outString->Len = 0;
        }

        return true;
    }

    ASSERT(fStartGet <= fEndGet);

    return false; // parser ok to parse
}

///////////////////////////////////////////////////////////////////////////////
// Class : StringTranslator
//       - Static utilities for translating strings from one encoding scheme to
//         another. For example, routines for encoding and decoding URLs
///////////////////////////////////////////////////////////////////////////////


class HTTP_API StringTranslator
{
public:

	//DecodeURL:
	//
	// This function does 2 things: Decodes % encoded characters in URLs, and strips out
	// any ".." or "." complete filenames from the URL. Writes the result into ioDest.
	//
	//If successful, returns the length of the destination string.
	//If failure, returns an OS errorcode: OS_BadURLFormat, OS_NotEnoughSpace

	static int32_t   DecodeURL(const char* inSrc, int32_t inSrcLen, char* ioDest, int32_t inDestLen);

	//EncodeURL:
	//
	// This function takes a character string and % encodes any special URL characters.
	// In general, the output buffer will be longer than the input buffer, so caller should
	// be aware of that.
	//
	//If successful, returns the length of the destination string.
	//If failure, returns an QTSS errorcode: OS_NotEnoughSpace
	//
	// If function returns E2BIG, ioDest will be valid, but will contain
	// only the portion of the URL that fit.
	static int32_t   EncodeURL(const char* inSrc, int32_t inSrcLen, char* ioDest, int32_t inDestLen);

	// DecodePath:
	//
	// This function converts "network" or "URL" path delimiters (the '/' char) to
	// the path delimiter of the local file system. It does this conversion in place,
	// so the old data will be overwritten
	static void     DecodePath(char* inSrc, uint32_t inSrcLen);

#if STRINGTRANSLATORTESTING
    static bool		Test();
#endif
};



extern int64_t  G_sMsecSince1900;
extern int64_t  G_sMsecSince1970;
extern int64_t  G_sInitialMsec;
extern int64_t  G_sWrapTime;
extern int64_t  G_sCompareWrap;
extern int64_t  G_sLastTimeMilli;

class DateBuffer;

///////////////////////////////////////////////////////////////////////////////
// Class : DateTranslator
//       - Efficient routines & data structures for converting from
//         RFC 1123 compliant date strings to local file system dates & vice versa.
///////////////////////////////////////////////////////////////////////////////


class HTTP_API DateTranslator
{
public:

    // this updates the DateBuffer to be the current date / time.
    // If you wish to set the DateBuffer to a particular date, pass in that date.
    // Dates should be in the OS.h compliant format
    static void			UpdateDateBuffer(
							DateBuffer* inDateBuffer,
							const int64_t& inDate,
							time_t gmtoffset = 0);

    //Given an HTTP/1.1 compliant date string (in one of the three 1.1 formats)
    //this returns an OS.h compliant date/time value.
    static int64_t		ParseDate(BCStrPtrLen* inDateString);

private:

    static uint32_t		ConvertCharToMonthTableIndex(int inChar)
    {
        return (uint32_t)(toupper(inChar) - 'A'); // Convert to a value between 0 - 25
    }
};

class HTTP_API DateBuffer
{
public:

    // This class provides no protection against being accessed from multiple threads
    // simultaneously. Update & InexactUpdate rewrite the date buffer, so care should
    // be taken to protect against simultaneous access.

    DateBuffer() : fLastDateUpdate(0) { fDateBuffer[0] = 0; }
    ~DateBuffer() {}

    //SEE RFC 1123 for details on the date string format
    //ex: Mon, 04 Nov 1996 21:42:17 GMT

    //RFC 1123 date strings are always of this length
    enum
    {
        kDateBufferLen = 29
    };

    // Updates this date buffer to reflect the current time.
    // If a date is provided, this updates the DateBuffer to be that date.
    void		Update(const int64_t& inDate){ DateTranslator::UpdateDateBuffer(this, inDate); }

    // Updates this date buffer to reflect the current time, with a certain degree
    // of inexactitude (the range of error is defined by the kUpdateInterval value)
    void		InexactUpdate();

    //returns a NULL terminated C-string always of kHTTPDateLen length.
    char	*	GetDateBuffer()   { return fDateBuffer; }

private:

    enum
    {
        kUpdateInterval = 60000 // Update every minute
    };

    //+1 for terminator +1 for padding
    char		fDateBuffer[kDateBufferLen + 2];
    int64_t		fLastDateUpdate;

    friend class DateTranslator;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

#endif // HTTP_STRINGUTILS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : StringUtils.h
///////////////////////////////////////////////////////////////////////////////
