///////////////////////////////////////////////////////////////////////////////
// file : AMF.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_AMF_H_INCLUDED__
#define RTMP_AMF_H_INCLUDED__

#include <memory>
#include <BC/BCNodeList.h>
#include <BC/BCHashTable.h>
#include <BC/BCVector.h>
#include <BC/BCBuffer.h>
#include <RTMP/Exports.h>

// Valid AMF0 integer types
#define MAX_USHORT		65535

// Client types defined in AMF remoting message
#define FLASH_8						0x00
#define FLASH_COM					0x01
#define FLASH_9						0x03


// Valid AMF3 integer range
#define MIN_INT				(-268435457)
#define MAX_INT				(268435456)

// Reference bit
#define REFERENCE_BIT				0x01

// Empty string
#define EMPTY_STRING_TYPE			0x01

// Object Headers
#define STATIC_OBJECT				0x03
#define EXTERNALIZABLE_OBJECT		0x07
#define DYNAMIC_OBJECT				0x0B


inline bool IsReference(uint32_t nValue)
{
	return ((nValue & REFERENCE_BIT) == 0);
}

inline bool IsTraitsRef(uint32_t nValue)
{
	return ((nValue & STATIC_OBJECT) == REFERENCE_BIT);
}

inline bool IsClassDef(uint32_t nValue)
{
	return ((nValue & EXTERNALIZABLE_OBJECT) == STATIC_OBJECT);
}

inline bool IsExternalizable(uint32_t nValue)
{
	return ((nValue & EXTERNALIZABLE_OBJECT) == EXTERNALIZABLE_OBJECT);
}

inline bool IsDynamicClass(uint32_t nValue)
{
	return ((nValue & DYNAMIC_OBJECT) == DYNAMIC_OBJECT);
}

namespace BC
{
	class BCIStream;
	class BCOStream;
}

using namespace BC;

//////////////////////////////////////////////////////////////////////////
/// namespace AMF
//////////////////////////////////////////////////////////////////////////

namespace AMF
{

//////////////////////////////////////////////////////////////////////////
/// Namespace : AMFType
//////////////////////////////////////////////////////////////////////////

namespace AMFType
{
	typedef enum AMF0TypeE
	{
		AMF0_DOUBLE			= 0x00,
		AMF0_BOOL			= 0x01,
		AMF0_STRING			= 0x02,
		AMF0_OBJECT			= 0x03,
		AMF0_MOVIECLIP		= 0x04,
		AMF0_NULL			= 0x05,
		AMF0_UNDEFINED		= 0x06,
		AMF0_REFERENCE		= 0x07,
		AMF0_ECMAARRAY		= 0x08,
		AMF0_ENDOFOBJECT	= 0x09,
		AMF0_STRICTARRAY	= 0x0A,
		AMF0_DATE			= 0x0B,
		AMF0_LSTRING		= 0x0C,
		AMF0_UNSUPPORTED	= 0x0D,
		AMF0_RECORDSET		= 0x0E,
		AMF0_XMLDOC			= 0x0F,
		AMF0_TYPEDOBJECT	= 0x10,
		AMF0_AVMPLUS		= 0x11,
	}AMF0TypeE;

	typedef enum AMF3TypeE
	{
		AMF3_UNDEFINED		= 0x00,
		AMF3_NULL			= 0x01,
		AMF3_FALSE			= 0x02,
		AMF3_TRUE			= 0x03,
		AMF3_INT			= 0x04,
		AMF3_DOUBLE			= 0x05,
		AMF3_STRING			= 0x06,
		AMF3_XMLDOC			= 0x07,
		AMF3_DATE			= 0x08,
		AMF3_ARRAY			= 0x09,
		AMF3_OBJECT			= 0x0A,
		AMF3_XML			= 0x0B,
		AMF3_BYTEARRAY		= 0x0C,
	}AMF3TypeE;

	typedef enum AMFCTypeE
	{
		AMF_UNDEFINED		= 0x20,
		AMF_NULL			= 0x21,
		AMF_BOOL			= 0x22,
		AMF_NUMBER			= 0x23,
		AMF_DATE			= 0x24,
		AMF_STRING			= 0x25,
		AMF_XMLDOC			= 0x26,
		AMF_ARRAY			= 0x27,
		AMF_OBJECT			= 0x28,
		AMF_XML				= 0x29,
		AMF_BYTEARRAY		= 0x30,
		AMF_TABLEENTRY		= 0x31,
		AMF_LIST			= 0x32,
		AMF_TRAITS			= 0x33,
	}AMFCTypeE;
};

using namespace AMFType;

///////////////////////////////////////////////////////////////////////////////
// Pre-declarations : 
///////////////////////////////////////////////////////////////////////////////

class AMFCodecCtx;
class AMFVar;
typedef TNodeList<AMFVar>			AMFVarList;
typedef std::shared_ptr<AMFVar>		AMFVarPtr;

template<typename T>
inline T *AMFCast(const AMFVarPtr &pVar)
{
	ASSERT(pVar);
	return static_cast<T *>(pVar.get());
}

///////////////////////////////////////////////////////////////////////////////
// class : ObjectEncoding
///////////////////////////////////////////////////////////////////////////////

class ObjectEncoding
{
public:
	static const uint32_t AMF0	= 0;
	static const uint32_t AMF3	= 3;
};

///////////////////////////////////////////////////////////////////////////////
// Class : AMFVarWrapper
///////////////////////////////////////////////////////////////////////////////

class RTMP_API AMFVarWrapper : public BCNodeList::Node
{
	DECLARE_FIXED_ALLOC(AMFVarWrapper);
public:
	AMFVarWrapper();
	AMFVarWrapper(uint32_t eType, bool bAMF3 = false);
	AMFVarWrapper(AMFVar *pVar);
	AMFVarWrapper(const AMFVarPtr &pVar);
	AMFVarWrapper(const AMFVarWrapper &other);
	virtual ~AMFVarWrapper(){}

	inline AMFVarWrapper & operator = (const AMFVarWrapper& other)
	{
		var = other.var;
		return *this;
	}

	template <typename T>
	inline T	*	Cast() const     //NOTHROW
	{
		return (T *)var.get();
	}
	inline operator AMFVarPtr()
	{
		return var;
	}
	virtual uint8_t			GetType() const;
	virtual void			Write(BCOStream* pOutput) const;
	virtual void			Read(BCIStream* pInput);
	virtual AMFVarWrapper *	Clone() const
	{
		return new AMFVarWrapper(var);
	}

	AMFVarPtr		var;
};

///////////////////////////////////////////////////////////////////////////////
// Class : AMFVarWrapperList
///////////////////////////////////////////////////////////////////////////////

class RTMP_API AMFVarWrapperList : public TNodeList<AMFVarWrapper>
{
public:
	AMFVarWrapperList(){}
	~AMFVarWrapperList()
	{
		Clear();
	}

	void			Clear();
	AMFVarWrapperList &operator = (const AMFVarWrapperList& other);
};

///////////////////////////////////////////////////////////////////////////////
// class : AMFCodexCtx
//
// References in AMF3
//
// References are more prominent in AMF3 than in AMF0, and one should have
// two or three arrays to keep track of them, one for strings, one for objects
// and possibly one for class definitions (some combine the second and third
// array). References are per-body, so the reference arrays should be reset
// every time a new body is encountered (this can happen if calls are batched
// or if a /onDebugEvents body is sent.
///////////////////////////////////////////////////////////////////////////////

class RTMP_API AMFCodecCtx
{
public:
	AMFCodecCtx(uint8_t eEncoding = ObjectEncoding::AMF0);
	~AMFCodecCtx();

	void			SetEncoding(uint8_t eEncoding = ObjectEncoding::AMF0);
	uint8_t			GetEncoding() const;
	void			SetOwner(void *pOwner)
	{
		m_pOwner = pOwner;
	}
	void		*	GetOwner() const
	{
		return m_pOwner;
	}
	bool			IsUseRef() const;
	AMFVarPtr		GetEncObject(uint32_t nIndex);
	int32_t			PushEncObject(const AMFVarPtr &pVar);
	int32_t			GetEncObjectIndex(const AMFVarPtr &pVar);
	AMFVarPtr		GetDecObject(uint32_t nIndex);
	int32_t			PushDecObject(const AMFVarPtr &pVar);
	AMFVarPtr		GetEncString(uint32_t nIndex);
	int32_t			PushEncString(const AMFVarPtr &pVar);
	int32_t			GetEncStringIndex(const AMFVarPtr &pVar);
	AMFVarPtr		GetDecString(uint32_t nIndex);
	int32_t			PushDecString(const AMFVarPtr &pVar);
	AMFVarPtr		GetEncClassDef(uint32_t nIndex);
	int32_t			PushEncClassDef(const AMFVarPtr &pVar);
	int32_t			GetEncClassDefIndex(const AMFVarPtr &pVar);
	AMFVarPtr		GetDecClassDef(uint32_t nIndex);
	int32_t			PushDecClassDef(const AMFVarPtr &pVar);
	void			Clear();
	int32_t			GetStringIndex(LPCSTR lpszStr);
	int32_t			PushDecString(LPCSTR lpszStr);
	int32_t			PushEncString(LPCSTR lpszStr);
protected:
	AMFVarPtr		_GetVar(LPCSTR lpszPrefix, uint32_t nIndex);
	int32_t			_PushVar(
						LPCSTR lpszPrefix, 
						uint32_t nIndex, 
						const AMFVarPtr &pVar);
	int32_t			_GetVar(
						LPCSTR lpszPrefix, 
						uint32_t nIndex, 
						const AMFVarPtr &pVar);
	int32_t			_SetStringIndex(LPCSTR lpszStr, int32_t nIndex);
	void			_Clear(LPCSTR lpszPrefix, uint32_t nIndex);
private:
	DECLARE_NO_COPY_CLASS(AMFCodecCtx);
	uint8_t				m_eEncoding;
	void			*	m_pOwner;
	bool				m_bUseRef;
	uint16_t			m_nEncObjectIdx;
	uint16_t			m_nDecObjectIdx;
	uint16_t			m_nEncStringIdx;
	uint16_t			m_nDecStringIdx;
	uint16_t			m_nEncClassDefIdx;
	uint16_t			m_nDecClassDefIdx;
	BCStrHashTable		m_htRefs;
	AMFVarWrapperList	m_lstStrings;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFVar - the base amf encoded variant class
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFVar
{
	friend class AMFCodecCtx;
public:
	virtual ~AMFVar(){}
	virtual uint8_t		GetType() const						= 0;
	virtual void		Write(BCOStream* pOutput) const		= 0;
	virtual void		Read(BCIStream* pInput)				= 0;
	virtual AMFVar *	Clone() const						= 0;
	virtual uint8_t		GetEncoding()
	{
		return ObjectEncoding::AMF0;
	}

	inline bool			IsNull() const
	{
		uint8_t eType = GetType();
		return (eType == AMFType::AMF0_NULL ||
			eType == AMFType::AMF3_NULL);
	}
	const char		*	GetTypeName() const;

protected:
	AMFVar(){}

private:
	DECLARE_NO_COPY_CLASS(AMFVar);
};

//////////////////////////////////////////////////////////////////////////
/// class AMFUndefined
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFUndefined : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFUndefined);
public:
	AMFUndefined();
	virtual ~AMFUndefined();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFUndefined &	operator = (const AMFUndefined &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMFNull
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFNull : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFNull);
public:
	AMFNull();
	virtual ~AMFNull();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFNull		&	operator = (const AMFNull &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMFBool
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFBool: public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFBool);
public:
	AMFBool(bool bValue = false);
	virtual ~AMFBool();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFBool		&	operator = (const AMFBool &other);
	void			SetValue(const bool val){ m_bValue = val; }
	bool			GetValue() const{ return m_bValue; }
private:
	bool			m_bValue;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFDouble
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFNumber : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFNumber);
public:
	typedef enum NumTypeE
	{
		NUM_TYPE_DOUBLE,
		NUM_TYPE_INT
	}NumTypeE;
public:
	AMFNumber(NumTypeE eNumType = NUM_TYPE_DOUBLE);
	AMFNumber(NumTypeE eNumType, uint32_t nValue);
	AMFNumber(NumTypeE eNumType, float64_t fValue);
	AMFNumber(const AMFNumber &other);
	virtual ~AMFNumber();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFNumber	&	operator = (const AMFNumber &other);
	void			SetDoubleValue(float64_t val)
	{
		m_uValue.fValue = val;
		m_eType = NUM_TYPE_DOUBLE;
	}
	void			SetIntValue(uint32_t val)
	{
		m_uValue.nValue = val;
		m_eType = NUM_TYPE_INT;
	}
	NumTypeE		GetValueType() const { return m_eType; }
	float64_t		GetDoubleValue() const{ return m_uValue.fValue; }
	uint32_t		GetIntValue() const{ return m_uValue.nValue; }
	void			SetDoubleValue(const uint8_t* octets)
	{
		memcpy2(&m_uValue.fValue,octets,8);
		m_eType = NUM_TYPE_DOUBLE;
	}
private:
	union{
		double		fValue;
		uint32_t	nValue;
	}				m_uValue;
	NumTypeE		m_eType;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFDate - An AMF Date is serialized as the number
///                 of milliseconds elapsed since the epoch of midnight
///                 on 1st Jan 1970 in the UTC time zone.
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFDate : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFDate);
public:
	AMFDate(float64_t fTime = 0);
	~AMFDate();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream *pInput);
	AMFVar		*	Clone() const;
	AMFDate		&	operator = (const AMFDate &other);

	void			SetValue(float64_t dblVal){ m_fDateTime = dblVal; }
	float64_t	&	GetValue(){ return m_fDateTime; }
private:
	float64_t		m_fDateTime;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFString : base amf string
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFString : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFString);
public:
	AMFString();
	AMFString(const BCPString &refValue);
	AMFString(LPCSTR szValue);
	virtual ~AMFString();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual AMFVar	*	Clone() const;
	AMFString		&	operator = (const AMFString &other);

	void				SetValue(const char *szVal){ m_strValue = szVal; }
	void				SetValue(const BCPString &val){ m_strValue = val; }
	BCPString		&	GetValue(){ return m_strValue; }
protected:
	BCPString			m_strValue;
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMFAVMPlus
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFAVMPlus : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFAVMPlus);
public:
	AMFAVMPlus();
	virtual ~AMFAVMPlus();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFAVMPlus	&	operator = (const AMFAVMPlus &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMFTableEntry - this class represents an AMF Table
///                       entry (a name m_fValue pair)
//////////////////////////////////////////////////////////////////////////

class AMFTableEntry;
class AMFTable;

class RTMP_API AMFTableEntry : public AMFVarWrapper
{
	DECLARE_FIXED_ALLOC(AMFTableEntry);
public:
	AMFTableEntry();
	AMFTableEntry(const AMFTableEntry &other);
	AMFTableEntry(LPCSTR lpszKey, const AMFVarPtr &pValue);
	virtual ~AMFTableEntry();

	AMFVarPtr			GetKey();
	AMFVarPtr			GetValue();

	void				SetKey(LPCSTR szKey);
	void				SetKey(const AMFVarPtr & key);
	void				SetValue(const AMFVarPtr & val);
	AMFVarPtr			ReplaceValue(AMFVarPtr val);

	uint8_t				GetType() const;
	void				Write(BCOStream* pOutput) const;
	void				Read(BCIStream* pInput);
	AMFVarWrapper	*	Clone() const;
	AMFTableEntry	&	operator = (const AMFTableEntry &other);

private:
	AMFVarPtr			m_pKey;
	AMFVarPtr			m_pValue;
};

typedef TNodeList<AMFTableEntry>	AMFTableEntryList;

//////////////////////////////////////////////////////////////////////////
/// class AMFTable - An AMFTable is a collection of name-m_fValue
///                  pairs (AMFTable)
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFTable : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFTable);
public:
	AMFTable();
	virtual ~AMFTable();

	AMFVarPtr			Get(LPCSTR szKey);
	AMFTableEntry	*	Begin() const;
	AMFTableEntry	*	Next(AMFTableEntry *pIter) const;
	AMFTableEntry	*	End() const;
	void				Put(LPCSTR szKey, const AMFVarPtr &var);
	void				PutBool(LPCSTR szKey, bool bValue);
	void				PutDouble(LPCSTR szKey, double dbValue);
	void				PutString(LPCSTR szKey, LPCSTR szValue);
	bool				IsContainsKey(LPCSTR szKey);
	void				Remove(LPCSTR szKey);
	void				Clear();
	size_t				Size() const;

	virtual uint8_t		GetType() const	= 0;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	AMFTable		&	operator = (const AMFTable &other);

protected:
	AMFTableEntryList		m_lstEntries;
};

//////////////////////////////////////////////////////////////////////////
/// class AMF0Object - An AMF0 Object is just a pair of utf8 names with
///                   amf variant values
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF0Object : public AMFTable
{
	DECLARE_FIXED_ALLOC(AMF0Object);
public:
	AMF0Object();
	virtual ~AMF0Object();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual AMFVar	*	Clone() const;
	AMF0Object		&	operator = (const AMF0Object &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMFECMAArray - An AMF ECMA Array is mostly same as an AMF Object,
///                      as well as a long m_fValue
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF0ECMAArray : public AMFTable
{
	DECLARE_FIXED_ALLOC(AMF0ECMAArray);
public:
	AMF0ECMAArray();
	virtual ~AMF0ECMAArray();

	// Push back variant
	void				PutOrdinal(const AMFVarPtr & pVar);
	void				PutOrdinalBool(bool bValue);
	void				PutOrdinalDouble(double dbValue);
	void				PutOrdinalString(LPCSTR szValue);
	// Get array size
	AMFVarPtr 			Get(uint32_t nIndex);
	size_t				Size() const;

	uint8_t				GetType() const;
	void				Write(BCOStream* pOutput) const;
	void				Read(BCIStream* pInput);
	AMFVar			*	Clone() const;
	AMF0ECMAArray	&	operator = (const AMF0ECMAArray &other);
private:
	AMFVarWrapperList		m_lstOrdinal;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFStrictArray - An AMF Strict Array is an amf object array with
///                        ordinal indices.
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF0SArray : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMF0SArray);
public:
	AMF0SArray();
	~AMF0SArray();

	// Get array size
	AMFVarPtr 			Get(uint32_t nIndex);
	size_t				Size() const;

	uint8_t				GetType() const;
	void				Write(BCOStream *pOutput) const;
	void				Read(BCIStream *pInput);
	AMFVar			*	Clone() const;
	AMF0SArray 		&	operator = (const AMF0SArray &other);

	AMFVarPtr			PopFront();
	AMFVarPtr			PopBack();
	uint32_t			Push(AMFVarPtr var);
	uint32_t			PushBool(bool bValue);
	uint32_t			PushDouble(double dbValue);
	uint32_t			PushString(LPCSTR szValue);
	uint32_t			Remove(uint32_t nIndex);
	void				Clear();
private:
	AMFVarWrapperList		m_lstVars;
};

//////////////////////////////////////////////////////////////////////////
/// class AMFSArray - An AMF Strict Array is an amf object array with
///                        ordinal indices.
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFSArray : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMFSArray);
public:
	AMFSArray();
	virtual ~AMFSArray();

	uint8_t			GetType() const;
	void			Write(BCOStream* pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMFSArray	&	operator = (const AMFSArray &other);

	AMFVarPtr		Get(uint32_t nIndex);
	AMFVarPtr		PopBack();
	uint32_t		Push(AMFVarPtr var);
	uint32_t		PushBool(bool bValue);
	uint32_t		PushDouble(double dbValue);
	uint32_t		PushString(LPCSTR szValue);
	uint32_t		Remove(uint32_t nIndex);
	void			Clear();
	uint32_t		Size() const;
private:
	AMFVarWrapperList		m_lstVars;
};

//////////////////////////////////////////////////////////////////////////
/// class AMF0XmlDoc - A xml UTF-8 long string
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF0XmlDoc : public AMFString
{
	DECLARE_FIXED_ALLOC(AMF0XmlDoc);
public:
	AMF0XmlDoc();
	~AMF0XmlDoc();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMF0XmlDoc	&	operator = (const AMF0XmlDoc &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMF0TObject
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF0TObject : public AMF0Object
{
	DECLARE_FIXED_ALLOC(AMF0TObject);
public:
	AMF0TObject();
	~AMF0TObject();

	uint8_t				GetType() const;
	void				Write(BCOStream* pOutput) const;
	void				Read(BCIStream* pInput);
	AMFVar			*	Clone() const;
	AMF0TObject		&	operator = (const AMF0TObject &other);

	AMFVarPtr			GetClsName() const{ return m_strClsName; }
private:
	AMFVarPtr			m_strClsName;
};

///////////////////////////////////////////////////////////////////////////////
// AMF3 Specific type
///////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
/// class AMF3XmlDoc - A xml UTF-8 long string
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF3XmlDoc : public AMFString
{
	DECLARE_FIXED_ALLOC(AMF3XmlDoc);
public:
	AMF3XmlDoc();
	~AMF3XmlDoc();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMF3XmlDoc	&	operator = (const AMF3XmlDoc &other);
private:
};

//////////////////////////////////////////////////////////////////////////
/// class AMF3Array - An AMF3 Array is mostly same as an AMF3 Object,
///                   as well as a long value
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF3Array : public AMFTable
{
	DECLARE_FIXED_ALLOC(AMF3Array);
public:
	AMF3Array();
	virtual ~AMF3Array();

	uint8_t				GetType() const;
	void				Write(BCOStream* pOutput) const;
	void				Read(BCIStream* pInput);
	AMFVar		*		Clone() const;
	uint8_t				GetEncoding()
	{
		return ObjectEncoding::AMF3;
	}
	AMF3Array	&		operator = (const AMF3Array &other);

	AMFVarPtr			Get(uint32_t nIndex);
	AMFVarWrapper	*	Begin() const;
	AMFVarWrapper	*	Next(AMFVarWrapper *pIter) const;
	AMFVarWrapper	*	End() const;
	AMFVarPtr			PopFront();
	AMFVarPtr			PopBack();
	uint32_t			Push(const AMFVarPtr &var);
	uint32_t			PushBool(bool bValue);
	uint32_t			PushDouble(double dbValue);
	uint32_t			PushString(LPCSTR szValue);
	uint32_t			Remove(uint32_t nIndex);
	void				Clear();
	uint32_t			Size() const;
private:
	AMFVarWrapperList		m_lstVars;
};

//////////////////////////////////////////////////////////////////////////
/// class AMF3Object - An AMF3 Object is just a pair of utf8 names with
///                   amf variant values
//////////////////////////////////////////////////////////////////////////

typedef enum AMF3ObjectTypeE
{
	AMFOBJ_REFERENCE	= 0,
	AMFOBJ_CLSDEFREF	= 1,
	AMFOBJ_STATIC		= 2,
	AMFOBJ_DYNAMIC		= 3,
	AMFOBJ_EXTERNAL		= 4
}AMF3ObjectTypeE;

class RTMP_API AMF3Traits : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMF3Traits);

	friend class AMF3Object;

public:
	AMF3Traits(AMF3ObjectTypeE eType = AMFOBJ_DYNAMIC);
	virtual ~AMF3Traits();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual void		Read(BCIStream* pInput, uint32_t nMask);
	virtual AMFVar  *	Clone() const;
	AMF3Traits		&	operator = (const AMF3Traits &other);
	void				Clear();
	inline uint32_t		Count() const
	{
		return m_lstMembers.Count();
	}
	int32_t				GetIndexByKey(LPCSTR lpszKey);
	inline uint8_t		GetObjectType() const
	{
		return m_eObjectType;
	}

	AMF3ObjectTypeE		m_eObjectType;
	AMFVarPtr			m_strClsName;
	AMFVarWrapperList	m_lstMembers;
};

class RTMP_API AMF3Object : public AMFTable
{
	DECLARE_FIXED_ALLOC(AMF3Object);
public:
	AMF3Object(AMF3ObjectTypeE eType = AMFOBJ_DYNAMIC);
	virtual ~AMF3Object();

	virtual uint8_t		GetType() const;
	virtual void		Write(BCOStream* pOutput) const;
	virtual void		Read(BCIStream* pInput);
	virtual AMFVar  *	Clone() const;
	virtual uint8_t		GetEncoding()
	{
		return ObjectEncoding::AMF3;
	}
	AMF3Object		&	operator = (const AMF3Object &other);
	AMFVarPtr			Get(LPCSTR szKey);

protected:
private:
	AMFVarPtr			m_pTraits;
	AMFVarWrapperList	m_lstValues;
};

//////////////////////////////////////////////////////////////////////////
/// class AMF3Xml - A xml UTF-8 string
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF3Xml : public AMFString
{
	DECLARE_FIXED_ALLOC(AMF3Xml);
public:
	AMF3Xml();
	~AMF3Xml();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream* pInput);
	AMFVar		*	Clone() const;
	AMF3Xml		&	operator = (const AMF3Xml &other);
private:
};

//////////////////////////////////////////////////////////////////////////
///	AMF3ByteArray
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMF3ByteArray : public AMFVar
{
	DECLARE_FIXED_ALLOC(AMF3ByteArray);
public:
	AMF3ByteArray();
	AMF3ByteArray(const AMF3ByteArray &lhs);
	~AMF3ByteArray();

	uint8_t			GetType() const;
	void			Write(BCOStream *pOutput) const;
	void			Read(BCIStream *pInput);
	AMFVar		*	Clone() const;
	uint8_t			GetEncoding()
	{
		return ObjectEncoding::AMF3;
	}
	AMF3ByteArray &	operator = (const AMF3ByteArray &other);

	void			Push(uint8_t value);
	void			Push(LPCVOID pData, uint32_t nCount);
	uint32_t		GetLength() const { return m_sBody.UsedLength(); }

	mutable BCBuffer		m_sBody;
};
//////////////////////////////////////////////////////////////////////////
///	AMFCodec - A Codec to Encode and Decode amf types into raw data
//////////////////////////////////////////////////////////////////////////

class RTMP_API AMFCodec
{
public:
	AMFCodec();
	~AMFCodec();

	// Read AMF object without object type
	static AMFVarPtr	Decode(BCIStream* pInput);
	static AMFVarPtr	GetReference(
							uint8_t eAmfType, 
							BCIStream* pInput,
							AMFCodecCtx *pCtx,
							long eEncoding);
	// Write AMF object with object type
	static void			Encode(BCOStream* pOutput, const AMFVarPtr &var);
	static AMFVar	*	CreateVar(uint8_t eAmfType, bool bAMF3 = false);
	static const char*	GetTypeName(uint8_t eAmfType, bool bAMF3 = false);
	static double		ParseDouble(const uint8_t* buf);
	static int8_t		GetVLIntegerSize(uint64_t nValue);
	static int32_t		EncodeVLUInt32(BCOStream *pOutput, uint32_t nValue);
	static int32_t		DecodeVLUInt32(
							BCIStream *pInput, 
							uint32_t *pValue,
							bool bPeek = false);
	static int32_t		EncodeVLUInt64(BCOStream *pOutput, uint64_t nValue);
	static int32_t		DecodeVLUInt64(BCIStream *pInput, uint64_t *pValue);
	static int32_t		DecodeString(BCIStream *pInput, AMFVarPtr &refStr);
	static int32_t		EncodeString(
							BCOStream *pOutput,
							const BCPString &refStr,
							bool bHasType = false);
	static int32_t		EncodeString(
							BCOStream *pOutput,
							const AMFVarPtr &pStr,
							bool bHasType = false);
	static void			Test();
private:
	DECLARE_NO_COPY_CLASS(AMFCodec);
};

///////////////////////////////////////////////////////////////////////////////
// namespace scope functions
///////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////
/// end of namespace AMF
//////////////////////////////////////////////////////////////////////////

} // End of namespace : AMF

#endif // RTMP_AMF_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : AMF.h
///////////////////////////////////////////////////////////////////////////////
