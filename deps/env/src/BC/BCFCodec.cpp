
///////////////////////////////////////////////////////////////////////////////
// File : BCFCodec.cpp
///////////////////////////////////////////////////////////////////////////////

#include <BC/ByteOrder.h>
#include <BC/BCMemPool.h>
#include <BC/BCFCodec.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define BCF_EMPTY_STRING		0x01

//////////////////////////////////////////////////////////////////////////
/// class BCFVar
//////////////////////////////////////////////////////////////////////////

const char* BCFVar::GetTypeName() const
{
	return BCFCodec::GetTypeName(GetType());
}
#ifdef HAVE_DUMP
const BCString BCFVar::toString() const
{
	std::ostrstream os;
	Dump(os);
	os<<std::ends;
	BCString aString = os.str();
	os.rdbuf()->freeze(0);
	return aString;
}
#endif // HAVE_DUMP

//////////////////////////////////////////////////////////////////////////
/// class BCFUndefined
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFUndefined, 8);

BCFUndefined::BCFUndefined()
{
}

BCFUndefined::~BCFUndefined()
{
}

uint8_t BCFUndefined::GetType() const
{
	return BCF_TYPE_UNDEFINED;
}

void BCFUndefined::Write(BCOStream* pOutput) const
{
	pOutput->WriteUInt8(BCF_TYPE_UNDEFINED);
}

void BCFUndefined::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

BCFVar *BCFUndefined::Clone() const
{
	BCFUndefined *pNull;

	pNull = new BCFUndefined();
	if (pNull)
	{
		*pNull = *this;
	}
	return pNull;
}

BCFUndefined &BCFUndefined::operator = (const BCFUndefined &other)
{
	UNUSED(other);
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFNull
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFNull, 8);

BCFNull::BCFNull()
{
}

BCFNull::~BCFNull()
{
}

uint8_t BCFNull::GetType() const
{
	return BCF_TYPE_NULL;
}

void BCFNull::Write(BCOStream* pOutput) const
{
	UNUSED(pOutput);
}

void BCFNull::Read(BCIStream* pInput)
{
	UNUSED(pInput);
}

BCFVar *BCFNull::Clone() const
{
	BCFNull *pNull;

	pNull = new BCFNull();
	if (pNull)
	{
		*pNull = *this;
	}
	return pNull;
}

BCFNull &BCFNull::operator = (const BCFNull &other)
{
	UNUSED(other);
	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFFalse
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFFalse, 8);

BCFFalse::BCFFalse()
{
	//
}

BCFFalse::~BCFFalse()
{
	//
}

uint8_t BCFFalse::GetType() const
{
	return BCF_TYPE_FALSE;
}

void BCFFalse::Write(BCOStream* pOutput) const
{
	// Nothing to do
	UNUSED(pOutput);
}

void BCFFalse::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

BCFVar *BCFFalse::Clone() const
{
	BCFFalse *pFalse;

	pFalse = new BCFFalse();
	if (pFalse)
	{
		*pFalse = *this;
	}
	return pFalse;
}

BCFFalse &BCFFalse::operator = (const BCFFalse &other)
{
	UNUSED(other);
	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFTrue
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFTrue, 8);

BCFTrue::BCFTrue()
{
	//
}

BCFTrue::~BCFTrue()
{
	//
}

uint8_t BCFTrue::GetType() const
{
	return BCF_TYPE_TRUE;
}

void BCFTrue::Write(BCOStream* pOutput) const
{
	// Nothing to do
	UNUSED(pOutput);
}

void BCFTrue::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

BCFVar *BCFTrue::Clone() const
{
	BCFTrue *pTrue;

	pTrue = new BCFTrue();
	if (pTrue)
	{
		*pTrue = *this;
	}
	return pTrue;
}

BCFTrue &BCFTrue::operator = (const BCFTrue &other)
{
	UNUSED(other);
	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFInt
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFInt, 8);

BCFInt::BCFInt(uint64_t nValue)
	: m_nValue(nValue)
{
	//
}

BCFInt::~BCFInt()
{
	//
}

uint8_t BCFInt::GetType() const
{
	//if (m_nValue < MAX_INT && (int32_t)m_nValue > MIN_INT) 
	//{
	//	return BCF_TYPE_INT;
	//}
	//else
	//{
	//	return BCF_TYPE_DOUBLE;
	//}
	return BCF_TYPE_INT;
}

void BCFInt::Write(BCOStream *pOutput) const
{
	pOutput->WriteUInt64BE(m_nValue);
}

void BCFInt::Read(BCIStream* pInput)
{
	pInput->ReadUInt64BE(&m_nValue);
}

BCFVar *BCFInt::Clone() const
{
	BCFInt *pInt;

	pInt = new BCFInt();
	if (pInt)
	{
		*pInt = *this;
	}
	return pInt;
}

BCFInt &BCFInt::operator = (const BCFInt &other)
{
	m_nValue = other.m_nValue;

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFDouble
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFDouble, 8);

BCFDouble::BCFDouble() 
	: m_fValue(0.0)
{
}

BCFDouble::~BCFDouble()
{
}

uint8_t BCFDouble::GetType() const
{
	return BCF_TYPE_DOUBLE;
}

void BCFDouble::Write(BCOStream* pOutput) const
{
	pOutput->WriteFloat64BE(m_fValue);
}

void BCFDouble::Read(BCIStream* pInput)
{
	m_fValue = 0;
	pInput->ReadFloat64BE(&m_fValue);
}

BCFVar *BCFDouble::Clone() const
{
	BCFDouble *pDBL;

	pDBL = new BCFDouble();
	if (pDBL)
	{
		pDBL->m_fValue = m_fValue;
	}
	return pDBL;
}

BCFDouble &BCFDouble::operator = (const BCFDouble &other)
{
	m_fValue = other.m_fValue;

	return *this;
}

#ifdef HAVE_DUMP
std::ostream& BCFDouble::Dump(std::ostream& os) const
{
	return os << m_fValue;
}
#endif

//////////////////////////////////////////////////////////////////////////
/// class BCFString
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFString, 8);

BCFString::BCFString()
{
}


BCFString::BCFString(const BCPString& other)
	: m_strValue(other)
{
}

BCFString::BCFString(const std::string& other)
	: m_strValue(other)
{
}

BCFString::~BCFString()
{
}

uint8_t BCFString::GetType() const
{
	return BCF_TYPE_STRING;
}

void BCFString::Write(BCOStream* pOutput) const
{
	uint32_t nLen;

	nLen = m_strValue.length();
	if (nLen > 0)
	{
		BCFCodec::EncodeVLInteger(pOutput, nLen);
		pOutput->WriteStringExact(m_strValue);
	}
}

void BCFString::Read(BCIStream* pInput)
{
	uint32_t len = 0;
	BCFCodec::DecodeVLInteger(pInput, &len);
	if (len > 0)
	{
		pInput->ReadStringExact(m_strValue,(size_t)len);
	}
}

BCFVar *BCFString::Clone() const
{
	BCFString *pStr;

	pStr = new BCFString();
	if (pStr)
	{
		pStr->m_strValue = m_strValue.c_str();
	}
	return pStr;
}

BCFString &BCFString::operator = (const BCFString &other)
{
	m_strValue = other.m_strValue.c_str();

	return *this;
}

BCFString &BCFString::operator = (const std::string &other)
{
	m_strValue = other;

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFXmlDoc
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFXmlDoc, 8);

BCFXmlDoc::BCFXmlDoc() 
{
}

BCFXmlDoc::~BCFXmlDoc()
{
}

uint8_t BCFXmlDoc::GetType() const
{
	return BCF_TYPE_XMLDOC;
}

void BCFXmlDoc::Write(BCOStream* pOutput) const
{
	BCFString::Write(pOutput);
}

void BCFXmlDoc::Read(BCIStream* pInput)
{
	BCFString::Read(pInput);
}

BCFVar *BCFXmlDoc::Clone() const
{
	BCFXmlDoc *pDoc;

	pDoc = new BCFXmlDoc();
	if (pDoc)
	{
		*pDoc = *this;
	}
	return pDoc;
}

BCFXmlDoc &BCFXmlDoc::operator = (const BCFXmlDoc &other)
{
	BCFString::operator =(other);

	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFDate
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFDate, 8);

BCFDate::BCFDate()
{
	//
}

BCFDate::~BCFDate()
{
	//
}

uint8_t BCFDate::GetType() const
{
	return BCF_TYPE_DATE;
}

void BCFDate::Write(BCOStream *pOutput) const
{
	ASSERT(m_nU29DValue < (uint32_t)MAX_INT && 
		m_nU29DValue > (uint32_t)MIN_INT);

	// Int is in valid BCF3 int range.
	BCFCodec::EncodeVLInteger(pOutput, m_nU29DValue);
	pOutput->WriteFloat64BE(m_fDateTime);
}

void BCFDate::Read(BCIStream* pInput)
{
	BCFCodec::DecodeVLInteger(pInput, &m_nU29DValue);
	pInput->ReadFloat64BE(&m_fDateTime);
}

BCFVar *BCFDate::Clone() const
{
	BCFDate *pDate;

	pDate = new BCFDate();
	if (pDate)
	{
		*pDate = *this;
	}
	return pDate;
}

BCFDate &BCFDate::operator = (const BCFDate &other)
{
	m_nU29DValue = other.m_nU29DValue;
	m_fDateTime = other.m_fDateTime;

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFTableEntry
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFTableEntry, 8);

BCFTableEntry::BCFTableEntry() 
	: m_pValue(0)
{
}

BCFTableEntry::~BCFTableEntry()
{
	BC_SAFE_DELETE_PTR(m_pValue);
}

BCPString& BCFTableEntry::GetKey()
{
	return m_strKey;
}

BCFVar* BCFTableEntry::GetValue()
{
	return m_pValue;
}

void BCFTableEntry::SetKey(LPCSTR keyStr)
{
	ASSERT(keyStr);
	m_strKey = keyStr;
}

void BCFTableEntry::SetKey(const std::string& keyStr)
{
	m_strKey = keyStr;
}

void BCFTableEntry::SetValue(BCFVar* val)
{
	//this shall delete the old value
	BC_SAFE_DELETE_PTR(m_pValue);
	m_pValue = val;
}


BC::BCFVar * BCFTableEntry::ReplaceValue(BCFVar *val)
{
	BCFVar *pOldVar = m_pValue;
	m_pValue = val;
	return pOldVar;
}

uint8_t BCFTableEntry::GetType() const
{
	return BCF_TYPE_TABLEENTRY;
}

void BCFTableEntry::Write(BCOStream* pOutput) const
{
	BCFCodec::EncodeString(pOutput, m_strKey);
	if(m_pValue)
	{
		BCFCodec::Encode(pOutput, m_pValue);
	}
	else
	{
		BCFNull nullValue;
		BCFCodec::Encode(pOutput, &nullValue);
	}
}

void BCFTableEntry::Read(BCIStream* pInput)
{
	BCFVar *pVar;

	BCFCodec::DecodeString(pInput, m_strKey);
	pVar = BCFCodec::Decode(pInput);
	ASSERT(pVar);
	SetValue(pVar);
}

BCFTableEntry *BCFTableEntry::Clone() const
{
	BCFTableEntry *pEntry;

	pEntry = new BCFTableEntry();
	if (pEntry)
	{
		*pEntry = *this;
	}
	return pEntry;
}

BCFTableEntry &BCFTableEntry::operator = (const BCFTableEntry &other)
{
	m_strKey = other.m_strKey;
	BC_SAFE_DELETE_PTR(m_pValue);
	if (other.m_pValue)
	{
		m_pValue = other.m_pValue->Clone();
	}
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFTable
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFTable, 8);

BCFTable::BCFTable()
{
}

BCFTable::~BCFTable()
{
	Clear();
}

BCFVar* BCFTable::Get(LPCSTR key)
{
	BCFTableEntry *pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(; pEntry != pEnd; pEntry = m_lstEntries.Next(pEntry))
	{
		if(pEntry->GetKey() == key)
		{
			return pEntry->GetValue();
		}
	}
	return 0;
}

BCFTableEntry *BCFTable::BeginEntry() const
{
	return m_lstEntries.Begin();
}

BCFTableEntry *BCFTable::NextEntry(BCFVar *pIter) const
{
	return m_lstEntries.Next(static_cast<BCFTableEntry *>(pIter));
}

BCFTableEntry *BCFTable::EndEntry() const
{
	return m_lstEntries.End();
}

void BCFTable::Put(LPCSTR key, BCFVar* var)
{
	BCFTableEntry *pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;pEntry = m_lstEntries.Next(pEntry))
	{
		if(pEntry->GetKey() == key)
		{
			pEntry->SetValue(var);
			return;
		}
	}

	// create a new entry and shove it into the list
	pEntry = new BCFTableEntry();
	pEntry->SetKey(key);
	pEntry->SetValue(var);
	m_lstEntries.PushBack(pEntry);
}

void BCFTable::PutBool(LPCSTR szKey, bool bValue)
{
	BCFVar *pVar = NULL;
	if (bValue)
	{
		pVar = new BCFTrue();
	} 
	else
	{
		pVar = new BCFFalse();
	}
	if (pVar)
	{
		Put(szKey, pVar);
	}
}

void  BCFTable::PutInt(LPCSTR szKey, uint64_t nValue)
{
	BCFInt *pVar = new BCFInt();
	if (pVar)
	{
		pVar->SetValue(nValue);
		Put(szKey, pVar);
	}
}

void  BCFTable::PutDouble(LPCSTR szKey, double dbValue)
{
	BCFDouble *pVar = new BCFDouble();
	if (pVar)
	{
		pVar->SetValue(dbValue);
		Put(szKey, pVar);
	}
}

void  BCFTable::PutString(LPCSTR szKey, LPCSTR szValue)
{
	BCFString *pVar = new BCFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Put(szKey, pVar);
	}
}

void  BCFTable::PutString(LPCSTR szKey, const std::string& szValue)
{
	BCFString *pVar = new BCFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Put(szKey, pVar);
	}
}

bool  BCFTable::IsContainsKey(LPCSTR key)
{
	return Get(key) != 0;
}

void BCFTable::Remove(LPCSTR szKey)
{
	BCFTableEntry *pEntry, *pNext, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;)
	{
		if(pEntry->GetKey() == szKey)
		{
			pNext = m_lstEntries.Next(pEntry);
			pEntry->RemoveFromList();
			BC_SAFE_DELETE_PTR(pEntry);
			pEntry = pNext;
		}
		else
		{
			pEntry = m_lstEntries.Next(pEntry);
		}
	}
}

void  BCFTable::Clear()
{
	BCFTableEntry *pEntry;

	while((pEntry = m_lstEntries.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pEntry);
	}
}

size_t BCFTable::Size() const
{
	return m_lstEntries.Count();
}

BCRESULT BCFTable::Qsort(int (* _PtFuncCompare)(LPCVOID, LPCVOID))
{
	uint32_t nCount = m_lstEntries.Count();
	if (nCount)
	{
		KBPool sPool;
		BCFTableEntry **lpArray = (BCFTableEntry **)sPool.Calloc(
			nCount*sizeof(BCFTableEntry *));
		for (uint32_t i = 0;i < nCount;i++)
		{
			lpArray[i] = m_lstEntries.PopFront();
		}
		qsort(lpArray, nCount, sizeof(BCFTableEntry *), _PtFuncCompare);
		for (uint32_t j = 0;j < nCount;j++)
		{
			m_lstEntries.PushBack(lpArray[j]);
		}
	}

	return BC_R_SUCCESS;
}

void BCFTable::Write(BCOStream* pOutput) const
{
	BCFTableEntry* pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;pEntry = m_lstEntries.Next(pEntry))
	{
		pEntry->Write(pOutput);
	}
	//write end of object
	// empty name string
	pOutput->WriteUInt8(BCF_EMPTY_STRING);
}

void BCFTable::Read(BCIStream* pInput)
{
	bool bObjFinished = false;
	uint8_t nByte;
	BCFTableEntry* pEntry;
	while(!pInput->Eof() && !bObjFinished)
	{
		pInput->PeekUInt8(&nByte);
		if(nByte == BCF_EMPTY_STRING)
		{
			pInput->ReadUInt8(&nByte);
			bObjFinished = true;
			break;
		}
		else
		{
			pEntry = new BCFTableEntry();
			ASSERT(pEntry);
			pEntry->Read(pInput);
			m_lstEntries.PushBack(pEntry);
		}
	}
}

BCFTable &BCFTable::operator = (const BCFTable &other)
{
	BCFTableEntry *pEntry, *pEnd;

	Clear();
	pEntry = other.m_lstEntries.Begin();
	pEnd = other.m_lstEntries.End();
	for (;pEntry != pEnd;pEntry = other.m_lstEntries.Next(pEntry))
	{
		m_lstEntries.PushBack(pEntry->Clone());
	}
	return *this;
}

BCFTable& BCFTable::operator +=(const BCFTable& other)
{
	BCFTableEntry* pEntry, * pEnd;

	pEntry = other.m_lstEntries.Begin();
	pEnd = other.m_lstEntries.End();
	for (; pEntry != pEnd; pEntry = other.m_lstEntries.Next(pEntry))
	{
		Put(pEntry->GetKey(), pEntry->GetValue()->Clone());
	}
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFArray, 8);

BCFArray::BCFArray()
{
	//
}

BCFArray::~BCFArray()
{
	Clear();
}

uint8_t BCFArray::GetType() const
{
	return BCF_TYPE_ARRAY;
}

void BCFArray::Write(BCOStream* pOutput) const
{
	uint32_t nSize;

	// Write type
	pOutput->WriteUInt8(BCF_TYPE_ARRAY);
	// Write array instance
	nSize = m_lstDenseVars.Count();
	BCFCodec::EncodeVLInteger(pOutput, nSize);
	// Write key-value pairs
	BCFTable::Write(pOutput);
	// Write dense values
	if (nSize)
	{
		BCFVar *pVar, *pEnd;

		pVar = m_lstDenseVars.Begin();
		pEnd = m_lstDenseVars.End();
		for (;pVar != pEnd;pVar = m_lstDenseVars.Next(pVar))
		{
			BCFCodec::Encode(pOutput, pVar);
		}
	}
}

void BCFArray::Read(BCIStream* pInput)
{
	uint32_t nSize;
	BCFVar *pVar;

	BCFCodec::DecodeVLInteger(pInput, &nSize);
	// Read associative name-value pairs
	BCFTable::Read(pInput);
	// Read dense values
	while((pVar = m_lstDenseVars.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pVar);
	}
	for (uint32_t i = 0;i < nSize;i++)
	{
		pVar = BCFCodec::Decode(pInput);
		ASSERT(pVar);
		m_lstDenseVars.PushBack(pVar);
	}
}

BCFVar *BCFArray::Clone() const
{
	BCFArray *pArray;

	pArray = new BCFArray();
	if (pArray)
	{
		*pArray = *this;
	}
	return pArray;
}

BCFArray &BCFArray::operator = (const BCFArray &other)
{
	BCFVar *pVar, *pEnd;

	BCFTable::operator =(other);
	Clear();
	pVar = other.m_lstDenseVars.Begin();
	pEnd = other.m_lstDenseVars.End();
	for (;pVar != pEnd;pVar = other.m_lstDenseVars.Next(pVar))
	{
		m_lstDenseVars.PushBack(pVar->Clone());
	}

	return *this;
}

BCFVar *BCFArray::Get(uint32_t nIndex)
{
	return m_lstDenseVars[nIndex];
}

BCFVar *BCFArray::Begin() const
{
	return m_lstDenseVars.Begin();
}

BCFVar *BCFArray::Next(BCFVar *pIter) const
{
	return m_lstDenseVars.Next(pIter);
}

BCFVar *BCFArray::End() const
{
	return m_lstDenseVars.End();
}

BCFVar *BCFArray::PopBack()
{
	return m_lstDenseVars.PopBack();
}

uint32_t BCFArray::Push(BCFVar* pVar)
{
	ASSERT(pVar);
	m_lstDenseVars.PushBack(pVar);
	return m_lstDenseVars.Count();
}

uint32_t BCFArray::PushBool(bool bValue)
{
	BCFVar *pVar;
	if (bValue)
	{
		pVar = new BCFTrue();
	}
	else
	{
		pVar = new BCFFalse();
	}
	if (pVar)
	{
		Push(pVar);
	}
	return m_lstDenseVars.Count();
}

uint32_t  BCFArray::PushInt(uint64_t nValue)
{
	BCFInt *pVar = new BCFInt();
	if (pVar)
	{
		pVar->SetValue(nValue);
		Push(pVar);
	}
	return m_lstDenseVars.Count();
}

uint32_t  BCFArray::PushDouble(double dbValue)
{
	BCFDouble *pVar = new BCFDouble();
	if (pVar)
	{
		pVar->SetValue(dbValue);
		Push(pVar);
	}
	return m_lstDenseVars.Count();
}

uint32_t  BCFArray::PushString(LPCSTR szValue)
{
	BCFString *pVar = new BCFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Push(pVar);
	}
	return m_lstDenseVars.Count();
}

uint32_t BCFArray::Remove(uint32_t nIndex)
{
	BCFVar *pVar;

	pVar = m_lstDenseVars[nIndex];
	if (pVar)
	{
		pVar->RemoveFromList();
		BC_SAFE_DELETE_PTR(pVar);
	}
	return m_lstDenseVars.Count();
}

void  BCFArray::Clear()
{
	BCFVar *pVar;

	while((pVar = m_lstDenseVars.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pVar);
	}
}

uint32_t BCFArray::Size() const
{
	return m_lstDenseVars.Count();
}

//////////////////////////////////////////////////////////////////////////
/// class BCFObject
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFObject, 8);

BCFObject::BCFObject()
{
}

BCFObject::~BCFObject()
{
}

uint8_t BCFObject::GetType() const
{
	return BCF_TYPE_OBJECT;
}

void BCFObject::Write(BCOStream* pOutput) const
{
	BCFTable::Write(pOutput);
}

void BCFObject::Read(BCIStream* pInput)
{
	BCFTable::Read(pInput);
}

BCFVar *BCFObject::Clone() const
{
	BCFObject *pObj;

	pObj = new BCFObject();
	if (pObj)
	{
		*pObj = *this;
	}
	return pObj;
}

BCFObject &BCFObject::operator = (const BCFObject &other)
{
	BCFTable::operator =(other);
	return *this;
}

BCFObject& BCFObject::operator    +=(const BCFObject& other)
{
	BCFTable::operator +=(other);
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class BCFXml
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFXml, 8);

BCFXml::BCFXml() 
{
}

BCFXml::~BCFXml()
{
}

uint8_t BCFXml::GetType() const
{
	return BCF_TYPE_XML;
}

void BCFXml::Write(BCOStream* pOutput) const
{
	BCFCodec::EncodeVLInteger(pOutput, m_strValue.length());
	pOutput->Write(m_strValue.sdata(), m_strValue.length());
}

void BCFXml::Read(BCIStream* pInput)
{
	BCFCodec::DecodeString(pInput, m_strValue);
}

BCFVar *BCFXml::Clone() const
{
	BCFXml *pXML;

	pXML = new BCFXml();
	if (pXML)
	{
		*pXML = *this;
	}
	return pXML;
}

BCFXml &BCFXml::operator = (const BCFXml &other)
{
	BCFString::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// BCFByteArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFByteArray, 8);

BCFByteArray::BCFByteArray()
{
	//
}

BCFByteArray::BCFByteArray(const BCFByteArray &lhs)
	: BCFVar()
{
	m_sBuffer = lhs.m_sBuffer;
}

BCFByteArray::~BCFByteArray()
{
	Clear();
}

uint8_t BCFByteArray::GetType() const
{
	return BCF_TYPE_BYTEARRAY;
}

void BCFByteArray::Write(BCOStream *pOutput) const
{
	void *pData;
	uint32_t nDataLen;

	BCFCodec::EncodeVLInteger(pOutput, m_sBuffer.UsedLength());
	m_sBuffer.Rewind();
	while((pData = m_sBuffer.ReadBlock(INFINITE, nDataLen)) != NULL)
	{
		pOutput->Write(pData, nDataLen);
	}
	m_sBuffer.Rewind();
}

void BCFByteArray::Read(BCIStream *pInput)
{
	uint32_t nDataLen, nBlockSize;
	void *pData;

	BCFCodec::DecodeVLInteger(pInput, &nDataLen);
	m_sBuffer.Reset(1);
	nBlockSize = nDataLen;
	while(nDataLen > 0 && (pData = m_sBuffer.GetWritableBlock(nBlockSize)) != NULL)
	{
		pInput->Read(pData, nBlockSize);
		m_sBuffer.UngetWritableBlock(nBlockSize);
		nDataLen -= nBlockSize;
	}
}

BCFVar *BCFByteArray::Clone() const
{
	BCFByteArray *pBArray;

	pBArray = new BCFByteArray();
	if (pBArray)
	{
		*pBArray = *this;
	}
	return pBArray;
}

BCFByteArray &BCFByteArray::operator = (const BCFByteArray &other)
{
	m_sBuffer = other.m_sBuffer;
	m_sBuffer.Rewind();
	return *this;
}

uint8_t BCFByteArray::operator[] (uint32_t index) const
{
	void *pData;

	pData = m_sBuffer.MapAddress(index);
	if (pData)
	{
		return *(uint8_t *)pData;
	}
	throw BC_R_UNEXPECTEDEND;
}

uint8_t& BCFByteArray::operator[] (uint32_t index)
{
	if (m_sBuffer.UsedLength() <= index)
	{
		m_sBuffer.Add(index+1 - m_sBuffer.UsedLength());
	}
	return *(uint8_t *)m_sBuffer.MapAddress(index);
}

void BCFByteArray::Push(uint8_t nByte)
{
	m_sBuffer.Write(&nByte, 1);
}

void BCFByteArray::Push(const uint8_t *data, uint32_t count)
{
	m_sBuffer.Write(data, count);
}

void BCFByteArray::SetLength(uint32_t newLength)
{
	if (newLength > m_sBuffer.UsedLength())
	{
		m_sBuffer.Add(newLength - m_sBuffer.UsedLength());
	}
}

void BCFByteArray::Clear()
{
	m_sBuffer.Reset(1);
}

//////////////////////////////////////////////////////////////////////////
/// BCFCodec
//////////////////////////////////////////////////////////////////////////

BCFCodec::BCFCodec()
{
	//
}

BCFCodec::~BCFCodec()
{
	//
}

BCFVar* BCFCodec::NewVariant(uint8_t eAmfType)
{
	BCFVar *pAmfType = NULL;
	switch(eAmfType)
	{
	case BCF_TYPE_UNDEFINED:
		pAmfType = new BCFUndefined();
		break;
	case BCF_TYPE_NULL:
		pAmfType = new BCFNull();
		break;
	case BCF_TYPE_FALSE:
		pAmfType = new BCFFalse();
		break;
	case BCF_TYPE_TRUE:
		pAmfType = new BCFTrue();
		break;
	case BCF_TYPE_INT:
		pAmfType = new BCFInt();
		break;
	case BCF_TYPE_DOUBLE:
		pAmfType = new BCFDouble();
		break;
	case BCF_TYPE_STRING:
		pAmfType = new BCFString();
		break;
	case BCF_TYPE_XMLDOC:
		pAmfType = new BCFXmlDoc();
		break;
	case BCF_TYPE_DATE:
		pAmfType = new BCFDate();
		break;
	case BCF_TYPE_ARRAY:
		pAmfType = new BCFArray();
		break;
	case BCF_TYPE_OBJECT:
		pAmfType = new BCFObject();
		break;
	case BCF_TYPE_XML:
		pAmfType = new BCFXml();
		break;
	case BCF_TYPE_BYTEARRAY:
		pAmfType = new BCFByteArray();
		break;
	}
	
	return pAmfType;
}

double BCFCodec::ParseDouble(const uint8_t* buf)
{
	float64_t val;
	
	memcpy(&val, buf, sizeof(float64_t));
	val = BC::ByteOrder::swapBytesDouble(val);
	return val;
}

BCFVar* BCFCodec::Decode(BCIStream* pInput)
{
	uint8_t amfType = BCF_TYPE_NULL;
	pInput->ReadUInt8(&amfType);
	BCFVar* var = NewVariant(amfType);
	if(var)
	{
		var->Read(pInput);
	}
	return var;
}

void BCFCodec::Encode(BCOStream* pOutput, const BCFVar* var)
{
	if(var)
	{
		pOutput->WriteUInt8(var->GetType());
		var->Write(pOutput);
	}
}

const char* BCFCodec::GetTypeName(uint8_t eAmfType)
{
	switch(eAmfType)
	{
	case BCF_TYPE_UNDEFINED:
		return "BCF::UNDEFINED";
	case BCF_TYPE_NULL:
		return "BCF::NULL";
	case BCF_TYPE_FALSE:
		return "BCF::FALSE";
	case BCF_TYPE_TRUE:
		return "BCF::TRUE";
	case BCF_TYPE_INT:
		return "BCF::INT";
	case BCF_TYPE_DOUBLE:
		return "BCF::DOUBLE";
	case BCF_TYPE_STRING:
		return "BCF::STRING";
	case BCF_TYPE_XMLDOC:
		return "BCF::XMLDOC";
	case BCF_TYPE_DATE:
		return "BCF::DATE";
	case BCF_TYPE_ARRAY:
		return "BCF::ARRAY";
	case BCF_TYPE_OBJECT:
		return "BCF::OBJECT";
	case BCF_TYPE_XML:
		return "BCF::XML";
	case BCF_TYPE_BYTEARRAY:
		return "BCF::BYTEARRAY";
	default:
		return "BCF::UNKNOWN";
	}
}

int32_t BCFCodec::EncodeVLInteger(BCOStream *pOutput, uint32_t nValue)
{
	uint8_t tmp[4];
	uint32_t tmp_size;

	/*
	* Int can be up to 4 bytes long.
	*
	* The first bit of the first 3 bytes
	* is set if another byte follows.
	*
	* The integer value is the last 7 bits from
	* the first 3 bytes and the 8 bits of the last byte
	* (29 bits).
	*
	* The int is negative if the 1st bit of the 29 int is set.
	*/
	nValue &= 0x1fffffff; // Ignore 1st 3 bits of 32 bit int, since we're encoding to 29 bit.
	if (nValue < 0x80) 
	{
		tmp_size = 1;
		tmp[0] = nValue;
	} 
	else if (nValue < 0x4000) 
	{
		tmp_size = 2;
		tmp[0] = (nValue >> 7 & 0x7f) | 0x80; // Shift bits by 7 to fill 1st byte and set next byte flag
		tmp[1] = nValue & 0x7f; // Shift bits by 7 to fill 2nd byte, leave next byte flag unset
	} 
	else if (nValue < 0x200000) 
	{
		tmp_size = 3;
		tmp[0] = (nValue >> 14 & 0x7f) | 0x80;
		tmp[1] = (nValue >> 7 & 0x7f) | 0x80;
		tmp[2] = nValue & 0x7f;
	} 
	else if (nValue < 0x40000000) 
	{
		tmp_size = 4;
		tmp[0] = (nValue >> 22 & 0x7f) | 0x80;
		tmp[1] = (nValue >> 15 & 0x7f) | 0x80;
		tmp[2] = (nValue >> 8 & 0x7f) | 0x80; // Shift bits by 8, since we can use all bits in the 4th byte
		tmp[3] = (nValue & 0xff);
	} 
	else 
	{
		// Int is too big to be encoded by BCF.:(
		return 0;        
	}

	return pOutput->Write(tmp, tmp_size);
}

int32_t BCFCodec::DecodeVLInteger(BCIStream *pInput, uint32_t *pValue)
{
	int32_t result = 0;
	uint32_t byte_cnt = 0;
	uint8_t byte;
	
	if (pInput->ReadUInt8(&byte) != 1)
		return 0;

	// If 0x80 is set, int includes the next byte, up to 4 total bytes
	while ((byte & 0x80) && (byte_cnt < 3)) 
	{
		result <<= 7;
		result |= byte & 0x7F;
		if (pInput->ReadUInt8(&byte) != 1)
			return 0;
		byte_cnt++;
	}

	// shift bits in last byte
	if (byte_cnt < 3) 
	{
		result <<= 7; // shift by 7, since the 1st bit is reserved for next byte flag
		result |= byte & 0x7F;
	} 
	else 
	{
		result <<= 8; // shift by 8, since no further bytes are possible and 1st bit is not used for flag.
		result |= byte & 0xff;
	}

	// Move sign bit, since we're converting 29bit->32bit
	if (result & 0x10000000) 
	{
		result -= 0x20000000;
	}

	*pValue = result;
	return byte_cnt + 1;
}

int32_t BCFCodec::DecodeString(BCIStream *pInput, BCPString &refStr)
{
	int32_t result = 0;
	uint32_t nLen;
	result = BCFCodec::DecodeVLInteger(pInput, &nLen);
	if (result <= 0)
	{
		return result;
	}
	refStr.clear();
	if (pInput->RemainingLength() < (size_t)nLen)
	{
		return 0;
	}
	pInput->Read(refStr.GetWriteBuffer(nLen), nLen);
	refStr.UngetWriteBuffer(nLen);

	return refStr.length();
}

int32_t BCFCodec::EncodeString(
	BCOStream *pOutput, 
	const BCPString &refStr,
	bool bHasType /*= false*/)
{
	uint32_t nLen;
	int32_t result = 0;
#ifdef USE_UTF8
	BCPString strUtf8;
#endif

	// Allow empty string
#ifdef USE_UTF8
	nLen = BCOEMToUtf8(refStr, strUtf8);
#else
	nLen = refStr.Len();
#endif

	if (bHasType)
	{
		result += pOutput->WriteUInt8(BCF_TYPE_STRING);
	}
	result += BCFCodec::EncodeVLInteger(pOutput, nLen);
	if (nLen)
	{
#ifdef USE_UTF8
		result += pOutput->WriteStringExact(strUtf8);
#else
		result += pOutput->WriteStringExact(refStr);
#endif
	}

	return result;
}

uint64_t GetBCFInt(BCFVar *pVar)
{
	if (IS_BCF_NUMBER(pVar))
	{
		if (IS_BCF_INT(pVar))
		{
			return ((BCFInt *)pVar)->GetValue();
		}
		else
		{
			return ((BCFDouble *)pVar)->GetValue();
		}
	} 
	else
	{
		throw -1;
	}
	return 0;
}

double GetBCFDouble(BCFVar *pVar)
{
	if (IS_BCF_NUMBER(pVar))
	{
		if (IS_BCF_INT(pVar))
		{
			return ((BCFInt *)pVar)->GetValue();
		}
		else
		{
			return ((BCFDouble *)pVar)->GetValue();
		}
	} 
	else
	{
		throw -1;
	}
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file : BCFCodec.cpp
///////////////////////////////////////////////////////////////////////////////
