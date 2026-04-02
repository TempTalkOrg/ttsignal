///////////////////////////////////////////////////////////////////////////////
// file : HTTPRequest.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_HTTPREQUEST_H_INCLUDED__
#define HTTP_HTTPREQUEST_H_INCLUDED__

#include <HTTP/Exports.h>
#include <BC/BCStrPtrLen.h>
#include "HTTPProtocol.h"
#include "StringUtils.h"


using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// Class : HTTPRequest
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPRequest
{
public:
    // Constructor
    HTTPRequest(BCStrPtrLen* serverHeader, BCStrPtrLen* requestPtr);

    // This cosntructor is used when the request has been parsed and thrown away
    // and the response has to be created
    HTTPRequest(BCStrPtrLen* serverHeader);

    // Destructor
    virtual ~HTTPRequest();

    // Should be called before accessing anything in the request header
    // Calls ParseRequestLine and ParseHeaders
    PARSE_ERROR              Parse();

    // Basic access methods for the HTTP method, the absolute request URI,
    // the host name from URI, the relative request URI, the request file path,
    // the HTTP version, the Status code, the keep-alive tag.
    HTTPMethod              GetMethod(){ return fMethod; }
    BCStrPtrLen			*   GetRequestLine(){ return &fRequestLine; }
    BCStrPtrLen			*   GetRequestAbsoluteURI(){ return &fAbsoluteURI; }
    BCStrPtrLen			*   GetSchemefromAbsoluteURI(){ return &fAbsoluteURIScheme; }
    BCStrPtrLen			*   GetHostfromAbsoluteURI(){ return &fHostHeader; }
    BCStrPtrLen			*   GetRequestRelativeURI(){ return &fRelativeURI; }
    char				*   GetRequestPath(){ return fRequestPath; }
    HTTPVersion             GetVersion(){ return fVersion; }
    HTTPStatusCode          GetStatusCode(){ return fStatusCode; }
    bool					IsRequestKeepAlive(){ return fRequestKeepAlive; }

    // If header field exists in the request, it will be found in the dictionary
    // and the value returned. Otherwise, NULL is returned.
    BCStrPtrLen*			GetHeaderValue(HTTPHeader inHeader);

    // Creates a header with the corresponding version and status code
    void                    CreateResponseHeader(HTTPVersion version, HTTPStatusCode statusCode);

    // To append response header fields as appropriate
    void                    AppendResponseHeader(HTTPHeader inHeader, BCStrPtrLen* inValue);
    void                    AppendDateAndExpiresFields();
    void                    AppendDateField();
    void                    AppendConnectionCloseHeader();
    void                    AppendConnectionKeepAliveHeader();
    void                    AppendContentLengthHeader(uint64_t length_64bit);
    void                    AppendContentLengthHeader(uint32_t length_32bit);

    // Returns the completed response header by appending CRLF to the end of the header
    // fields buffer
    BCStrPtrLen			*	GetCompleteResponseHeader();

    // Parse if-modified-since header
    time_t                  ParseIfModSinceHeader();

private:
    enum { kMinHeaderSizeInBytes = 512 };

    // Gets the method, version and calls ParseURI
    PARSE_ERROR              ParseRequestLine(StringParser* parser);
    // Parses the URI to get absolute and relative URIs, the host name and the file path
    PARSE_ERROR              ParseURI(StringParser* parser);
    // Parses the headers and adds them into a dictionary
    // Also calls SetKeepAlive with the Connection header field's value if it exists
    PARSE_ERROR              ParseHeaders(StringParser* parser);

    // Sets fRequestKeepAlive
    void                    SetKeepAlive(BCStrPtrLen* keepAliveValue);
    // Used in initialize and CreateResponseHeader
    void                    PutStatusLine(StringFormatter* putStream, HTTPStatusCode status, HTTPVersion version);
    //For writing into the premade headers
    BCStrPtrLen			*	GetServerHeader(){ return &fSvrHeader; }

    // Complete request and response headers
    BCStrPtrLen						fRequestHeader;
    ResizeableStringFormatter		m_sResponseFormatter;
    BCStrPtrLen						m_sResponseHeader;

    // Private members
    HTTPMethod				fMethod;
    HTTPVersion				fVersion;

    BCStrPtrLen				fRequestLine;

    // For the URI (fAbsoluteURI and fRelativeURI are the same if the URI is of the form "/path")
    BCStrPtrLen				fAbsoluteURI;       // If it is of the form "http://foo.bar.com/path"
    BCStrPtrLen				fRelativeURI;       // If it is of the form "/path"

    // If it is an absolute URI, these fields will be filled in
    // "http://foo.bar.com/path" => fAbsoluteURIScheme = "http", fHostHeader = "foo.bar.com",
    // fRequestPath = "path"
    BCStrPtrLen				fAbsoluteURIScheme;
    BCStrPtrLen				fHostHeader;        // If the full url is given in the request line
    char*					fRequestPath;       // Also contains the query string

    HTTPStatusCode			fStatusCode;
    bool					fRequestKeepAlive;              // Keep-alive information in the client request
    BCStrPtrLen				fFieldValues[httpNumHeaders];   // Array of header field values parsed from the request
    BCStrPtrLen				fSvrHeader;                     // Server header set up at initialization
    static BCStrPtrLen		sColonSpace;
    static uint8_t			sURLStopConditions[];
	KBPool					m_sPool;
};
///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

#endif // HTTP_HTTPREQUEST_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPRequest.h
///////////////////////////////////////////////////////////////////////////////
