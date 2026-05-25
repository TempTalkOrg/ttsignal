///////////////////////////////////////////////////////////////////////////////
// file : HTTPRequest.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include "HTTPProtocol.h"
#include "StringUtils.h"
#include "HTTPRequest.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// Class : HTTPRequest
///////////////////////////////////////////////////////////////////////////////

static bool			sFalse = false;
static bool			sTrue = true;
static BCStrPtrLen	sCloseString((char *)"close", 5);
static BCStrPtrLen	sKeepAliveString((char *)"keep-alive", 10);
static BCSpinMutex	G_sDateLock;
static DateBuffer	G_sDataBuffer;

BCStrPtrLen HTTPRequest::sColonSpace((char *)": ", 2);

uint8_t HTTPRequest::sURLStopConditions[] =
{
  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, //0-9      //'\t' is a stop condition
  1, 0, 0, 1, 0, 0, 0, 0, 0, 0, //10-19    //'\r' & '\n' are stop conditions
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, //30-39    //' '
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //40-49
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //50-59
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //60-69
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //90-99
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //120-129
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
  0, 0, 0, 0, 0, 0                         //250-255
};


// Constructor
HTTPRequest::HTTPRequest(BCStrPtrLen* serverHeader, BCStrPtrLen* requestPtr)
{
    // Store the pointer to the server header field
    fSvrHeader = *serverHeader;

    // Set initial state
    fRequestHeader = *requestPtr;
    fMethod = httpIllegalMethod;
    fVersion = httpIllegalVersion;
    fAbsoluteURI = NULL;
    fRelativeURI = NULL;
    fAbsoluteURIScheme = NULL;
    fHostHeader = NULL;
    fRequestPath = NULL;
    fStatusCode = httpOK;
    fRequestKeepAlive = false; // Default value when there is no version string
}

// Constructor for creating a response only
HTTPRequest::HTTPRequest(BCStrPtrLen* serverHeader)
{
    // Store the pointer to the server header field
    fSvrHeader = *serverHeader;

    // We do not require any of these:
    fRequestHeader = NULL;

    fMethod = httpIllegalMethod;
    fVersion = httpIllegalVersion;
    fRequestLine = NULL;
    fAbsoluteURI = NULL;
    fRelativeURI = NULL;
    fAbsoluteURIScheme = NULL;
    fHostHeader = NULL;
    fRequestPath = NULL;
    fStatusCode = 0;
    fRequestKeepAlive = false;
}

// Destructor
 HTTPRequest::~HTTPRequest()
 {
    //
 }
//Parses the request
PARSE_ERROR HTTPRequest::Parse()
{
    ASSERT(fRequestHeader.Ptr != NULL);
    StringParser parser(&fRequestHeader);

    // Store the request line (used for logging)
    // (ex: GET /index.html HTTP/1.0)
    StringParser requestLineParser(&fRequestHeader);
    requestLineParser.ConsumeUntil(&fRequestLine, StringParser::sEOLMask);

    // Parse request line returns an error if there is an error in the
    // request URI or the formatting of the request line.
    // If the method or version are not found, they are set
    // to httpIllegalMethod or httpIllegalVersion respectively,
    // and ERR_NoErr is returned.
    PARSE_ERROR err = ParseRequestLine(&parser);
    if (err != ERR_NoErr)
		return err;

    // Parse headers and set values of headers into fFieldValues array
    err = ParseHeaders(&parser);
    if (err != ERR_NoErr)
		return err;

    return ERR_NoErr;
}

PARSE_ERROR HTTPRequest::ParseRequestLine(StringParser* parser)
{
    // Get the method - If the method is not one of the defined methods
    // then it doesn't return an error but sets fMethod to httpIllegalMethod
    BCStrPtrLen theParsedData;
    parser->ConsumeWord(&theParsedData);
    fMethod = HTTPProtocol::GetMethod(&theParsedData);

    // Consume whitespace
    parser->ConsumeWhitespace();

    // Parse the URI - If it fails returns an error after setting
    // the fStatusCode to the appropriate error code
    PARSE_ERROR err = ParseURI(parser);
    if (err != ERR_NoErr)
		return err;

    // Consume whitespace
    parser->ConsumeWhitespace();

    // If there is a version, consume the version string
    BCStrPtrLen versionStr;
    parser->ConsumeUntil(&versionStr, StringParser::sEOLMask);
    // Check the version
    if (versionStr.Len > 0)
            fVersion = HTTPProtocol::GetVersion(&versionStr);

    // Go past the end of line
    if (!parser->ExpectEOL())
    {
        fStatusCode = httpBadRequest;
        return ERR_BadArgument;     // Request line is not properly formatted!
    }

    return ERR_NoErr;
}

PARSE_ERROR HTTPRequest::ParseURI(StringParser* parser)
{
    // read in the complete URL into fRequestAbsURI
    parser->ConsumeUntil(&fAbsoluteURI, sURLStopConditions);

    StringParser urlParser(&fAbsoluteURI);

    // we always should have a slash before the URI
    // If not, that indicates this is a full URI
    if (fAbsoluteURI.Ptr[0] != '/')
    {
        //if it is a full URL, store the scheme and host name
        urlParser.ConsumeLength(&fAbsoluteURIScheme, 7); //consume "http://"
        urlParser.ConsumeUntil(&fHostHeader, '/');
    }

    // whatever is in this position is the relative URI
    BCStrPtrLen relativeURI(urlParser.GetCurrentPosition(),
		urlParser.GetDataReceivedLen() - urlParser.GetDataParsedLen());
    // read this URI into fRequestRelURI
    fRelativeURI = relativeURI;

    // Allocate memory for fRequestPath
    uint32_t len = fRelativeURI.Len;
    len++;
    char * relativeURIDecoded = (char *)m_sPool.Alloc(len);

    int32_t theBytesWritten = StringTranslator::DecodeURL(
		fRelativeURI.Ptr, fRelativeURI.Len, relativeURIDecoded, len);

    //if negative, an error occurred, reported as an PARSE_ERROR
    //we also need to leave room for a terminator.
    if ((theBytesWritten < 0) || ((uint32_t)theBytesWritten == len))
    {
        fStatusCode = httpBadRequest;
        return ERR_BadArgument;
    }
    fRequestPath = (char *)m_sPool.Alloc(theBytesWritten + 1);
    ::memcpy(fRequestPath, relativeURIDecoded + 1, theBytesWritten);
    fRequestPath[theBytesWritten] = '\0';
    return ERR_NoErr;
}

// Parses the Connection header and makes sure that request is properly terminated
PARSE_ERROR HTTPRequest::ParseHeaders(StringParser* parser)
{
    BCStrPtrLen theKeyWord;
    bool isStreamOK;

    //Repeat until we get a \r\n\r\n, which signals the end of the headers
    while ((parser->PeekFast() != '\r') && (parser->PeekFast() != '\n'))
    {
        //First get the header identifier

        isStreamOK = parser->GetThru(&theKeyWord, ':');
        if (!isStreamOK)
        {       // No colon after header!
            fStatusCode = httpBadRequest;
            return ERR_BadArgument;
        }

        if (parser->PeekFast() == ' ')
        {        // handle space, if any
            isStreamOK = parser->Expect(' ');
            ASSERT(isStreamOK);
        }

        //Look up the proper header enumeration based on the header string.
        HTTPHeader theHeader = HTTPProtocol::GetHeader(&theKeyWord);

        BCStrPtrLen theHeaderVal;
        isStreamOK = parser->GetThruEOL(&theHeaderVal);

        if (!isStreamOK)
        {       // No EOL after header!
            fStatusCode = httpBadRequest;
            return ERR_BadArgument;
        }

        // If this is the connection header
        if ( theHeader == httpConnectionHeader )
        { // Set the keep alive boolean based on the connection header value
            SetKeepAlive(&theHeaderVal);
        }

        // Have the header field and the value; Add value to the array
        // If the field is invalid (or unrecognized) just skip over gracefully
        if ( theHeader != httpIllegalHeader )
            fFieldValues[theHeader] = theHeaderVal;

    }

    isStreamOK = parser->ExpectEOL();
    ASSERT(isStreamOK);

    return ERR_NoErr;
}

void HTTPRequest::SetKeepAlive(BCStrPtrLen *keepAliveValue)
{
    if ( sCloseString.EqualIgnoreCase(keepAliveValue->Ptr, keepAliveValue->Len) )
	{
        fRequestKeepAlive = sFalse;
	}
    else
    {
        ASSERT( sKeepAliveString.EqualIgnoreCase(keepAliveValue->Ptr, keepAliveValue->Len) );
        fRequestKeepAlive = sTrue;
    }
}

void HTTPRequest::PutStatusLine(
	StringFormatter* putStream,
	HTTPStatusCode status,
	HTTPVersion version)
{
    putStream->Put(*(HTTPProtocol::GetVersionString(version)));
    putStream->PutSpace();
    putStream->Put(*(HTTPProtocol::GetStatusCodeAsString(status)));
    putStream->PutSpace();
    putStream->Put(*(HTTPProtocol::GetStatusCodeString(status)));
    putStream->PutEOL();
}

BCStrPtrLen* HTTPRequest::GetHeaderValue(HTTPHeader inHeader)
{
    if ( inHeader !=  httpIllegalHeader )
        return &fFieldValues[inHeader];
    return NULL;
}

void HTTPRequest::CreateResponseHeader(HTTPVersion version, HTTPStatusCode statusCode)
{
    // If we are creating a second response for the same request, make sure and
    // deallocate memory for old response and allocate fresh memory
    m_sResponseFormatter.Reset();
	m_sPool.Clear();

    // Allocate memory for the response when you first create it
    char* responseString = (char *)m_sPool.Alloc(kMinHeaderSizeInBytes);
    m_sResponseHeader.Ptr = responseString;
	m_sResponseHeader.Len = kMinHeaderSizeInBytes;
    m_sResponseFormatter.Reset(m_sResponseHeader);

    //make a partial header for the given version and status code
    PutStatusLine(&m_sResponseFormatter, statusCode, version);
    ASSERT(fSvrHeader.Ptr != NULL);
    m_sResponseFormatter.Put(fSvrHeader);
    m_sResponseFormatter.PutEOL();
    m_sResponseHeader.Len = m_sResponseFormatter.GetCurrentOffset();
}

BCStrPtrLen* HTTPRequest::GetCompleteResponseHeader()
{
    m_sResponseFormatter.PutEOL();
    m_sResponseHeader.Len = m_sResponseFormatter.GetCurrentOffset();
    return &m_sResponseHeader;
}

void HTTPRequest::AppendResponseHeader(HTTPHeader inHeader, BCStrPtrLen* inValue)
{
    m_sResponseFormatter.Put(*(HTTPProtocol::GetHeaderString(inHeader)));
    m_sResponseFormatter.Put(sColonSpace);
    m_sResponseFormatter.Put(*inValue);
    m_sResponseFormatter.PutEOL();
    m_sResponseHeader.Len = m_sResponseFormatter.GetCurrentOffset();
}

void HTTPRequest::AppendContentLengthHeader(uint64_t length_64bit)
{
    char contentLength[256];
    sprintf(contentLength, "%" _U64BITARG_, length_64bit);
    BCStrPtrLen contentLengthPtr(contentLength);
    AppendResponseHeader(httpContentLengthHeader, &contentLengthPtr);
}

void HTTPRequest::AppendContentLengthHeader(uint32_t length_32bit)
{
    char contentLength[256];
    sprintf(contentLength, "%" _U32BITARG_, length_32bit);
    BCStrPtrLen contentLengthPtr(contentLength);
    AppendResponseHeader(httpContentLengthHeader, &contentLengthPtr);
}

void HTTPRequest::AppendConnectionCloseHeader()
{
    AppendResponseHeader(httpConnectionHeader, &sCloseString);
}

void HTTPRequest::AppendConnectionKeepAliveHeader()
{
    AppendResponseHeader(httpConnectionHeader, &sKeepAliveString);
}

void HTTPRequest::AppendDateAndExpiresFields()
{
    BCSpinMutex::Owner lock(G_sDateLock);
    DateBuffer* theDateBuffer = &G_sDataBuffer;
    theDateBuffer->InexactUpdate(); // Update the date buffer to the current date & time
    BCStrPtrLen theDate(theDateBuffer->GetDateBuffer(), DateBuffer::kDateBufferLen);

    // Append dates, and have this response expire immediately
    this->AppendResponseHeader(httpDateHeader, &theDate);
    this->AppendResponseHeader(httpExpiresHeader, &theDate);
}

void HTTPRequest::AppendDateField()
{
    BCSpinMutex::Owner lock(G_sDateLock);
	DateBuffer* theDateBuffer = &G_sDataBuffer;
    theDateBuffer->InexactUpdate(); // Update the date buffer to the current date & time
    BCStrPtrLen theDate(theDateBuffer->GetDateBuffer(), DateBuffer::kDateBufferLen);

    // Append date
    this->AppendResponseHeader(httpDateHeader, &theDate);
}

time_t HTTPRequest::ParseIfModSinceHeader()
{
    time_t theIfModSinceDate = (time_t) DateTranslator::ParseDate(&fFieldValues[httpIfModifiedSinceHeader]);
    return theIfModSinceDate;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPRequest.cpp
///////////////////////////////////////////////////////////////////////////////
