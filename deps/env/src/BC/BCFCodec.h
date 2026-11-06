
#ifndef BCFCODEC_H_INCLUDED__
#define BCFCODEC_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCStream.h>
#include <BC/BCBuffer.h>
#include <BC/BCPString.h>
#include <BC/BCVector.h>
#include <BC/BCFixedAlloc.h>
#include <string>
#ifdef HAVE_DUMP
#include <iosfwd>
#endif // HAVE_DUMP


// Valid BCF integer range
#define MIN_INT				(-268435457)
#define MAX_INT				(268435456)

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

typedef enum BCFTypeE
{
	BCF_TYPE_UNDEFINED		= 0x00,
	BCF_TYPE_NULL			= 0x01,
	BCF_TYPE_FALSE			= 0x02,
	BCF_TYPE_TRUE			= 0x03,
	BCF_TYPE_INT			= 0x04,
	BCF_TYPE_DOUBLE			= 0x05,
	BCF_TYPE_STRING			= 0x06,
	BCF_TYPE_XMLDOC			= 0x07,
	BCF_TYPE_DATE			= 0x08,
	BCF_TYPE_ARRAY			= 0x09,
	BCF_TYPE_OBJECT			= 0x0A,
	BCF_TYPE_XML			= 0x0B,
	BCF_TYPE_BYTEARRAY		= 0x0C,
	BCF_TYPE_TABLEENTRY		= 0x20,
}BCFTypeE;

class BCFVar;

//////////////////////////////////////////////////////////////////////////
/// Macros
//////////////////////////////////////////////////////////////////////////

#define IS_BCF_UNDEFINED(var)	\
	(var && var->GetType() == BCF_TYPE_UNDEFINED)
#define IS_BCF_NULL(var)	\
	(var && var->GetType() == BCF_TYPE_NULL)
#define IS_BCF_FALSE(var)	\
	(var && var->GetType() == BCF_TYPE_FALSE)
#define IS_BCF_TRUE(var)	\
	(var && var->GetType() == BCF_TYPE_TRUE)
#define IS_BCF_INT(var)	\
	(var && var->GetType() == BCF_TYPE_INT)
#define IS_BCF_DOUBLE(var)	\
	(var && var->GetType() == BCF_TYPE_DOUBLE)
#define IS_BCF_STRING(var)	\
	(var && var->GetType() == BCF_TYPE_STRING)
#define IS_BCF_XMLDOC(var)	\
	(var && var->GetType() == BCF_TYPE_XMLDOC)
#define IS_BCF_DATE(var)	\
	(var && var->GetType() == BCF_TYPE_DATE)
#define IS_BCF_ARRAY(var)	\
	(var && var->GetType() == BCF_TYPE_ARRAY)
#define IS_BCF_OBJECT(var)	\
	(var && var->GetType() == BCF_TYPE_OBJECT)
#define IS_BCF_XML(var)	\
	(var && var->GetType() == BCF_TYPE_XML)
#define IS_BCF_BYTEARRAY(var)	\
	(var && var->GetType() == BCF_TYPE_BYTEARRAY)
#define IS_BCF_BOOL(var)	\
	(IS_BCF_TRUE(var) || IS_BCF_FALSE(var))
#define IS_BCF_NUMBER(var)	\
	(var && (var->GetType() == BCF_TYPE_DOUBLE || var->GetType() == BCF_TYPE_INT))

// Get value
#define GET_BCF_INT(var)	GetBCFInt(var)
#define GET_BCF_DOUBLE(var)	GetBCFDouble(var)
#define GET_BCF_STRING(var)	\
	(((BCFString *)var)->GetValue())
#define GET_BCF_BYTEARRAY(var)	\
	(((BCFByteArray *)var)->GetBuffer())
#define GET_BCF_BOOL(var)	\
	(IS_BCF_TRUE(var)?TRUE:FALSE)

uint64_t GetBCFInt(BCFVar *pVar);
double   GetBCFDouble(BCFVar *pVar);

//////////////////////////////////////////////////////////////////////////
/// class BCFVar - the base BCF encoded variant class
//////////////////////////////////////////////////////////////////////////

class BC_API BCFVar : public BCNodeList::Node
{
public:
	virtual ~BCFVar(){}
	virtual uint8_t		GetType() const					= 0;
	virtual void		Write(BCOStream* pOutput) const	= 0;
	virtual void		Read(BCIStream* pInput)			= 0;
	virtual BCFVar *	Clone() const					= 0;

	bool				IsNull() const
	{ 
		return GetType() == BCF_TYPE_NULL; 
	}

	const char	*	GetTypeName() const;

#ifdef HAVE_DUMP
	virtual std::ostream& Dump(std::ostream& os) const=0;
	std::ostream& DumpAll(std::ostream& os) const
	{
		os<<"("<<GetTypeName()<<") ";
		Dump(os);
		return os;
	}
	const BCString toString() const;
#endif
protected:
	BCFVar(){}
private:
	DECLARE_NO_COPY_CLASS(BCFVar);
};

typedef TNodeList<BCFVar>			BCFVarList;

//////////////////////////////////////////////////////////////////////////
/// class BCFUndefined
//////////////////////////////////////////////////////////////////////////

class BC_API BCFUndefined : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFUndefined);
public:
	BCFUndefined();
	virtual ~BCFUndefined();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFUndefined &	operator = (const BCFUndefined &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class BCFNull
//////////////////////////////////////////////////////////////////////////

class BC_API BCFNull : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFNull);
public:
	BCFNull();
	virtual ~BCFNull();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFNull	&	operator = (const BCFNull &other);
#ifdef HAVE_DUMP
	std::ostream& Dump(std::ostream& os) const{ return os; };
#endif
private:
};

///////////////////////////////////////////////////////////////////////////////
// class : BCFFalse
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFFalse : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFFalse);
public:
	BCFFalse();
	~BCFFalse();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFFalse	&	operator = (const BCFFalse &other);
private:
};

///////////////////////////////////////////////////////////////////////////////
// class : BCFTrue
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFTrue : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFTrue);
public:
	BCFTrue();
	~BCFTrue();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFTrue	&	operator = (const BCFTrue &other);
private:
};

///////////////////////////////////////////////////////////////////////////////
// class : BCFInt
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFInt : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFInt);
public:
	BCFInt(uint64_t nValue = 0);
	~BCFInt();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFInt		&	operator = (const BCFInt &other);
	void			SetValue(uint64_t val){ m_nValue = val; }
	uint64_t		GetValue() const{ return m_nValue; }
private:
	uint64_t		m_nValue;
};

//////////////////////////////////////////////////////////////////////////
/// class BCFDouble
//////////////////////////////////////////////////////////////////////////

class BC_API BCFDouble : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFDouble);
public:
	BCFDouble();
	virtual ~BCFDouble();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFDouble	&	operator = (const BCFDouble &other);
	void			SetValue(float64_t val){ m_fValue = val; }
	float64_t		GetValue() const{ return m_fValue; }
	void			SetValue(const uint8_t* octets){ memcpy2(&m_fValue,octets,8); }
#ifdef HAVE_DUMP
	std::ostream& Dump(std::ostream& os) const;
#endif
private:
	float64_t		m_fValue;
};

//////////////////////////////////////////////////////////////////////////
/// class BCFString - a string encoded in UTF-8
//////////////////////////////////////////////////////////////////////////

class BC_API BCFString : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFString);
public:
	BCFString();
	BCFString(const BCPString &other);
	BCFString(const std::string &other);
	virtual ~BCFString();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual BCFVar *	Clone() const;
	BCFString		&	operator = (const BCFString &other);
	BCFString		&	operator = (const std::string &other);
	void				SetValue(const char *szVal){ m_strValue = szVal; }
	void				SetValue(const std::string &str){ m_strValue = str; }
	void				SetValue(const BCPString &val){ m_strValue = val; }
	const BCPString&	GetValue() const{ return m_strValue; }
#ifdef HAVE_DUMP
	std::ostream& Dump(std::ostream& os) const{ return os << m_strValue.c_str(); }
#endif
protected:
	BCPString			m_strValue;
private:
};

//////////////////////////////////////////////////////////////////////////
/// class BCFXmlDoc - A xml UTF-8 long string
//////////////////////////////////////////////////////////////////////////

class BC_API BCFXmlDoc : public BCFString
{
	DECLARE_FIXED_ALLOC(BCFXmlDoc);
public:
	BCFXmlDoc();
	~BCFXmlDoc();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFXmlDoc	&	operator = (const BCFXmlDoc &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class BCFDate - An BCF Date is serialized as the number 
///                 of milliseconds elapsed since the epoch of midnight 
///                 on 1st Jan 1970 in the UTC time zone.
//////////////////////////////////////////////////////////////////////////

class BC_API BCFDate : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFDate);
public:
	BCFDate();
	~BCFDate();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream *pInput);
	BCFVar		*	Clone() const;
	BCFDate		&	operator = (const BCFDate &other);

private:
	uint32_t		m_nU29DValue;
	float64_t		m_fDateTime;
};

//////////////////////////////////////////////////////////////////////////
/// class BCFTableEntry - this class represents an BCF Table 
///                       entry (a name-value pair)
//////////////////////////////////////////////////////////////////////////

class BCFTableEntry;
class BCFTable;

class BC_API BCFTableEntry : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFTableEntry);
public:
	BCFTableEntry();
	virtual ~BCFTableEntry();

	BCPString		&	GetKey();
	BCFVar			*	GetValue();

	void				SetKey(LPCSTR keyStr);
	void				SetKey(const std::string& keyStr);
	void				SetValue(BCFVar * val);
	BCFVar			*	ReplaceValue(BCFVar *val);

	uint8_t				GetType() const;
	void				Write(BCOStream* pOutput) const;
	void				Read(BCIStream* pInput);
	BCFTableEntry	*	Clone() const;
	BCFTableEntry	&	operator = (const BCFTableEntry &other);
#ifdef HAVE_DUMP
	std::ostream	&	Dump(std::ostream& os) const;
#endif

private:
	BCPString				m_strKey;
	BCFVar				*	m_pValue;
};

typedef TNodeList<BCFTableEntry>		BCFTableEntryList;

//////////////////////////////////////////////////////////////////////////
/// class BCFTableEntry - An BCFTable is a collection of name-value 
///                       pairs (BCFTableEntry)
//////////////////////////////////////////////////////////////////////////

class BC_API BCFTable : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFTable);
public:
	BCFTable();
	virtual ~BCFTable();

	BCFVar			*	Get(LPCSTR szKey);
	BCFTableEntry	*	BeginEntry() const;
	BCFTableEntry	*	NextEntry(BCFVar *pIter) const;
	BCFTableEntry	*	EndEntry() const;
	void				Put(LPCSTR szKey, BCFVar* var);
	void				PutBool(LPCSTR szKey, bool bValue);
	void				PutInt(LPCSTR szKey, uint64_t nValue);
	void				PutDouble(LPCSTR szKey, double dbValue);
	void				PutString(LPCSTR szKey, LPCSTR szValue);
	void				PutString(LPCSTR szKey, const std::string& strValue);
	bool				IsContainsKey(LPCSTR szKey);
	void				Remove(LPCSTR szKey);
	void				Clear();
	size_t				Size() const;
	BCRESULT			Qsort(int (* _PtFuncCompare)(LPCVOID, LPCVOID));

	virtual uint8_t		GetType() const	= 0;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	BCFTable		&	operator = (const BCFTable &other);
	BCFTable		&   operator +=(const BCFTable &other);

protected:
	BCFTableEntryList		m_lstEntries;
};

//////////////////////////////////////////////////////////////////////////
/// class BCFArray - An BCF Array is mostly same as an BCF Object, 
///                   as well as a long value
//////////////////////////////////////////////////////////////////////////

class BC_API BCFArray : public BCFTable
{
	DECLARE_FIXED_ALLOC(BCFArray);
public:
	BCFArray();
	virtual ~BCFArray();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFArray	&	operator = (const BCFArray &other);

	BCFVar		*	Get(uint32_t nIndex);
	BCFVar		*	Begin() const;
	BCFVar		*	Next(BCFVar *pIter) const;
	BCFVar		*	End() const;
	BCFVar		*	PopBack();
	uint32_t		Push(BCFVar* var);
	uint32_t		PushBool(bool bValue);
	uint32_t		PushInt(uint64_t nValue);
	uint32_t		PushDouble(double dbValue);
	uint32_t		PushString(LPCSTR szValue);
	uint32_t		Remove(uint32_t nIndex);
	void			Clear();
	uint32_t		Size() const;
private:
	BCFVarList			m_lstDenseVars;
};

//////////////////////////////////////////////////////////////////////////
/// class BCFObject - An BCF Object is just a pair of utf8 names with 
///                   amf variant values
//////////////////////////////////////////////////////////////////////////

class BC_API BCFObject : public BCFTable
{
	DECLARE_FIXED_ALLOC(BCFObject);
public:
	BCFObject();
	virtual ~BCFObject();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual BCFVar  *	Clone() const;
	BCFObject		&	operator = (const BCFObject &other);
	BCFObject		&   operator += (const BCFObject &other);
#ifdef HAVE_DUMP
	std::ostream	&	Dump(std::ostream& os) const{ return BCFTable::Dump(os); }
#endif
private:
};

//////////////////////////////////////////////////////////////////////////
/// class BCFXml - A xml UTF-8 string
//////////////////////////////////////////////////////////////////////////

class BC_API BCFXml : public BCFString
{
	DECLARE_FIXED_ALLOC(BCFXml);
public:
	BCFXml();
	~BCFXml();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream* pInput);
	BCFVar		*	Clone() const;
	BCFXml		&	operator = (const BCFXml &other);
private:
};

//////////////////////////////////////////////////////////////////////////
///	BCFByteArray
//////////////////////////////////////////////////////////////////////////

class BC_API BCFByteArray : public BCFVar
{
	DECLARE_FIXED_ALLOC(BCFByteArray);
public:
	BCFByteArray();
	BCFByteArray(const BCFByteArray &lhs);
	~BCFByteArray();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream *pInput);
	BCFVar		*	Clone() const;
	BCFByteArray &	operator = (const BCFByteArray &other);

	uint8_t			operator[] (uint32_t index) const;
	uint8_t		&	operator[] (uint32_t index);
	void			Push(uint8_t value);
	void			Push(const uint8_t *buffer, uint32_t count);
	uint32_t		GetLength() const { return m_sBuffer.UsedLength(); }
	void			SetLength(uint32_t newLength);
	BCBuffer	*	GetBuffer() { return &m_sBuffer; }
	void			Clear();

protected:
	mutable BCBuffer		m_sBuffer;
};

//////////////////////////////////////////////////////////////////////////
///	BCFCodec - A Codec to Encode and Decode BCF types into raw data
//////////////////////////////////////////////////////////////////////////

class BC_API BCFCodec
{
public:
	BCFCodec();
	~BCFCodec();

	// Read BCF object without object type
	static BCFVar	*	Decode(BCIStream* pInput);
	// Write BCF object with object type
	static void			Encode(BCOStream* pOutput, const BCFVar* var);
	static BCFVar	*	NewVariant(uint8_t eAmfType);
	static const char*	GetTypeName(uint8_t eAmfType);
	static double		ParseDouble(const uint8_t* buf);
	static int32_t		EncodeVLInteger(BCOStream *pOutput, uint32_t nValue);
	static int32_t		DecodeVLInteger(BCIStream *pInput, uint32_t *pValue);
	static int32_t		DecodeString(BCIStream *pInput, BCPString &refStr);
	static int32_t		EncodeString(
							BCOStream *pOutput,
							const BCPString &refStr,
							bool bHasType = false);
private:
	DECLARE_NO_COPY_CLASS(BCFCodec);
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BCFCODEC_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file BCFCodec.h
///////////////////////////////////////////////////////////////////////////////
