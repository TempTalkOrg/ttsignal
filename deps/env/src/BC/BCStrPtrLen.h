
#ifndef BC_BCSTRPTRLEN_INCLUDE__
#define BC_BCSTRPTRLEN_INCLUDE__

#include "BC/Exports.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : StrPtrLen
///////////////////////////////////////////////////////////////////////////////


#define STRPTRLENTESTING 0

class BC_API BCStrPtrLen
{
public:

	//CONSTRUCTORS/DESTRUCTOR
	//These are so tiny they can all be inlined
	BCStrPtrLen() : Ptr(NULL), Len(0) {}
	BCStrPtrLen(char* sp) : Ptr(sp), Len(sp != NULL ? strlen(sp) : 0) {}
    BCStrPtrLen(const char* sp) : Ptr((char *)sp), Len(sp != NULL ? strlen(sp) : 0) {}
	BCStrPtrLen(char *sp, uint32_t len) : Ptr(sp), Len(len) {}
	virtual ~BCStrPtrLen() {}

	//OPERATORS:
	bool Equal(const BCStrPtrLen &compare) const;
	bool EqualIgnoreCase(const char* compare, const size_t len) const;
	bool EqualIgnoreCase(const BCStrPtrLen &compare) const
	{
		return EqualIgnoreCase(compare.Ptr, compare.Len);
	}
	bool Equal(const char* compare) const;
	bool NumEqualIgnoreCase(const char* compare, const uint32_t len) const;

	void Delete() { delete [] Ptr; Ptr = NULL; Len = 0; }
	char *ToUpper() { for (uint32_t x = 0; x < Len ; x++) Ptr[x] = toupper (Ptr[x]); return Ptr;}

	char *FindStringCase(const char *queryCharStr, BCStrPtrLen *resultStr, bool caseSensitive) const;

	char *FindString(BCStrPtrLen *queryStr, BCStrPtrLen *outResultStr)
	{
		ASSERT(queryStr != NULL);
		ASSERT(queryStr->Ptr != NULL);
		ASSERT(0 == queryStr->Ptr[queryStr->Len]);
		return FindStringCase(queryStr->Ptr, outResultStr,true);
	}

	char *FindStringIgnoreCase(BCStrPtrLen *queryStr, BCStrPtrLen *outResultStr)
	{
		ASSERT(queryStr != NULL);
		ASSERT(queryStr->Ptr != NULL);
		ASSERT(0 == queryStr->Ptr[queryStr->Len]);
		return FindStringCase(queryStr->Ptr, outResultStr,false);
	}

	char *FindString(BCStrPtrLen *queryStr)
	{
		ASSERT(queryStr != NULL);
		ASSERT(queryStr->Ptr != NULL);
		ASSERT(0 == queryStr->Ptr[queryStr->Len]);
		return FindStringCase(queryStr->Ptr, NULL,true);
	}

	char *FindStringIgnoreCase(const BCStrPtrLen *queryStr)
	{
		ASSERT(queryStr != NULL);
		ASSERT(queryStr->Ptr != NULL);
		ASSERT(0 == queryStr->Ptr[queryStr->Len]);
		return FindStringCase(queryStr->Ptr, NULL,false);
	}

	char *FindString(const char *queryCharStr)
	{
		return FindStringCase(queryCharStr, NULL,true);
	}
	char *FindStringIgnoreCase(const char *queryCharStr)
	{
		return FindStringCase(queryCharStr, NULL,false);
	}
	char *FindString(const char *queryCharStr, BCStrPtrLen *outResultStr)
	{
		return FindStringCase(queryCharStr, outResultStr,true);
	}
	char *FindStringIgnoreCase(const char *queryCharStr, BCStrPtrLen *outResultStr)
	{
		return FindStringCase(queryCharStr, outResultStr,false);
	}

	char *FindString(BCStrPtrLen &query, BCStrPtrLen *outResultStr)
	{
		return FindString( &query, outResultStr);
	}
	char *FindStringIgnoreCase(BCStrPtrLen &query, BCStrPtrLen *outResultStr)
	{
		return FindStringIgnoreCase( &query, outResultStr);
	}
	char *FindString(BCStrPtrLen &query)
	{
		return FindString( &query);
	}
	char *FindStringIgnoreCase(const BCStrPtrLen &query)
	{
		return FindStringIgnoreCase( &query);
	}

	BCStrPtrLen& operator=(const BCStrPtrLen& newStr)
	{
		Ptr = newStr.Ptr; Len = newStr.Len;
		return *this;
	}
	BCStrPtrLen& operator=(const void * newStr)
	{
		Ptr = (char *)newStr; 
		if (Ptr)
		{
			Len = strlen(Ptr);
		}
		else
		{
			Len = 0;
		}
		return *this;
	}
	char operator[](int i)
	{
		/*ASSERT(i<Len);i*/
		return Ptr[i];
	}
	void Set(char* inPtr, uint32_t inLen) { Ptr = inPtr; Len = inLen; }
	void Set(char* inPtr) { Ptr = inPtr; Len = (inPtr) ?  ::strlen(inPtr) : 0; }

	//This is a non-encapsulating interface. The class allows you to access its
	//data.
	char*       Ptr;
	size_t      Len;

	// convert to a "NEW'd" zero terminated char array
	char*   GetAsCString() const;

	//Utility function
	size_t    TrimTrailingWhitespace();
	size_t    TrimLeadingWhitespace();

	size_t  RemoveWhitespace();
	void  TrimWhitespace()
	{
		TrimLeadingWhitespace();
		TrimTrailingWhitespace();
	}

private:

	static uint8_t    sCaseInsensitiveMask[];
	static uint8_t    sNonPrintChars[];
};



class BCStrPtrLenDel : public BCStrPtrLen
{
public:
	BCStrPtrLenDel() : BCStrPtrLen() {}
	BCStrPtrLenDel(char* sp) : BCStrPtrLen(sp) {}
	BCStrPtrLenDel(char *sp, uint32_t len) : BCStrPtrLen(sp,len) {}
	~BCStrPtrLenDel() { Delete(); }
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////
}

///////////////////////////////////////////////////////////////////////////////
// End of file.
///////////////////////////////////////////////////////////////////////////////

#endif
