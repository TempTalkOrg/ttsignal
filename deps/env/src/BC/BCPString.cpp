
#include <BC/Utils.h>
#include <BC/BCPString.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// BCPString
///////////////////////////////////////////////////////////////////////////////
	
IMPLEMENT_FIXED_ALLOC(BCPString, 8);

const size_t BCPString::npos = bcSTRING_MAXLEN;

BCPString::BCPString()
	: m_pStr(NULL)
	, m_nLenShift(2)
	, m_nDataLen(0)
{
	clear();
}

BCPString::BCPString(const char ch, size_t n /* = 1 */)
	: m_pStr(NULL)
	, m_nLenShift(2)
	, m_nDataLen(0)
{
	clear();
	append(ch, n);
}

BCPString::BCPString(const char *lpStr, size_t nLen)
	: m_pStr(NULL)
	, m_nLenShift(2)
	, m_nDataLen(0)
{
	clear();
	if (nLen > 0)
	{
		append(lpStr, nLen);
	}
	else
	{
		append(lpStr, strlen(lpStr));
	}
}

BCPString::BCPString(const BCPString &str)
	: m_pStr(NULL)
	, m_nLenShift(2)
	, m_nDataLen(0)
{
	clear();
	append(str.c_str(), str.length());
}

BCPString::BCPString(const std::string &str)
	: m_pStr(NULL)
	, m_nLenShift(2)
	, m_nDataLen(0)
{
	clear();
	append(str.c_str(), str.length());
}

BCPString::~BCPString()
{
	if (m_pStr)
	{
		free(m_pStr);
	}
}

void BCPString::resize(int32_t newsize)
{
	int32_t ss = m_nLenShift;

	while (SIZE_TO_SHIFT(ss) < newsize)
	{
		ss++;
	}

	int32_t newdataz = SIZE_TO_SHIFT(ss);

	if (ss > m_nLenShift)
	{
		char * newdata = (char*)malloc(newdataz);
		if (!newdata)
		{
			throw ("BCPStringData::resize()",
				"You must asign a valid memory pool!");
		}
		memcpy(newdata, m_pStr, m_nDataLen);
		free(m_pStr);
		m_pStr = newdata;
		m_nLenShift = ss;
		m_pStr[m_nDataLen] = '\0';
	}
}

const char *BCPString::c_str() const
{
	return m_pStr;
}

const char *BCPString::sdata() const
{
	return m_pStr;
}

uint32_t BCPString::length() const
{
	return m_nDataLen;
}

char &BCPString::operator[](uint32_t k)
{
	if (k < Len())
	{
		return m_pStr[k];
	}
	throw BC_R_RANGE;
	return m_tmpChar;
}

char BCPString::operator [](int32_t k) const
{
	ASSERT(k >=0 && k < m_nDataLen);
	return m_pStr[k];
}

BCPString &BCPString::append(const char * lpStr, int32_t len)
{
	if (lpStr == NULL)
	{
		return *this;
	}
	resize(m_nDataLen + len + 1);
	memcpy(m_pStr + m_nDataLen, lpStr, len);
	m_nDataLen += len;
	m_pStr[m_nDataLen] = 0;
	return *this;
}

BCPString &BCPString::append(const char * lpStr)
{
	if (lpStr == NULL)
	{
		return *this;
	}
	return append(lpStr, (int32_t)strlen(lpStr));
}

BCPString &BCPString::append(int32_t  v)
{
	char abcd[64];
	memzero(abcd, sizeof(abcd));
	sprintf(abcd, "%d", v);
	return append(abcd);
}

BCPString &BCPString::append(uint32_t  v)
{
	char abcd[64];
	memzero(abcd, sizeof(abcd));
	sprintf(abcd, "%u", v);
	return append(abcd);
}

BCPString &BCPString::append(int64_t  v)
{
	char abcd[64];
	memzero(abcd, sizeof(abcd));
	sprintf(abcd, "%" _S64BITARG_, v);
	return append(abcd);
}

BCPString &BCPString::append(uint64_t  v)
{
	char abcd[64];
	memzero(abcd, sizeof(abcd));
	sprintf(abcd, "%" _U64BITARG_, v);
	return append(abcd);
}

BCPString &BCPString::append(double  v)
{
	char abcd[64];
	memzero(abcd, sizeof(abcd));
	sprintf(abcd, "%f", v);
	return append(abcd);
}

BCPString &BCPString::append(char v)
{
	return append(&v, 1);
}

BCPString &BCPString::append(char ch, uint32_t n)
{
	for (uint32_t i = 0;i < n;i++)
	{
		append(&ch, 1);
	}
	return *this;
}

BCPString &BCPString::append(const std::string &str)
{
	if (str.empty())
	{
		return *this;
	}
	return append(str.c_str(), str.length());
}

BCPString &BCPString::operator<<(int32_t v)
{
	return append(v);
}

BCPString &BCPString::operator<<(uint32_t v)
{
	return append(v);
}

BCPString &BCPString::operator<<(int64_t v)
{
	return append(v);
}

BCPString &BCPString::operator<<(uint64_t v)
{
	return append(v);
}

BCPString &BCPString::operator<<(double v)
{
	return append(v);
}

BCPString &BCPString::operator<<(const char * v)
{
	return append(v);
}

BCPString &BCPString::operator<<(char ch)
{
	return append(&ch, 1);
}

BCPString &BCPString::operator<<(const std::string& v)
{
	return append(v);
}

BCPString & BCPString::operator +=(const BCPString & other)
{
	append(other.m_pStr, other.m_nDataLen);

	return *this;
}

BCPString & BCPString::operator +=(const std::string & other)
{
	append(other.c_str(), other.length());

	return *this;
}

BCPString & BCPString::operator +=(const char * d)
{
	return append(d);
}

BCPString & BCPString::operator +=(char charValue)
{
	return append(charValue);
}

BCPString &BCPString::operator =(const BCPString & str)
{
	clear();
	append(str.c_str(), str.length());
	return *this;
}

BCPString &BCPString::operator =(const std::string & str)
{
	clear();
	append(str.c_str(), str.length());
	return *this;
}

BCPString &BCPString::operator =(const char * data)
{
	clear();
	return *this << (char*)data;
}

BCPString &BCPString::operator =(const char ch)
{
	clear();
	return *this << ch;
}

void BCPString::clear()
{
	m_nLenShift = 2;
	if (m_pStr)
	{
		free(m_pStr);
	}
	m_pStr = (char*)malloc(SIZE_TO_SHIFT(m_nLenShift));
	if (!m_pStr)
	{
		throw ("BCPStringData::clear()",
			"You must asign a valid memory pool!");
	}
	m_nDataLen = 0;
	m_pStr[m_nDataLen] = 0;
}

// Gives a duplicate symbol (presumably a case-insensitivity problem)
size_t BCPString::find(char ch, size_t nStart) const
{
	ASSERT( nStart <= Len() );
	const char *p = strchr(c_str() + nStart, ch);

	return p == NULL ? npos : p - c_str();
}

size_t BCPString::rfind(char ch, size_t nStart) const
{
	if ( nStart == npos )
	{
		nStart = Len();
	}
	else
	{
		ASSERT( nStart <= Len() );
	}

	const char *p = strrchr(c_str(), ch);

	if ( p == NULL )
		return npos;

	size_t result = p - c_str();
	return ( result > nStart ) ? npos : result;
}

// Load data from given file handler
char *BCPString::GetWriteBuffer(uint32_t nLen)
{
	resize(m_nDataLen + nLen + 1);
	return m_pStr + m_nDataLen;
}

void BCPString::UngetWriteBuffer(uint32_t nLen)
{
	Use(nLen);
}

void BCPString::Use(uint32_t nLen)
{
	int32_t nDstLen = m_nDataLen + nLen;
	if (nDstLen > SIZE_TO_SHIFT(m_nLenShift))
	{
		ASSERT(0);
		return;
	}
	m_nDataLen += nLen;
	resize(m_nDataLen + 1);
	m_pStr[m_nDataLen] = 0;
}

// Format functions
BCPString & BCPString::FormatV(const char * szFormat, va_list argList)
{
	char *fmtStr = NULL;
	
	bc_vasprintf(&fmtStr, szFormat, argList);
	if (fmtStr)
	{
		append(fmtStr);
		free(fmtStr);
	}
	return *this;
}

BCPString & BCPString::Format(const char *szFormat, ...)
{
	va_list args;

	va_start(args, szFormat);
	FormatV(szFormat, args);
	va_end(args);

	return *this;
}

BCPString & BCPString::Printf(const char *szFormat, ...)
{
	va_list args;

	va_start(args, szFormat);
	FormatV(szFormat, args);
	va_end(args);

	return *this;
}

// ---------------------------------------------------------------------------
// finding (return bcNOT_FOUND if not found and index otherwise)
// ---------------------------------------------------------------------------

// find a character
int BCPString::Find(char ch, bool bFromEnd) const
{
	const char *psz = bFromEnd ?
		strrchr(m_pStr, ch) : strchr(m_pStr, ch);

	return (psz == NULL) ? bcNOT_FOUND : psz - (const char*) m_pStr;
}

// find a sub-string (like strstr)
int BCPString::Find(const char *pszSub) const
{
	const char *psz = strstr(m_pStr, pszSub);

	return (psz == NULL) ? bcNOT_FOUND : psz - (const char*) m_pStr;
}

// ---------------------------------------------------------------------------
// case conversion
// ---------------------------------------------------------------------------

BCPString& BCPString::MakeUpper()
{
	for ( char *p = m_pStr; *p; p++ )
		*p = (char)toupper(*p);

	return *this;
}

BCPString& BCPString::MakeLower()
{
	for ( char *p = m_pStr; *p; p++ )
		*p = (char)tolower(*p);

	return *this;
}

// convert to upper case, return the copy of the string
BCPString BCPString::Upper() const
{
	BCPString s(*this);
	return s.MakeUpper();
}

// convert to lower case, return the copy of the string
BCPString BCPString::Lower() const
{
	BCPString s(*this);
	return s.MakeLower();
}

// extract string of length nCount starting at nFirst
BCPString BCPString::Mid(size_t nFirst, size_t nCount) const
{
	size_t nLen;

	nLen = m_nDataLen;

	// default value of nCount is bcSTRING_MAXLEN and means "till the end"
	if ( nCount == bcSTRING_MAXLEN )
	{
		nCount = nLen - nFirst;
	}

	// out-of-bounds requests return sensible things
	if ( nFirst + nCount > nLen )
	{
		nCount = nLen - nFirst;
	}

	if ( nFirst > nLen )
	{
		// AllocCopy() will return empty string
		nCount = 0;
	}

	BCPString dest;
	dest.append(m_pStr + nFirst, nCount);

	return dest;
}

// extract nCount first (leftmost) characters
BCPString BCPString::Left(size_t nCount) const
{
	if ( nCount > (size_t)m_nDataLen )
		nCount = m_nDataLen;

	BCPString dest(c_str(), nCount);
	return dest;
}

/*
* concatenation functions come in 5 flavours:
*  string + string
*  char   + string      and      string + char
*  C str  + string      and      string + C str
*/

BCPString operator+(const BCPString& str1, const BCPString& str2)
{
	BCPString s(str1.c_str(), str1.Len());
	s += str2;

	return s;
}

BCPString operator+(const BCPString& str, char ch)
{
	BCPString s(str.c_str(), str.Len());
	s += ch;

	return s;
}

BCPString operator+(char ch, const BCPString& str)
{
	BCPString s = ch;
	s += str;

	return s;
}

BCPString operator+(const BCPString& str, const char *psz)
{
	BCPString s(str.c_str(), str.Len());
	s += psz;

	return s;
}

BCPString operator+(const char *psz, const BCPString& str)
{
	BCPString s = psz;
	s += str;

	return s;
}

///////////////////////////////////////////////////////////////////////////////
// NULL String
///////////////////////////////////////////////////////////////////////////////

const char * G_sNullPString = "";

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file...
///////////////////////////////////////////////////////////////////////////////
