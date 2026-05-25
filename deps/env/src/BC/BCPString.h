
#ifndef BC_BCPSTRING_INCLUDE__
#define BC_BCPSTRING_INCLUDE__

#include <BC/Exports.h>
#include <BC/BCFixedAlloc.h>
#include <string>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

const unsigned int bcSTRING_MAXLEN = UINT_MAX - 100;
#define bcNOT_FOUND (-1)

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// BCPString
///////////////////////////////////////////////////////////////////////////////

class BC_API BCPString
{
	DECLARE_FIXED_ALLOC(BCPString);
public:
	BCPString();
	BCPString(const char ch, size_t n = 1);
	BCPString(const char *lpStr, size_t nLen = 0);
	BCPString(const BCPString &str);
	BCPString(const std::string &str);
	virtual ~BCPString();

	void			resize(int32_t);
	const char	*	sdata() const;
	uint32_t		Len() const { return m_nDataLen; }
	uint32_t		length() const;
	uint32_t		size() const {return Len();}
	bool			empty() const{ return IsEmpty(); }
	const char	*	c_str() const;
	char		&	operator[](uint32_t k);
	char			operator [](int32_t k) const;
	operator const char *() const{ return c_str(); };

	BCPString	&	append(const char * v, int32_t len) ;
	BCPString	&	append(const char * v);
	BCPString	&	append(int32_t v);
	BCPString	&	append(uint32_t v);
	BCPString	&	append(int64_t v);
	BCPString	&	append(uint64_t v);
	BCPString	&	append(double v);
	BCPString	&	append(char v);
	BCPString	&	append(char ch, uint32_t n);
	BCPString	&	append(const std::string &v);
	BCPString	&	operator <<(int32_t v);
	BCPString	&	operator <<(uint32_t v);
	BCPString	&	operator <<(int64_t v);
	BCPString	&	operator <<(uint64_t v);
	BCPString	&	operator <<(double v);
	BCPString	&	operator <<(const char * v);
	BCPString	&	operator <<(char ch);
	BCPString	&	operator <<(const std::string& v);
	BCPString	&	operator +=(const BCPString &);
	BCPString	&	operator +=(const std::string &);
	BCPString	&	operator +=(const char *);
	BCPString	&	operator +=(char charValue);
private:
	// non-destructive concatenation
	//
	friend BCPString BCDLLEXPORT operator+(const BCPString& string1, const BCPString& string2);
	//
	friend BCPString BCDLLEXPORT operator+(const BCPString& string, char ch);
	//
	friend BCPString BCDLLEXPORT operator+(char ch, const BCPString& string);
	//
	friend BCPString BCDLLEXPORT operator+(const BCPString& string, const char *psz);
	//
	friend BCPString BCDLLEXPORT operator+(const char *psz, const BCPString& string);
public:
	BCPString	&	operator =(const BCPString&);
	BCPString	&	operator =(const std::string&);
	BCPString	&	operator =(const char *);
	BCPString	&	operator =(const char ch);
	void			clear();
	void			reserve(uint32_t size){ clear(); resize(size);}
	char		*	GetWriteBuffer(uint32_t nLen);
	void			UngetWriteBuffer(uint32_t nLen);
	void			Use(uint32_t nLen);

	// Borrow from BCString
	// string comparison
	// case-sensitive comparison (returns a value < 0, = 0 or > 0)
	int				Cmp(const char *psz) const { return strcmp(sdata(), psz); }

	// Format functions
	BCPString	&	FormatV(const char * szFormat, va_list argList);
	BCPString	&	Format(const char *szFormat, ...);
	// Print function
	BCPString	&	Printf(const char *szFormat, ...);

	bool			IsEmpty() const{ return Len() == 0;}

	// lib.string.modifiers
	// same as `this_string = str'
	BCPString	&	assign(const BCPString& str)
	{ return *this = str; }
	// same as ` = str[pos..pos + n]
	BCPString	&	assign(const BCPString& str, uint32_t pos, uint32_t n)
	{ clear(); return append(str.c_str() + pos, n); }
	// same as `= first n (or all if n == npos) characters of sz'
	BCPString	&	assign(const char *sz, uint32_t n = npos)
	{ clear(); return append(sz, n == npos ? strlen(sz) : n); }
	// same as `= n copies of ch'
	BCPString	&	assign(uint32_t n, char ch)
	{ clear(); return append(ch, n); }

	// find the first occurence of character ch after nStart
	size_t		find(char ch, size_t nStart = 0) const;
	// as find, but from the end
	size_t		rfind(char ch, size_t nStart = npos) const;

	// searching and replacing
	// searching (return starting index, or -1 if not found)
	int Find(char ch, bool bFromEnd = FALSE) const;   // like strchr/strrchr
	// searching (return starting index, or -1 if not found)
	int Find(const char *pszSub) const;               // like strstr

	// case conversion
	// convert to upper case in place, return the string itself
	BCPString& MakeUpper();
	// convert to upper case, return the copy of the string
	// Here's something to remember: BC++ doesn't like returns in inlines.
	BCPString Upper() const ;
	// convert to lower case in place, return the string itself
	BCPString& MakeLower();
	// convert to lower case, return the copy of the string
	BCPString Lower() const ;

	// simple sub-string extraction
	// return substring starting at nFirst of length nCount (or till the end
	// if nCount = default value)
	BCPString Mid(size_t nFirst, size_t nCount = bcSTRING_MAXLEN) const;

	// operator version of Mid()
	BCPString  operator()(size_t start, size_t len) const
	{ return Mid(start, len); }

	// use Mid()
	BCPString SubString(size_t from, size_t to) const
	{ return Mid(from, (to - from + 1)); }

	// use Mid()
	BCPString SubStr(size_t from, size_t size) const
	{ return Mid(from, size); }

#if (defined(UNICODE) && defined(_WIN32)) 
	LPCWSTR wc_str() const;
#endif // UNICODE && _WIN32

	// get first nCount characters
	BCPString Left(size_t nCount) const;
protected:
private:
	char			*	m_pStr;
	int32_t				m_nLenShift;
	int32_t				m_nDataLen;
	char				m_tmpChar;

public:
	static const size_t	npos;
};

// ---------------------------------------------------------------------------
// BCPString comparison functions: operator versions are always case sensitive
// ---------------------------------------------------------------------------

inline bool operator==(const BCPString& s1, const BCPString& s2)
{ return (s1.Len() == s2.Len()) && (s1.Cmp(s2) == 0); }
inline bool operator==(const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) == 0; }
inline bool operator==(const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) == 0; }
inline bool operator!=(const BCPString& s1, const BCPString& s2)
{ return (s1.Len() != s2.Len()) || (s1.Cmp(s2) != 0); }
inline bool operator!=(const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) != 0; }
inline bool operator!=(const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) != 0; }
inline bool operator< (const BCPString& s1, const BCPString& s2)
{ return s1.Cmp(s2) < 0; }
inline bool operator< (const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) <  0; }
inline bool operator< (const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) >  0; }
inline bool operator> (const BCPString& s1, const BCPString& s2)
{ return s1.Cmp(s2) >  0; }
inline bool operator> (const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) >  0; }
inline bool operator> (const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) <  0; }
inline bool operator<=(const BCPString& s1, const BCPString& s2)
{ return s1.Cmp(s2) <= 0; }
inline bool operator<=(const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) <= 0; }
inline bool operator<=(const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) >= 0; }
inline bool operator>=(const BCPString& s1, const BCPString& s2)
{ return s1.Cmp(s2) >= 0; }
inline bool operator>=(const BCPString& s1, const char  * s2)
{ return s1.Cmp(s2) >= 0; }
inline bool operator>=(const char  * s1, const BCPString& s2)
{ return s2.Cmp(s1) <= 0; }


// non-destructive concatenation
//
BCPString BCDLLEXPORT operator+(const BCPString& string1, const BCPString& string2);
//
BCPString BCDLLEXPORT operator+(const BCPString& string, char ch);
//
BCPString BCDLLEXPORT operator+(char ch, const BCPString& string);
//
BCPString BCDLLEXPORT operator+(const BCPString& string, const char *psz);
//
BCPString BCDLLEXPORT operator+(const char *psz, const BCPString& string);

BCDLLEXPORT_DATA(extern const char *) G_sNullPString;

#define NULLPSTRING			G_sNullPString

BC_API
uint32_t BCUtf8ToOEM(const char * szUtf8, BCPString &refOEM);

BC_API
uint32_t BCOEMToUtf8(const char * szOEM, BCPString &refUtf8);

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_BCPSTRING_INCLUDE__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
