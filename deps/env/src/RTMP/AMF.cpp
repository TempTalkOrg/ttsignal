///////////////////////////////////////////////////////////////////////////////
// file : AMF.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#include <BC/ByteOrder.h>
#include <BC/BCStrPtrLen.h>
#include <BC/BCPString.h>
#include <BC/BCStream.h>
#include <BC/BCLog.h>
#include <BC/BCException.h>
#include "AMF.h"

#ifdef _MSC_VER
#pragma warning(disable:4018)
#endif // _MSC_VER



//////////////////////////////////////////////////////////////////////////
/// namespace AMF
//////////////////////////////////////////////////////////////////////////

namespace AMF
{

#define THROW_FALSE(exp) {if(!(exp)){throw -1;}}

static inline uint32_t MakeReference(uint32_t nIndex, bool bRef)
{
	return ((nIndex << 1) | (bRef?0:0x01));
}

///////////////////////////////////////////////////////////////////////////////
// Class : AMFVarWrapper
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFVarWrapper, 32);

AMFVarWrapper::AMFVarWrapper()
{
	//
}

AMFVarWrapper::AMFVarWrapper(uint32_t eType, bool bAMF3 /* = false */)
{
	var = AMFVarPtr(AMFCodec::CreateVar(eType, bAMF3));
}

AMFVarWrapper::AMFVarWrapper(const AMFVarWrapper &other)
{
	var = other.var;
}

AMFVarWrapper::AMFVarWrapper( AMFVar *pVar )
	: var(pVar)
{
	ASSERT(var);
}

AMFVarWrapper::AMFVarWrapper( const AMFVarPtr &pVar )
	: var(pVar)
{
	ASSERT(var);
}

uint8_t AMFVarWrapper::GetType() const
{
	return var->GetType();
}

void AMFVarWrapper::Write(BCOStream* pOutput) const
{
	AMFCodec::Encode(pOutput, var);
}

void AMFVarWrapper::Read(BCIStream* pInput)
{
	var = AMFCodec::Decode(pInput);
}

///////////////////////////////////////////////////////////////////////////////
// Class : AMFVarWrapperList
///////////////////////////////////////////////////////////////////////////////

void AMFVarWrapperList::Clear()
{
	AMFVarWrapper *pVar;

	while((pVar = PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pVar);
	}
}

AMFVarWrapperList &AMFVarWrapperList::operator = (const AMFVarWrapperList &other)
{
	Clear();

	AMFVarWrapper *pVar = other.Begin(), *pEnd = other.End();
	for (;pVar != pEnd;pVar = other.Next(pVar))
	{
		PushBack(pVar->Clone());
	}

	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : AMFCodecCtx
///////////////////////////////////////////////////////////////////////////////

static const BCStrPtrLen	kENCOBJ("ENCOBJ");
static const BCStrPtrLen	kDECOBJ("DECOBJ");
static const BCStrPtrLen	kENCSTR("ENCSTR");
static const BCStrPtrLen	kDECSTR("DECSTR");
static const BCStrPtrLen	kENCCLS("ENCCLS");
static const BCStrPtrLen	kDECCLS("DECCLS");
static const BCStrPtrLen	kSTRING("STRING");

AMFCodecCtx::AMFCodecCtx(uint8_t eEncoding)
	: m_eEncoding(eEncoding)
	, m_pOwner(NULL)
	, m_bUseRef(false)
	, m_nEncObjectIdx(0)
	, m_nDecObjectIdx(0)
	, m_nEncStringIdx(0)
	, m_nDecStringIdx(0)
	, m_nEncClassDefIdx(0)
	, m_nDecClassDefIdx(0)
{
	//
}

AMFCodecCtx::~AMFCodecCtx()
{
	Clear();
}

void AMFCodecCtx::SetEncoding(uint8_t eEncoding /* = ObjectEncoding::AMF0 */)
{
	m_eEncoding = eEncoding;
}

uint8_t AMFCodecCtx::GetEncoding() const
{
	return m_eEncoding;
}

bool AMFCodecCtx::IsUseRef() const
{
	return m_bUseRef;
}

AMFVarPtr AMFCodecCtx::GetEncObject(uint32_t nIndex)
{
	return _GetVar(kENCOBJ.Ptr, nIndex);
}

int32_t AMFCodecCtx::PushEncObject(const AMFVarPtr &pVar)
{
	return _PushVar(kENCOBJ.Ptr, m_nEncObjectIdx++, pVar);
}

int32_t AMFCodecCtx::GetEncObjectIndex(const AMFVarPtr &pVar)
{
	return _GetVar(kENCOBJ.Ptr, m_nEncObjectIdx, pVar);
}

AMFVarPtr AMFCodecCtx::GetDecObject(uint32_t nIndex)
{
	return _GetVar(kDECOBJ.Ptr, nIndex);
}

int32_t AMFCodecCtx::PushDecObject(const AMFVarPtr &pVar)
{
	return _PushVar(kDECOBJ.Ptr, m_nDecObjectIdx++, pVar);
}

AMFVarPtr AMFCodecCtx::GetEncString(uint32_t nIndex)
{
	return _GetVar(kENCSTR.Ptr, nIndex);
}

int32_t AMFCodecCtx::GetEncStringIndex(const AMFVarPtr &pVar)
{
	return _GetVar(kENCSTR.Ptr, m_nEncStringIdx, pVar);
}

int32_t AMFCodecCtx::PushEncString(const AMFVarPtr &pVar)
{
	if (pVar && pVar->GetType() == AMF_STRING)
	{
		BCPString &refStr = ((AMFString *)pVar.get())->GetValue();
		int32_t nIndex = GetStringIndex(refStr);
		if (nIndex < 0)
		{
			nIndex = _PushVar(kENCSTR.Ptr, m_nEncStringIdx++, pVar);
			_SetStringIndex(refStr, nIndex);
		}
		return nIndex;
	}
	return -1;
}

AMFVarPtr AMFCodecCtx::GetDecString(uint32_t nIndex)
{
	return _GetVar(kDECSTR.Ptr, nIndex);
}

int32_t AMFCodecCtx::PushDecString(const AMFVarPtr &pVar)
{
	return _PushVar(kDECSTR.Ptr, m_nDecStringIdx++, pVar);
}

AMFVarPtr AMFCodecCtx::GetEncClassDef(uint32_t nIndex)
{
	return _GetVar(kENCCLS.Ptr, nIndex);
}

int32_t AMFCodecCtx::PushEncClassDef(const AMFVarPtr &pVar)
{
	return _PushVar(kENCCLS.Ptr, m_nEncClassDefIdx++, pVar);
}

int32_t AMFCodecCtx::GetEncClassDefIndex(const AMFVarPtr &pVar)
{
	return _GetVar(kENCCLS.Ptr, m_nEncClassDefIdx, pVar);
}

AMFVarPtr AMFCodecCtx::GetDecClassDef(uint32_t nIndex)
{
	return _GetVar(kDECCLS.Ptr, nIndex);
}

int32_t AMFCodecCtx::PushDecClassDef(const AMFVarPtr &pVar)
{
	return _PushVar(kDECCLS.Ptr, m_nDecClassDefIdx++, pVar);
}

void AMFCodecCtx::Clear()
{
	_Clear(kENCOBJ.Ptr, m_nEncObjectIdx);
	_Clear(kDECOBJ.Ptr, m_nDecObjectIdx);
	_Clear(kENCSTR.Ptr, m_nEncStringIdx);
	_Clear(kDECSTR.Ptr, m_nDecStringIdx);
	_Clear(kENCCLS.Ptr, m_nEncClassDefIdx);
	_Clear(kDECCLS.Ptr, m_nDecClassDefIdx);
	m_nEncObjectIdx = 0;
	m_nDecObjectIdx = 0;
	m_nEncStringIdx = 0;
	m_nDecStringIdx = 0;
	m_nEncClassDefIdx = 0;
	m_nDecClassDefIdx = 0;
	m_lstStrings.Clear();
	m_htRefs.Clear();
}

int32_t AMFCodecCtx::_SetStringIndex( LPCSTR lpszStr, int32_t nIndex )
{
	BCPString strFmt;

	strFmt.Format("%s_%s", kSTRING.Ptr, lpszStr);
	m_htRefs[strFmt] = (LPVOID)(int64_t)nIndex;
	return nIndex;
}

int32_t AMFCodecCtx::PushDecString(LPCSTR lpszStr)
{
	int32_t nIndex = GetStringIndex(lpszStr);
	if (0 > nIndex)
	{
		AMFVarWrapper *pStr = new AMFVarWrapper(AMF_STRING);
		pStr->Cast<AMFString>()->GetValue() = lpszStr;
		m_lstStrings.PushBack(pStr);
		return PushDecString(pStr->var);
	} 
	else
	{
		return nIndex;
	}
}

int32_t AMFCodecCtx::PushEncString(LPCSTR lpszStr)
{
	int32_t nIndex = GetStringIndex(lpszStr);
	if (0 > nIndex)
	{
		AMFVarWrapper *pStr = new AMFVarWrapper(AMF_STRING);
		pStr->Cast<AMFString>()->GetValue() = lpszStr;
		m_lstStrings.PushBack(pStr);
		return PushEncString(pStr->var);
	} 
	else
	{
		return nIndex;
	}
}

AMFVarPtr AMFCodecCtx::_GetVar(LPCSTR lpszPrefix, uint32_t nIndex)
{
	char szIndex[32];

	memzero(szIndex, sizeof(szIndex));
	snprintf(szIndex, sizeof(szIndex), "%s_%" _U32BITARG_, lpszPrefix, 
		nIndex);
	return ((AMFVarWrapper *)m_htRefs[szIndex])->var;
}

int32_t AMFCodecCtx::_PushVar(
	LPCSTR lpszPrefix, 
	uint32_t nIndex, 
	const AMFVarPtr &pVar)
{
	if (pVar)
	{
		char szIndex[32];

		memzero(szIndex, sizeof(szIndex));
		snprintf(szIndex, sizeof(szIndex), "%s_%" _U32BITARG_, lpszPrefix, 
			nIndex);
		m_htRefs[szIndex] = new AMFVarWrapper(pVar);
		return nIndex;
	}
	return -1;
}

int32_t AMFCodecCtx::_GetVar(
	LPCSTR lpszPrefix, 
	uint32_t nIndexLimit, 
	const AMFVarPtr &refVar)
{
	if (refVar)
	{
		AMFVarWrapper *pVar;
		char szIndex[32];
		for (uint32_t i = 0;i < nIndexLimit;i++)
		{
			memzero(szIndex, sizeof(szIndex));
			snprintf(szIndex, sizeof(szIndex), "%s_%" _U32BITARG_, lpszPrefix, i);
			pVar = (AMFVarWrapper *)m_htRefs[szIndex];
			if (pVar && pVar->var == refVar)
			{
				return i;
			}
		}
	}
	return -1;
}

int32_t AMFCodecCtx::GetStringIndex( LPCSTR lpszStr )
{
	BCPString strFmt;
	LPVOID lpData;

	strFmt.Format("%s_%s", kSTRING.Ptr, lpszStr);
	if (!m_htRefs.Find(&lpData, strFmt))
	{
		return -1;
	}
	return (int64_t)lpData;
}

void AMFCodecCtx::_Clear(LPCSTR lpszPrefix, uint32_t nIndex)
{
	AMFVarWrapper *pWrapper;
	char szIndex[32];
	for (uint32_t i = 0;i < nIndex;i++)
	{
		memzero(szIndex, sizeof(szIndex));
		snprintf(szIndex, sizeof(szIndex), "%s_%" _U32BITARG_, lpszPrefix, i);
		pWrapper = (AMFVarWrapper *)m_htRefs[szIndex];
		BC_SAFE_DELETE_PTR(pWrapper);
		m_htRefs.Erase(szIndex);
	}
}

//////////////////////////////////////////////////////////////////////////
/// class AMFVar
//////////////////////////////////////////////////////////////////////////

const char* AMFVar::GetTypeName() const
{
	return AMFCodec::GetTypeName(GetType());
}

//////////////////////////////////////////////////////////////////////////
/// class AMFUndefined
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFUndefined, 8);

AMFUndefined::AMFUndefined()
{
}

AMFUndefined::~AMFUndefined()
{
}

uint8_t AMFUndefined::GetType() const
{
	return AMF_UNDEFINED;
}

void AMFUndefined::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_UNDEFINED);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		pOutput->WriteUInt8(AMF3_UNDEFINED);
	}
	else
	{
		ASSERT(0);
	}
}

void AMFUndefined::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

AMFVar *AMFUndefined::Clone() const
{
	return (new AMFUndefined());
}

AMFUndefined &AMFUndefined::operator = (const AMFUndefined &other)
{
	UNUSED(other);
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFNull
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFNull, 8);

AMFNull::AMFNull()
{
}

AMFNull::~AMFNull()
{
}

uint8_t AMFNull::GetType() const
{
	return AMF_NULL;
}

void AMFNull::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_NULL);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		pOutput->WriteUInt8(AMF3_NULL);
	}
	else
	{
		ASSERT(0);
	}
}

void AMFNull::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

AMFVar *AMFNull::Clone() const
{
	return (new AMFNull());
}

AMFNull &AMFNull::operator = (const AMFNull &other)
{
	UNUSED(other);
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFBool
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFBool, 8);

AMFBool::AMFBool(bool bValue) 
	: m_bValue(bValue)
{
}

AMFBool::~AMFBool()
{
}

uint8_t AMFBool::GetType() const
{
	return AMF_BOOL;
}

void AMFBool::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_BOOL);
		pOutput->WriteUInt8(m_bValue?1:0);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		if (m_bValue)
		{
			pOutput->WriteUInt8(AMF3_TRUE);
		} 
		else
		{
			pOutput->WriteUInt8(AMF3_FALSE);
		}
	} 
	else
	{
		ASSERT(0);
	}
}

void AMFBool::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		uint8_t nByteValue = 0;
		pInput->ReadUInt8(&nByteValue);
		SetValue(nByteValue > 0);
	}
	else
	{
		THROW_FALSE(0);
	}
}

AMFVar *AMFBool::Clone() const
{
	return (new AMFBool(m_bValue));
}

AMFBool &AMFBool::operator = (const AMFBool &other)
{
	m_bValue = other.m_bValue;
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFNumber
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFNumber, 8);

AMFNumber::AMFNumber(NumTypeE eNumType /*= NUM_TYPE_DOUBLE*/)
	: m_eType(eNumType)
{
	m_uValue.fValue = 0.0;
}

AMFNumber::AMFNumber(const AMFNumber &other)
	: m_eType(other.m_eType)
{
	if (m_eType == NUM_TYPE_DOUBLE)
	{
		m_uValue.fValue = other.m_uValue.fValue;
	} 
	else
	{
		m_uValue.nValue = other.m_uValue.nValue;
	}
}

AMFNumber::AMFNumber(NumTypeE eNumType, uint32_t nValue)
	: m_eType(eNumType)
{
	m_uValue.nValue = nValue;
}

AMFNumber::AMFNumber(NumTypeE eNumType, float64_t fValue)
	: m_eType(eNumType)
{
	m_uValue.fValue = fValue;
}

AMFNumber::~AMFNumber()
{
}

uint8_t AMFNumber::GetType() const
{
	return AMF_NUMBER;
}

void AMFNumber::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_DOUBLE);
		if (m_eType == NUM_TYPE_DOUBLE)
		{
			pOutput->WriteFloat64BE(m_uValue.fValue);
		}
		else
		{
			pOutput->WriteFloat64BE(m_uValue.nValue);
		}
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		if (m_eType == NUM_TYPE_DOUBLE)
		{
			// Write type
			pOutput->WriteUInt8(AMF3_DOUBLE);
			// Write value
			pOutput->WriteFloat64BE(m_uValue.fValue);
		} 
		else
		{
			float64_t fValue;

			fValue = m_uValue.nValue;
			if (fValue < MAX_INT && fValue > MIN_INT) 
			{
				// Write type
				pOutput->WriteUInt8(AMF3_INT);
				// Write value
				AMFCodec::EncodeVLUInt32(pOutput, m_uValue.nValue);
			}
			else
			{
				// Write type
				pOutput->WriteUInt8(AMF3_DOUBLE);
				// Write value
				pOutput->WriteFloat64BE(m_uValue.nValue);
			}
		}
	} 
	else
	{
		ASSERT(0);
	}
}

void AMFNumber::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		ASSERT(m_eType == NUM_TYPE_DOUBLE);
		m_uValue.fValue = 0;
		pInput->ReadFloat64BE(&m_uValue.fValue);
		m_eType = NUM_TYPE_DOUBLE;
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		if (m_eType == NUM_TYPE_DOUBLE)
		{
			pInput->ReadFloat64BE(&m_uValue.fValue);
		}
		else
		{
			AMFCodec::DecodeVLUInt32(pInput, &m_uValue.nValue);
		}
	}
	else
	{
		THROW_FALSE(0);
	}
}

AMFVar *AMFNumber::Clone() const
{
	return (new AMFNumber(*this));
}

AMFNumber &AMFNumber::operator = (const AMFNumber &other)
{
	m_eType = other.m_eType;
	m_uValue = other.m_uValue;
	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : AMFDate
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFDate, 8);

AMFDate::AMFDate(float64_t fTime) : m_fDateTime(fTime)
{
	//
}

AMFDate::~AMFDate()
{
	//
}

uint8_t AMFDate::GetType() const
{
	return AMF_DATE;
}

void AMFDate::Write(BCOStream *pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		// Write type
		pOutput->WriteUInt8(AMF0_DATE);
		// Write time zone
		pOutput->WriteUInt16BE(0);
		pOutput->WriteFloat64BE(m_fDateTime);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;

		// Write type
		pOutput->WriteUInt8(AMF3_DATE);
		// Write date instance
		nMask = MakeReference(0, false);
		AMFCodec::EncodeVLUInt32(pOutput, nMask);
		// Write date value
		pOutput->WriteFloat64BE(m_fDateTime);
		// We commented below code to ensure same-source reference
		//// Add to encode reference table
		//pCodecCtx->PushEncDate(AMFVarPtr(const_cast<AMFDate *>(this)));
	}
	else
	{
		ASSERT(0);
	}
}

void AMFDate::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		uint16_t nTimeZone;

		pInput->ReadUInt16BE(&nTimeZone);
		pInput->ReadFloat64BE(&m_fDateTime);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;

		AMFCodec::DecodeVLUInt32(pInput, &nMask);
		// Not reference
		ASSERT(!IsReference(nMask));
		// Read date time
		pInput->ReadFloat64BE(&m_fDateTime);
	}
	else
	{
		THROW_FALSE(0);
	}
}

AMFVar *AMFDate::Clone() const
{
	return (new AMFDate(m_fDateTime));
}

AMFDate &AMFDate::operator = (const AMFDate &other)
{
	m_fDateTime = other.m_fDateTime;
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFString
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFString, 8);

AMFString::AMFString()
{
	//
}

AMFString::AMFString(const BCPString &refValue)
	: m_strValue(refValue.c_str())
{
	//
}

AMFString::AMFString(LPCSTR szValue)
	: m_strValue(szValue)
{
	//
}

AMFString::~AMFString()
{
	//
}

uint8_t AMFString::GetType() const
{
	return AMF_STRING;
}

void AMFString::Write(BCOStream* pOutput) const
{
	//AMFCodec::EncodeString(pOutput, m_strValue, true);
	uint32_t nLen;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
#ifdef USE_UTF8
	BCPString strUtf8;
#endif

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	// Allow empty string
#ifdef USE_UTF8
	nLen = BCOEMToUtf8(m_strValue, strUtf8);
#else
	nLen = m_strValue.Len();
#endif
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;

		// Write type
		pOutput->WriteUInt8(AMF3_STRING);
		// Write string instance
		nMask = MakeReference(nLen, false);
		AMFCodec::EncodeVLUInt32(pOutput, nMask);
		if (nLen)
		{
#ifdef USE_UTF8
			pOutput->WriteStringExact(strUtf8);
#else
			pOutput->WriteStringExact(m_strValue);
#endif
		}
	} 
	else
	{
		if (nLen < 0xFFFF)
		{
			pOutput->WriteUInt8(AMF0_STRING);
			// UTF8 String
			pOutput->WriteUInt16BE(nLen);
#ifdef USE_UTF8
			pOutput->WriteStringExact(strUtf8);
#else
			pOutput->WriteStringExact(m_strValue);
#endif
		} 
		else
		{
			pOutput->WriteUInt8(AMF0_LSTRING);
			// UTF8 Long String
			pOutput->WriteUInt32BE(nLen);
#ifdef USE_UTF8
			pOutput->WriteStringExact(strUtf8);
#else
			pOutput->WriteStringExact(m_strValue);
#endif
		}
	}
}

void AMFString::Read(BCIStream* pInput)
{
	int32_t result = 0;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask, nLen;
		result = AMFCodec::DecodeVLUInt32(pInput, &nMask);
		if (result <= 0)
		{
			THROW_FALSE(0);
			return;
		}
		if (nMask == EMPTY_STRING_TYPE)
		{
			return;
		}
		// Not reference
		// Read string value
		m_strValue.clear();
		nLen = nMask >> 1;
		if (nLen == 0)
		{
			return;
		}
		if (pInput->RemainingLength() < nLen)
		{
			THROW_FALSE(0);
			return;
		}
		pInput->Read(m_strValue.GetWriteBuffer(nLen), nLen);
		m_strValue.UngetWriteBuffer(nLen);
#ifdef USE_UTF8
		BCUtf8ToOEM(m_strValue, m_strValue);
#endif
		// We commented below code to ensure same-source reference
		//pCodecCtx->PushDecString(AMFVarPtr(const_cast<AMFString *>(this)));
	}
	else
	{
		uint16_t nLen;
		result = pInput->ReadUInt16BE(&nLen);
		if (result < 2)
		{
			THROW_FALSE(0);
			return;
		}
		m_strValue.clear();
		if (pInput->RemainingLength() < nLen)
		{
			THROW_FALSE(0);
			return;
		}
		pInput->Read(m_strValue.GetWriteBuffer(nLen), nLen);
		m_strValue.UngetWriteBuffer(nLen);
	}
}

AMFVar *AMFString::Clone() const
{
	return (new AMFString(m_strValue));
}

AMFString &AMFString::operator = (const AMFString &other)
{
	m_strValue = other.m_strValue.c_str();
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFAVMPlus
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFAVMPlus, 8);

AMFAVMPlus::AMFAVMPlus()
{
}

AMFAVMPlus::~AMFAVMPlus()
{
}

uint8_t AMFAVMPlus::GetType() const
{
	return AMF0_AVMPLUS;
}

void AMFAVMPlus::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_AVMPLUS);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		ASSERT(0);
	}
}

void AMFAVMPlus::Read(BCIStream* pInput)
{
	// Nothing to do
	UNUSED(pInput);
}

AMFVar *AMFAVMPlus::Clone() const
{
	return (new AMFAVMPlus());
}

AMFAVMPlus &AMFAVMPlus::operator = (const AMFAVMPlus &other)
{
	UNUSED(other);
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFTableEntry
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFTableEntry, 8);

AMFTableEntry::AMFTableEntry() 
{
	//
}

AMFTableEntry::AMFTableEntry(const AMFTableEntry &other)
	: AMFVarWrapper(other)
	, m_pKey(other.m_pKey->Clone())
	, m_pValue(other.m_pValue->Clone())
{
	//
}

AMFTableEntry::AMFTableEntry(LPCSTR lpszKey, const AMFVarPtr &pValue)
	: m_pKey(new AMFString(lpszKey))
	, m_pValue(pValue->Clone())
{
	//
}



AMFTableEntry::~AMFTableEntry()
{
	//
}

AMFVarPtr AMFTableEntry::GetKey()
{
	return m_pKey;
}

AMFVarPtr AMFTableEntry::GetValue()
{
	return m_pValue;
}

void AMFTableEntry::SetKey(LPCSTR szKey)
{
	ASSERT(szKey);
	m_pKey.reset(new AMFString(szKey));
}

void AMFTableEntry::SetKey(const AMFVarPtr &key)
{
	m_pKey = key;
}

void AMFTableEntry::SetValue(const AMFVarPtr & val)
{
	//this shall delete the old value
	m_pValue = val;
}

AMFVarPtr AMFTableEntry::ReplaceValue(AMFVarPtr val)
{
	AMFVarPtr pOldVal = m_pValue;
	m_pValue = val;
	return pOldVal;
}

uint8_t AMFTableEntry::GetType() const
{
	return AMF_TABLEENTRY;
}

void AMFTableEntry::Write(BCOStream* pOutput) const
{
	AMFCodec::EncodeString(pOutput, m_pKey);
	if(m_pValue)
	{
		AMFCodec::Encode(pOutput, m_pValue);
	}
	else
	{
		AMFVarPtr pNull(new AMFNull());
		AMFCodec::Encode(pOutput, pNull);
	}
}

void AMFTableEntry::Read(BCIStream* pInput)
{
	AMFVarPtr pVar;

	AMFCodec::DecodeString(pInput, m_pKey);
	pVar = AMFCodec::Decode(pInput);
	if (!pVar)
	{
		throw BC_R_UNEXPECTED;
	}
	SetValue(pVar);
}

AMFVarWrapper *AMFTableEntry::Clone() const
{
	return (new AMFTableEntry(*this));
}

AMFTableEntry &AMFTableEntry::operator = (const AMFTableEntry &other)
{
	m_pKey.reset(other.m_pKey->Clone());
	m_pValue.reset(other.m_pValue->Clone());
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFTable
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFTable, 8);

AMFTable::AMFTable()
{
}

AMFTable::~AMFTable()
{
	Clear();
}

AMFVarPtr AMFTable::Get(LPCSTR key)
{
	AMFTableEntry *pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(; pEntry != pEnd; pEntry = m_lstEntries.Next(pEntry))
	{
		if(AMFCast<AMFString>(pEntry->GetKey())->GetValue() == key)
		{
			return pEntry->GetValue();
		}
	}
	return 0;
}

AMFTableEntry *AMFTable::Begin() const
{
	return m_lstEntries.Begin();
}

AMFTableEntry *AMFTable::Next(AMFTableEntry *pIter) const
{
	return m_lstEntries.Next(pIter);
}

AMFTableEntry *AMFTable::End() const
{
	return m_lstEntries.End();
}

void AMFTable::Put(LPCSTR key, const AMFVarPtr &var)
{
	AMFTableEntry *pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;pEntry = m_lstEntries.Next(pEntry))
	{
		if(AMFCast<AMFString>(pEntry->GetKey())->GetValue() == key)
		{
			pEntry->SetValue(var);
			return;
		}
	}

	// create a new entry and shove it into the list
	pEntry = new AMFTableEntry();
	pEntry->SetKey(key);
	pEntry->SetValue(var);
	m_lstEntries.PushBack(pEntry);
}

void AMFTable::PutBool(LPCSTR szKey, bool bValue)
{
	AMFVarPtr pVar(new AMFBool());
	if (pVar)
	{
		AMFCast<AMFBool>(pVar)->SetValue(bValue);
		Put(szKey, pVar);
	}
}

void  AMFTable::PutDouble(LPCSTR szKey, double dbValue)
{
	AMFVarPtr pVar(new AMFNumber());
	if (pVar)
	{
		AMFCast<AMFNumber>(pVar)->SetDoubleValue(dbValue);
		Put(szKey, pVar);
	}
}

void  AMFTable::PutString(LPCSTR szKey, LPCSTR szValue)
{
	AMFVarPtr pVar(new AMFString());
	if (pVar)
	{
		AMFCast<AMFString>(pVar)->SetValue(szValue);
		Put(szKey, pVar);
	}
}

bool  AMFTable::IsContainsKey(LPCSTR key)
{
	return !!Get(key);
}

void AMFTable::Remove(LPCSTR szKey)
{
	AMFTableEntry *pEntry, *pNext, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;)
	{
		if(AMFCast<AMFString>(pEntry->GetKey())->GetValue() == szKey)
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

void  AMFTable::Clear()
{
	AMFTableEntry *pEntry;

	while((pEntry = m_lstEntries.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pEntry);
	}
}

size_t AMFTable::Size() const
{
	return m_lstEntries.Count();
}

void AMFTable::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
	AMFTableEntry* pEntry, *pEnd;

	pEntry = m_lstEntries.Begin();
	pEnd = m_lstEntries.End();
	for(;pEntry != pEnd;pEntry = m_lstEntries.Next(pEntry))
	{
		pEntry->Write(pOutput);
	}
	//write end of object
	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		static uint8_t octets[3] = {0, 0, AMF0_ENDOFOBJECT};
		pOutput->Write(octets, 3);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		// empty name string
		pOutput->WriteUInt8(EMPTY_STRING_TYPE);
	}
	else
	{
		ASSERT(0);
	}
}

void AMFTable::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	//write end of object
	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		bool bObjFinished = false;
		uint8_t octets[3];
		AMFTableEntry* pEntry;
		while(!pInput->Eof() && !bObjFinished)
		{
			pInput->Peek(octets,3);
			if(octets[0] == 0 
				&& octets[1] == 0 
				&& octets[2] == AMF0_ENDOFOBJECT)
			{
				pInput->Read(octets,3);
				bObjFinished = true;
				break;
			}
			else
			{
				pEntry = new AMFTableEntry();
				ASSERT(pEntry);
				pEntry->Read(pInput);
				m_lstEntries.PushBack(pEntry);
			}
		}
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		bool bObjFinished = false;
		uint8_t nByte;
		AMFTableEntry* pEntry;
		while(!pInput->Eof() && !bObjFinished)
		{
			pInput->PeekUInt8(&nByte);
			if(nByte == EMPTY_STRING_TYPE)
			{
				pInput->Skip(1);
				bObjFinished = true;
				break;
			}
			else
			{
				pEntry = new AMFTableEntry();
				ASSERT(pEntry);
				pEntry->Read(pInput);
				m_lstEntries.PushBack(pEntry);
			}
		}
	}
	else
	{
		THROW_FALSE(0);
	}
}

AMFTable &AMFTable::operator = (const AMFTable &other)
{
	AMFTableEntry *pEntry, *pEnd;

	Clear();
	pEntry = other.m_lstEntries.Begin();
	pEnd = other.m_lstEntries.End();
	for (;pEntry != pEnd;pEntry = other.m_lstEntries.Next(pEntry))
	{
		m_lstEntries.PushBack(pEntry->Clone());
	}
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMF0Object
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF0Object, 8);

AMF0Object::AMF0Object()
{
	//
}

AMF0Object::~AMF0Object()
{
	//
}

uint8_t AMF0Object::GetType() const
{
	return AMF0_OBJECT;
}

void AMF0Object::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_OBJECT);
		AMFTable::Write(pOutput);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		pOutput->WriteUInt8(AMF3_OBJECT);
		AMFTable::Write(pOutput);
	}
}

void AMF0Object::Read(BCIStream* pInput)
{
	AMFTable::Read(pInput);
}

AMFVar *AMF0Object::Clone() const
{
	AMF0Object *pObj;

	pObj = new AMF0Object();
	if (pObj)
	{
		*pObj = *this;
	}
	return pObj;
}

AMF0Object &AMF0Object::operator = (const AMF0Object &other)
{
	AMFTable::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFECMAArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF0ECMAArray, 8);

AMF0ECMAArray::AMF0ECMAArray()
{
	//
}

AMF0ECMAArray::~AMF0ECMAArray()
{
	//
}

void AMF0ECMAArray::PutOrdinal(const AMFVarPtr & pVar)
{
	if (pVar)
	{
		m_lstOrdinal.PushBack(new AMFVarWrapper(pVar));
	}
}

void AMF0ECMAArray::PutOrdinalBool(bool bValue)
{
	AMFVarPtr pVar(new AMFBool());
	if (pVar)
	{
		AMFCast<AMFBool>(pVar)->SetValue(bValue);
		PutOrdinal(pVar);
	}
}

void AMF0ECMAArray::PutOrdinalDouble(double dbValue)
{
	AMFVarPtr pVar(new AMFNumber());
	if (pVar)
	{
		AMFCast<AMFNumber>(pVar)->SetDoubleValue(dbValue);
		PutOrdinal(pVar);
	}
}

void AMF0ECMAArray::PutOrdinalString(LPCSTR szValue)
{
	AMFVarPtr pVar(new AMFString());
	if (pVar)
	{
		AMFCast<AMFString>(pVar)->SetValue(szValue);
		PutOrdinal(pVar);
	}
}

AMFVarPtr AMF0ECMAArray::Get(uint32_t nIndex)
{
	return m_lstOrdinal[nIndex]->var;
}

size_t AMF0ECMAArray::Size() const
{
	return m_lstOrdinal.Count();
}

uint8_t AMF0ECMAArray::GetType() const
{
	return AMF0_ECMAARRAY;
}

void AMF0ECMAArray::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
	AMFVarWrapper *pEntry, *pEnd;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_ECMAARRAY);
		pOutput->WriteUInt32BE(m_lstEntries.Count());
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		pOutput->WriteUInt8(AMF3_ARRAY);
		pOutput->WriteUInt32BE(m_lstOrdinal.Count());
		pEntry = m_lstOrdinal.Begin();
		pEnd = m_lstOrdinal.End();
		for (; pEntry != pEnd; pEntry = m_lstOrdinal.Next(pEntry))
		{
			pEntry->Write(pOutput);
		}
	}

	AMFTable::Write(pOutput);
}

void AMF0ECMAArray::Read(BCIStream* pInput)
{
	uint32_t nSize;

	pInput->ReadUInt32BE(&nSize);
	AMFTable::Read(pInput);
}

AMFVar *AMF0ECMAArray::Clone() const
{
	AMF0ECMAArray *pArray;

	pArray = new AMF0ECMAArray();
	if (pArray)
	{
		*pArray = *this;
	}
	return pArray;
}

AMF0ECMAArray &AMF0ECMAArray::operator = (const AMF0ECMAArray &other)
{
	AMFTable::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFStrictArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF0SArray, 8);

AMF0SArray::AMF0SArray()
{
	//
}

AMF0SArray::~AMF0SArray()
{
	Clear();
}

AMFVarPtr AMF0SArray::Get(uint32_t nIndex)
{
	AMFVarWrapper *pVar = m_lstVars[nIndex];
	if (pVar)
	{
		return pVar->var;
	}
	return AMFVarPtr();
}

size_t AMF0SArray::Size() const
{
	return m_lstVars.Count();
}

uint8_t AMF0SArray::GetType() const
{
	return AMF0_STRICTARRAY;
}

void AMF0SArray::Write(BCOStream *pOutput) const
{
	AMFVarWrapper *pVar, *pEnd;

	pOutput->WriteUInt8(AMF0_STRICTARRAY);
	pOutput->WriteUInt32BE(Size());
	// Write array data
	pVar = m_lstVars.Begin();
	pEnd = m_lstVars.End();
	for (; pVar != pEnd; pVar = m_lstVars.Next(pVar))
	{
		pVar->Write(pOutput);
	}
}

void AMF0SArray::Read(BCIStream *pInput)
{
	uint32_t nSize;
	pInput->ReadUInt32BE(&nSize);
	for (uint32_t i = 0;i < nSize;i++)
	{
		AMFVarPtr pVar = AMFCodec::Decode(pInput);
		m_lstVars.PushBack(new AMFVarWrapper(pVar));
	}
}

AMFVarPtr AMF0SArray::PopFront()
{
	AMFVarWrapper *pVar = m_lstVars.PopFront();
	if (pVar)
	{
		AMFVarPtr pRetVar(pVar->var);
		BC_SAFE_DELETE_PTR(pVar);
		return pRetVar;
	}
	return AMFVarPtr();
}

AMFVarPtr AMF0SArray::PopBack()
{
	AMFVarWrapper *pVar = m_lstVars.PopBack();
	if (pVar)
	{
		AMFVarPtr pRetVar(pVar->var);
		BC_SAFE_DELETE_PTR(pVar);
		return pRetVar;
	}
	return AMFVarPtr();
}

uint32_t AMF0SArray::Push(AMFVarPtr pVar)
{
	ASSERT(pVar);
	m_lstVars.PushBack(new AMFVarWrapper(pVar));
	return m_lstVars.Count();
}

uint32_t AMF0SArray::PushBool(bool bValue)
{
	AMFBool *pVar = new AMFBool();
	if (pVar)
	{
		pVar->SetValue(bValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMF0SArray::PushDouble(double dbValue)
{
	AMFNumber *pVar = new AMFNumber();
	if (pVar)
	{
		pVar->SetDoubleValue(dbValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMF0SArray::PushString(LPCSTR szValue)
{
	AMFString *pVar = new AMFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t AMF0SArray::Remove(uint32_t nIndex)
{
	AMFVarWrapper *pVar;

	pVar = m_lstVars[nIndex];
	if (pVar)
	{
		pVar->RemoveFromList();
		BC_SAFE_DELETE_PTR(pVar);
	}
	return m_lstVars.Count();
}

void AMF0SArray::Clear()
{
	m_lstVars.Clear();
}

AMFVar *AMF0SArray::Clone() const
{
	AMF0SArray *pArray;

	pArray = new AMF0SArray();
	if (pArray)
	{
		*pArray = *this;
	}
	return pArray;
}

AMF0SArray &AMF0SArray::operator = (const AMF0SArray &other)
{
	m_lstVars = other.m_lstVars;
	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMFSArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMFSArray, 8);

AMFSArray::AMFSArray()
{
	//
}

AMFSArray::~AMFSArray()
{
	Clear();
}

uint8_t AMFSArray::GetType() const
{
	return AMF0_STRICTARRAY;
}

void AMFSArray::Write(BCOStream* pOutput) const
{
	AMFVarWrapper *pVar, *pEnd;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_STRICTARRAY);
		pOutput->WriteUInt32BE(Size());
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		pOutput->WriteUInt8(AMF3_ARRAY);
		// Write reference
	}
	// Write array data
	pVar = m_lstVars.Begin();
	pEnd = m_lstVars.End();
	for(;pVar != pEnd;pVar = m_lstVars.Next(pVar))
	{
		pVar->Write(pOutput);
	}
}

void AMFSArray::Read(BCIStream* pInput)
{
	AMFVarPtr pVar;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
	uint32_t nSize = 0;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pInput->ReadUInt32BE(&nSize);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		AMFCodec::DecodeVLUInt32(pInput, &nSize);
		// Read reference
	}
	// Read array data
	for(uint32_t i = 0;i < nSize && !pInput->Eof();i++)
	{
		pVar = AMFCodec::Decode(pInput);
		THROW_FALSE(pVar);
		m_lstVars.PushBack(new AMFVarWrapper(pVar));
	}
}

AMFVar *AMFSArray::Clone() const
{
	AMFSArray *pArray;

	pArray = new AMFSArray();
	if (pArray)
	{
		*pArray = *this;
	}
	return pArray;
}

AMFSArray &AMFSArray::operator = (const AMFSArray &other)
{
	m_lstVars = other.m_lstVars;
	return *this;
}

AMFVarPtr AMFSArray::Get(uint32_t nIndex)
{
	AMFVarWrapper *pVar = m_lstVars[nIndex];
	if (pVar)
	{
		return pVar->var;
	}
	return AMFVarPtr();
}

AMFVarPtr AMFSArray::PopBack()
{
	AMFVarWrapper *pVar = m_lstVars.PopBack();
	if (pVar)
	{
		AMFVarPtr pRetVar(pVar->var);
		BC_SAFE_DELETE_PTR(pVar);
		return pRetVar;
	}
	return AMFVarPtr();
}

uint32_t AMFSArray::Push(AMFVarPtr pVar)
{
	ASSERT(pVar);
	m_lstVars.PushBack(new AMFVarWrapper(pVar));
	return m_lstVars.Count();
}

uint32_t AMFSArray::PushBool(bool bValue)
{
	AMFBool *pVar = new AMFBool();
	if (pVar)
	{
		pVar->SetValue(bValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMFSArray::PushDouble(double dbValue)
{
	AMFNumber *pVar = new AMFNumber();
	if (pVar)
	{
		pVar->SetDoubleValue(dbValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMFSArray::PushString(LPCSTR szValue)
{
	AMFString *pVar = new AMFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t AMFSArray::Remove(uint32_t nIndex)
{
	AMFVarWrapper *pVar;

	pVar = m_lstVars[nIndex];
	if (pVar)
	{
		pVar->RemoveFromList();
		BC_SAFE_DELETE_PTR(pVar);
	}
	return m_lstVars.Count();
}

void  AMFSArray::Clear()
{
	m_lstVars.Clear();
}

uint32_t AMFSArray::Size() const
{
	return m_lstVars.Count();
}

//////////////////////////////////////////////////////////////////////////
/// class AMF0XmlDoc
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF0XmlDoc, 8);

AMF0XmlDoc::AMF0XmlDoc() 
{
	//
}
AMF0XmlDoc::~AMF0XmlDoc()
{
	//
}

uint8_t AMF0XmlDoc::GetType() const
{
	return AMF0_XMLDOC;
}

void AMF0XmlDoc::Write(BCOStream* pOutput) const
{
	AMFString::Write(pOutput);
}

void AMF0XmlDoc::Read(BCIStream* pInput)
{
	AMFString::Read(pInput);
}

AMFVar *AMF0XmlDoc::Clone() const
{
	AMF0XmlDoc *pDoc;

	pDoc = new AMF0XmlDoc();
	if (pDoc)
	{
		*pDoc = *this;
	}
	return pDoc;
}

AMF0XmlDoc &AMF0XmlDoc::operator = (const AMF0XmlDoc &other)
{
	AMFString::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMF0TObject
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF0TObject, 8);

AMF0TObject::AMF0TObject()
{
	//
}

AMF0TObject::~AMF0TObject()
{
	//
}

uint8_t AMF0TObject::GetType() const
{
	return AMF0_TYPEDOBJECT;
}

void AMF0TObject::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		pOutput->WriteUInt8(AMF0_TYPEDOBJECT);
		AMFCodec::EncodeString(pOutput, m_strClsName);
		AMFTable::Write(pOutput);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		ASSERT(0);
	}
}

void AMF0TObject::Read(BCIStream* pInput)
{
	AMFCodec::DecodeString(pInput, m_strClsName);
	AMF0Object::Read(pInput);
}

AMFVar *AMF0TObject::Clone() const
{
	AMF0TObject *pObj;

	pObj = new AMF0TObject();
	if (pObj)
	{
		*pObj = *this;
	}
	return pObj;
}

AMF0TObject &AMF0TObject::operator = (const AMF0TObject &other)
{
	AMF0Object::operator =(other);
	m_strClsName = other.m_strClsName;

	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// AMF3 specific type
///////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
/// class AMF3XmlDoc
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3XmlDoc, 8);

AMF3XmlDoc::AMF3XmlDoc() 
{
	//
}

AMF3XmlDoc::~AMF3XmlDoc()
{
	//
}

uint8_t AMF3XmlDoc::GetType() const
{
	return AMF3_XMLDOC;
}

void AMF3XmlDoc::Write(BCOStream* pOutput) const
{
	AMFString::Write(pOutput);
}

void AMF3XmlDoc::Read(BCIStream* pInput)
{
	AMFString::Read(pInput);
}

AMFVar *AMF3XmlDoc::Clone() const
{
	AMF3XmlDoc *pDoc;

	pDoc = new AMF3XmlDoc();
	if (pDoc)
	{
		*pDoc = *this;
	}
	return pDoc;
}

AMF3XmlDoc &AMF3XmlDoc::operator = (const AMF3XmlDoc &other)
{
	AMFString::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// class AMF3Array
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3Array, 8);

AMF3Array::AMF3Array()
{
	//
}

AMF3Array::~AMF3Array()
{
	Clear();
}

uint8_t AMF3Array::GetType() const
{
	return AMF3_ARRAY;
}

void AMF3Array::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	switch(eEncoding)
	{
	case ObjectEncoding::AMF0:
		{
			//AMFVarPtr pArray((AMFVar *)this);
			//// Switch encode type
			//AMFCodec::Encode(pOutput, AMFVarPtr(new AMFAVMPlus()));
			//pCodecCtx->SetEncoding(ObjectEncoding::AMF3);
			//AMFCodec::Encode(pOutput, pArray);
			//pArray.Set(NULL);
			ASSERT(0);
		}
		break;
	case ObjectEncoding::AMF3:
		{
			uint32_t nMask, nSize;

			// Write array instance
			nSize = m_lstVars.Count();
			nMask = MakeReference(nSize, false);
			AMFCodec::EncodeVLUInt32(pOutput, nMask);
			// Write key-value pairs
			AMFTable::Write(pOutput);
			// Write dense values
			if (nSize)
			{
				AMFVarWrapper *pVar, *pEnd;

				pVar = m_lstVars.Begin();
				pEnd = m_lstVars.End();
				for (;pVar != pEnd;pVar = m_lstVars.Next(pVar))
				{
					AMFCodec::Encode(pOutput, pVar->var);
				}
			}
		}
		break;
	default:
		ASSERT(0);
		break;
	}
	// Restore encode type
	pCodecCtx->SetEncoding(eEncoding);
}

void AMF3Array::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		uint32_t nSize;
		pInput->ReadUInt32BE(&nSize);
		AMFTable::Read(pInput);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask, nSize;
		AMFVarPtr pVar;

		AMFCodec::DecodeVLUInt32(pInput, &nMask);
		// Read array instance
		nSize = nMask >> 1;
		// Read associative name-value pairs
		AMFTable::Read(pInput);
		// Read dense values
		m_lstVars.Clear();
		for (uint32_t i = 0;i < nSize;i++)
		{
			pVar = AMFCodec::Decode(pInput);
			THROW_FALSE(pVar);
			m_lstVars.PushBack(new AMFVarWrapper(pVar));
		}
	}
}

AMFVar *AMF3Array::Clone() const
{
	AMF3Array *pArray;

	pArray = new AMF3Array();
	if (pArray)
	{
		*pArray = *this;
	}
	return pArray;
}

AMF3Array &AMF3Array::operator = (const AMF3Array &other)
{
	AMFTable::operator =(other);
	m_lstVars = other.m_lstVars;

	return *this;
}

AMFVarPtr AMF3Array::Get(uint32_t nIndex)
{
	AMFVarWrapper *pVar = m_lstVars[nIndex];
	if (pVar)
	{
		return pVar->var;
	}
	return AMFVarPtr();
}

AMFVarWrapper *AMF3Array::Begin() const
{
	return m_lstVars.Begin();
}

AMFVarWrapper *AMF3Array::Next(AMFVarWrapper *pIter) const
{
	return m_lstVars.Next(pIter);
}

AMFVarWrapper *AMF3Array::End() const
{
	return m_lstVars.End();
}

AMFVarPtr AMF3Array::PopFront()
{
	AMFVarWrapper *pVar = m_lstVars.PopFront();
	if (pVar)
	{
		AMFVarPtr pRetVar(pVar->var);
		BC_SAFE_DELETE_PTR(pVar);
		return pRetVar;
	}
	return AMFVarPtr();
}

AMFVarPtr AMF3Array::PopBack()
{
	AMFVarWrapper *pVar = m_lstVars.PopBack();
	if (pVar)
	{
		AMFVarPtr pRetVar(pVar->var);
		BC_SAFE_DELETE_PTR(pVar);
		return pRetVar;
	}
	return AMFVarPtr();
}

uint32_t AMF3Array::Push(const AMFVarPtr &pVar)
{
	ASSERT(pVar);
	m_lstVars.PushBack(new AMFVarWrapper(pVar));
	return m_lstVars.Count();
}

uint32_t AMF3Array::PushBool(bool bValue)
{
	AMFBool *pVar = new AMFBool();
	if (pVar)
	{
		pVar->SetValue(bValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMF3Array::PushDouble(double dbValue)
{
	AMFNumber *pVar = new AMFNumber();
	if (pVar)
	{
		pVar->SetDoubleValue(dbValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t  AMF3Array::PushString(LPCSTR szValue)
{
	AMFString *pVar = new AMFString();
	if (pVar)
	{
		pVar->SetValue(szValue);
		Push(AMFVarPtr(pVar));
	}
	return m_lstVars.Count();
}

uint32_t AMF3Array::Remove(uint32_t nIndex)
{
	AMFVarWrapper *pVar;

	pVar = m_lstVars[nIndex];
	if (pVar)
	{
		pVar->RemoveFromList();
		BC_SAFE_DELETE_PTR(pVar);
	}
	return m_lstVars.Count();
}

void  AMF3Array::Clear()
{
	AMFVarWrapper *pVar;

	while((pVar = m_lstVars.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pVar);
	}
}

uint32_t AMF3Array::Size() const
{
	return m_lstVars.Count();
}

//////////////////////////////////////////////////////////////////////////
/// class AMF3Traits
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3Traits, 8);

AMF3Traits::AMF3Traits(AMF3ObjectTypeE eType/* = AMFOBJ_DYNAMIC*/)
	: m_eObjectType(AMFOBJ_DYNAMIC)
{
	UNUSED(eType);
}

AMF3Traits::~AMF3Traits()
{
	//
}

uint8_t AMF3Traits::GetType() const
{
	return AMF_TRAITS;
}

void AMF3Traits::Write(BCOStream* pOutput) const
{
	long eEncoding;
	AMFCodecCtx *pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();

	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		ASSERT(0);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		// Class name
		AMFCodec::EncodeString(pOutput, m_strClsName);
		// Write sealed attributes
		if (m_lstMembers.Count())
		{
			AMFVarWrapper *pVar, *pEnd;
			pVar = m_lstMembers.Begin();
			pEnd = m_lstMembers.End();
			for (;pVar != pEnd;pVar = m_lstMembers.Next(pVar))
			{
				AMFCodec::EncodeString(pOutput, pVar->var);
			}
		}
	}
}

void AMF3Traits::Read(BCIStream* pInput)
{
	UNUSED(pInput);
	// Not implement
	THROW_FALSE(0);
}

void AMF3Traits::Read(BCIStream* pInput, uint32_t nMask)
{
	long eEncoding;
	AMFCodecCtx *pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();

	//write end of object
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		ASSERT(0);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		AMFVarWrapper *pVar;
		uint32_t nSize;

		// Get traits type
		m_eObjectType = (nMask & 0x08)?AMFOBJ_DYNAMIC:AMFOBJ_STATIC;
		// Get number of sealed attributes
		nSize = nMask >> 4;
		// Read class name
		AMFCodec::DecodeString(pInput, m_strClsName);
		// Read sealed attributes
		while(!pInput->Eof() && nSize)
		{
			pVar = new AMFVarWrapper();
			ASSERT(pVar);
			if (pVar)
			{
				// Decode attribute
				AMFCodec::DecodeString(pInput, pVar->var);
				m_lstMembers.PushBack(pVar);
				nSize--;
			}
		}
		ASSERT(m_lstMembers.Count() == (nMask >> 4));
	}
	else
	{
		// Should never got here
		ASSERT(0);
	}
}

AMFVar *AMF3Traits::Clone() const
{
	AMF3Traits *pObj;

	pObj = new AMF3Traits();
	if (pObj)
	{
		*pObj = *this;
	}
	return pObj;
}

AMF3Traits &AMF3Traits::operator = (const AMF3Traits &other)
{
	m_strClsName = other.m_strClsName;
	m_lstMembers = other.m_lstMembers;
	
	return *this;
}

void AMF3Traits::Clear()
{
	m_lstMembers.Clear();
}

int32_t AMF3Traits::GetIndexByKey( LPCSTR lpszKey )
{
	AMFVarWrapper *pVar, *pEnd;

	pVar = m_lstMembers.Begin();
	pEnd = m_lstMembers.End();
	for (int32_t i = 0;pVar != pEnd;pVar = m_lstMembers.Next(pVar))
	{
		ASSERT(pVar->var->GetType() == AMF_STRING);
		if (AMFCast<AMFString>(pVar->var)->GetValue() == lpszKey)
		{
			return i;
		}
		i++;
	}
	return -1;
}

//////////////////////////////////////////////////////////////////////////
/// class AMF3Object
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3Object, 8);

AMF3Object::AMF3Object(AMF3ObjectTypeE eType/* = AMFOBJ_DYNAMIC*/)
	: m_pTraits(new AMF3Traits(eType))
{
	//
}

AMF3Object::~AMF3Object()
{
	//
}

uint8_t AMF3Object::GetType() const
{
	return AMF3_OBJECT;
}

void AMF3Object::Write(BCOStream* pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	switch(eEncoding)
	{
	case ObjectEncoding::AMF0:
		{
			////pOutput->WriteUInt8(AMF0_OBJECT);
			////AMFTable::Write(pOutput);
			//AMFVarPtr pObject((AMFVar *)this);
			//// Switch encode type
			//AMFCodec::Encode(pOutput, AMFVarPtr(new AMFAVMPlus()));
			//pCodecCtx->SetEncoding(ObjectEncoding::AMF3);
			//AMFCodec::Encode(pOutput, pObject);
			//pObject.Set(NULL);
			ASSERT(0);
		}
		break;
	case ObjectEncoding::AMF3:
		{
			uint32_t nMask;
			int32_t nIndex;

			// In as3, dynamic class won't use same traits with any other
			// static class, neither nor other dynamic classes have exactly
			// the same sealed-attributes.
			nIndex = pCodecCtx->GetEncClassDefIndex(m_pTraits);
			if (nIndex < 0)
			{
				uint8_t eType;

				nMask = m_lstValues.Count() << 4;
				eType = AMFCast<AMF3Traits>(m_pTraits)->GetObjectType();
				if (eType == AMFOBJ_STATIC)
				{
					nMask |= STATIC_OBJECT;
				} 
				else
				{
					nMask |= DYNAMIC_OBJECT;
				}
				// Write mask
				AMFCodec::EncodeVLUInt32(pOutput, nMask);
				// Write traits
				m_pTraits->Write(pOutput);
				// Add traits to reference table
				pCodecCtx->PushEncClassDef(m_pTraits);
			}
			else
			{
				nMask = (nIndex << 2) | REFERENCE_BIT;
				AMFCodec::EncodeVLUInt32(pOutput, nMask);
			}
			// Write values of sealed-attributes
			AMFVarWrapper *pVar, *pEnd;
			pVar = m_lstValues.Begin();
			pEnd = m_lstValues.End();
			for (;pVar != pEnd;pVar = m_lstValues.Next(pVar))
			{
				AMFCodec::Encode(pOutput, pVar->var);
			}
			// Write key-value pairs
			AMFTable::Write(pOutput);
		}
		break;
	default:
		ASSERT(0);
		break;
	}
}

void AMF3Object::Read(BCIStream* pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	//write end of object
	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		//AMFTable::Read(pInput);
		ASSERT(FALSE);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;

		AMFCodec::DecodeVLUInt32(pInput, &nMask);

		// Ref not found
		// Create instance based on class def
		if (IsTraitsRef(nMask)) // traits reference
		{
			m_pTraits = pCodecCtx->GetDecClassDef(nMask >> 2);
		}
		else
		{
			AMF3Traits *pTraits = new AMF3Traits();
			// Read traits
			if (pTraits)
			{
				// TODO : read traits
				pTraits->Read(pInput, nMask);
				m_pTraits.reset(pTraits);
				// Add traits to index table
				pCodecCtx->PushDecClassDef(m_pTraits);
			}
		}

		//// Reference must be added before children (to allow for recursion).
		//m_nRefIndex = pCodecCtx->PushDecObject(this);

		// read class instance
		// traits : dynamic class defs
		if (!IsExternalizable(nMask)) 
		{
			uint32_t nSize;

			// Read class sealed-values
			m_lstValues.Clear();
			nSize = AMFCast<AMF3Traits>(m_pTraits)->Count();
			if (nSize)
			{
				AMFVarWrapper *pVar;
				// Read attribute values
				while(!pInput->Eof() && nSize)
				{
					pVar = new AMFVarWrapper();
					ASSERT(pVar);
					if (pVar)
					{
						pVar->var = AMFCodec::Decode(pInput);
						m_lstValues.PushBack(pVar);
						nSize--;
					}
				}
			}
			// Read dynamic attributes
			bool bObjFinished = false;
			uint8_t nByte;
			AMFTableEntry *pEntry;
			AMFTable::Clear(); // clean up dynamic-entries
			while(!pInput->Eof() && !bObjFinished)
			{
				pInput->PeekUInt8(&nByte);
				if(nByte == EMPTY_STRING_TYPE)
				{
					pInput->ReadUInt8(&nByte);
					bObjFinished = true;
					break;
				}
				else
				{
					pEntry = new AMFTableEntry();
					ASSERT(pEntry);
					pEntry->Read(pInput);
					m_lstEntries.PushBack(pEntry);
				}
			}
		}
		else
		{
			// Read traits-ext : externalizable class defs
			// NOT IMPLEMENTATION
			ASSERT(0);
		}
	}
	else
	{
		// Should never got here
		ASSERT(0);
	}
	if (pCodecCtx != pInput->GetUserData())
	{
		pInput->SetUserData(pCodecCtx);
	}
}

AMFVar *AMF3Object::Clone() const
{
	AMF3Object *pObj;

	pObj = new AMF3Object();
	if (pObj)
	{
		*pObj = *this;
	}
	return pObj;
}

AMF3Object &AMF3Object::operator = (const AMF3Object &other)
{
	AMFTable::operator =(other);
	m_pTraits = other.m_pTraits;
	m_lstValues = other.m_lstValues;

	return *this;
}

AMFVarPtr AMF3Object::Get(LPCSTR lpszKey)
{
	int32_t nIndex;

	nIndex = AMFCast<AMF3Traits>(m_pTraits)->GetIndexByKey(lpszKey);
	if (nIndex > 0)
	{
		return m_lstValues[nIndex]->var;
	}
	else
	{
		return AMFTable::Get(lpszKey);
	}
}

//////////////////////////////////////////////////////////////////////////
/// class AMF3Xml
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3Xml, 8);

AMF3Xml::AMF3Xml() 
{
	//
}

AMF3Xml::~AMF3Xml()
{
	//
}

uint8_t AMF3Xml::GetType() const
{
	return AMF3_XML;
}

void AMF3Xml::Write(BCOStream* pOutput) const
{
	AMFCodec::EncodeVLUInt32(pOutput, m_strValue.length());
	pOutput->Write(m_strValue.sdata(), m_strValue.length());
}

void AMF3Xml::Read(BCIStream* pInput)
{
	AMFVarPtr pValue;
	AMFCodec::DecodeString(pInput, pValue);
	if (pValue && pValue->GetType() == AMF_STRING)
	{
		m_strValue = AMFCast<AMFString>(pValue)->GetValue();
	}
}

AMFVar *AMF3Xml::Clone() const
{
	AMF3Xml *pXML;

	pXML = new AMF3Xml();
	if (pXML)
	{
		*pXML = *this;
	}
	return pXML;
}

AMF3Xml &AMF3Xml::operator = (const AMF3Xml &other)
{
	AMFString::operator =(other);

	return *this;
}

//////////////////////////////////////////////////////////////////////////
/// AMF3ByteArray
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(AMF3ByteArray, 8);

AMF3ByteArray::AMF3ByteArray()
{
	//
}

AMF3ByteArray::AMF3ByteArray(const AMF3ByteArray &lhs)
	: AMFVar()
{
	AMF3ByteArray::operator =(lhs);
}

AMF3ByteArray::~AMF3ByteArray()
{
	//
}

uint8_t AMF3ByteArray::GetType() const
{
	return AMF3_BYTEARRAY;
}

void AMF3ByteArray::Write(BCOStream *pOutput) const
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	switch(eEncoding)
	{
	case ObjectEncoding::AMF0:
		{
			//AMFVarPtr pArray((AMFVar *)this);
			//// Switch encode type
			//AMFCodec::Encode(pOutput, AMFVarPtr(new AMFAVMPlus()));
			//pCodecCtx->SetEncoding(ObjectEncoding::AMF3);
			//AMFCodec::Encode(pOutput, pArray);
			//pArray.Set(NULL);
			ASSERT(0);
		}
		break;
	case ObjectEncoding::AMF3:
		{
			uint32_t nMask, nSize;

			// Not reference
			nSize = m_sBody.UsedLength();
			nMask = MakeReference(nSize, false);
			AMFCodec::EncodeVLUInt32(pOutput, nMask);
			m_sBody.Rewind();
			m_sBody.WriteTo(*pOutput);
		}
		break;
	}
	pCodecCtx->SetEncoding(eEncoding);
}

void AMF3ByteArray::Read(BCIStream *pInput)
{
	AMFCodecCtx *pCodecCtx;
	long eEncoding;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF0)
	{
		ASSERT(0);
	}
	else if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask, nSize;

		AMFCodec::DecodeVLUInt32(pInput, &nMask);
		// Not reference
		// Read array data
		nSize = nMask >> 1;
		m_sBody.ReadFrom(*pInput, nSize);
	}
}

AMFVar *AMF3ByteArray::Clone() const
{
	AMF3ByteArray *pBArray;

	pBArray = new AMF3ByteArray();
	if (pBArray)
	{
		*pBArray = *this;
	}
	return pBArray;
}

AMF3ByteArray &AMF3ByteArray::operator = (const AMF3ByteArray &other)
{
	other.m_sBody.Clone(m_sBody);
	return *this;
}

void AMF3ByteArray::Push(uint8_t cValue)
{
	m_sBody.Write(&cValue, 1);
}

void AMF3ByteArray::Push(LPCVOID pData, uint32_t nCount)
{
	m_sBody.Write(pData, nCount);
}

//////////////////////////////////////////////////////////////////////////
/// AMFCodec
//////////////////////////////////////////////////////////////////////////

AMFCodec::AMFCodec()
{
	//
}

AMFCodec::~AMFCodec()
{
	//
}

AMFVar* AMFCodec::CreateVar(uint8_t eAmfType, bool bAMF3 /*= false*/)
{
	AMFVar *pAmfType = NULL;
	if (bAMF3)
	{
		switch(eAmfType)
		{
		case AMF3_UNDEFINED:
			pAmfType = new AMFUndefined();
			break;
		case AMF3_NULL:
			pAmfType = new AMFNull();
			break;
		case AMF3_FALSE:
			pAmfType = new AMFBool(false);
			break;
		case AMF3_TRUE:
			pAmfType = new AMFBool(true);
			break;
		case AMF3_INT:
			pAmfType = new AMFNumber(AMFNumber::NUM_TYPE_INT);
			break;
		case AMF3_DOUBLE:
			pAmfType = new AMFNumber(AMFNumber::NUM_TYPE_DOUBLE);
			break;
		case AMF3_STRING:
			pAmfType = new AMFString();
			break;
		case AMF3_XMLDOC:
			pAmfType = new AMF3XmlDoc();
			break;
		case AMF3_DATE:
			pAmfType = new AMFDate();
			break;
		case AMF3_ARRAY:
			pAmfType = new AMF3Array();
			break;
		case AMF3_OBJECT:
			pAmfType = new AMF3Object();
			break;
		case AMF3_XML:
			pAmfType = new AMF3Xml();
			break;
		case AMF3_BYTEARRAY:
			pAmfType = new AMF3ByteArray();
			break;
		}
	}
	else
	{
		switch(eAmfType)
		{
		case AMF0_DOUBLE:
			pAmfType = new AMFNumber(AMFNumber::NUM_TYPE_DOUBLE);
			break;
		case AMF0_BOOL:
			pAmfType = new AMFBool();
			break;
		case AMF0_STRING:
			pAmfType = new AMFString();
			break;
		case AMF0_OBJECT:
			pAmfType = new AMF0Object();
			break;
		case AMF0_NULL:
			pAmfType = new AMFNull();
			break;
		case AMF0_UNDEFINED:
			pAmfType = new AMFUndefined();
			break;
		case AMF0_ECMAARRAY:
			pAmfType = new AMF0ECMAArray();
			break;
		case AMF0_STRICTARRAY:
			pAmfType = new AMF0SArray();
			break;
		case AMF0_DATE:
			pAmfType = new AMFDate();
			break;
		case AMF0_LSTRING:
			pAmfType = new AMFString();
			break;
		case AMF0_XMLDOC:
			pAmfType = new AMF0XmlDoc();
			break;
		case AMF0_TYPEDOBJECT:
			pAmfType = new AMF0TObject();
			break;
		case AMF0_AVMPLUS:
			pAmfType = new AMFAVMPlus();
			break;
		}
	}
	if (pAmfType == NULL)
	{
		switch(eAmfType)
		{
		case AMF_UNDEFINED:
			pAmfType = new AMFUndefined();
			break;
		case AMF_NUMBER:
			pAmfType = new AMFNumber();
			break;
		case AMF_BOOL:
			pAmfType = new AMFBool();
			break;
		case AMF_STRING:
			pAmfType = new AMFString();
			break;
		case AMF_NULL:
			pAmfType = new AMFNull();
			break;
		case AMF_DATE:
			pAmfType = new AMFDate();
			break;
		case AMF_ARRAY:
			pAmfType = new AMF3Array();
			break;
		case AMF_OBJECT:
			pAmfType = new AMF3Object();
			break;
		}
	}
	return pAmfType;
}

double AMFCodec::ParseDouble(const uint8_t* buf)
{
	float64_t val;
	
	memcpy(&val, buf, sizeof(float64_t));
	val = BC::ByteOrder::swapBytesDouble(val);
	return val;
}

AMFVarPtr AMFCodec::Decode(BCIStream* pInput)
{
	bool bAMF3;
	uint8_t eAmfType;
	AMFVarPtr pRetVar;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
	AMFVarPtr refVar;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	do
	{
		eAmfType = AMF0_NULL;
		bAMF3 = pCodecCtx->GetEncoding() == ObjectEncoding::AMF3;
		pInput->ReadUInt8(&eAmfType);
		if (!(pRetVar = GetReference(eAmfType, pInput, pCodecCtx, eEncoding)))
		{
			AMFVar *pVar = CreateVar(eAmfType, bAMF3);
			if(pVar)
			{
				if (bAMF3)
				{
					// Reference must be added before children (to allow for recursion).
					switch(eAmfType)
					{
					case AMF3_ARRAY:
					case AMF3_OBJECT:
						pRetVar.reset(pVar);
						pCodecCtx->PushDecObject(pRetVar);
						break;
					}
				}
				else
				{
					// Reference must be added before children (to allow for recursion).
					switch(eAmfType)
					{
					case AMF0_STRICTARRAY:
					case AMF0_OBJECT:
					case AMF0_ECMAARRAY:
						pRetVar.reset(pVar);
						pCodecCtx->PushDecObject(pRetVar);
						break;
					}
				}
				pVar->Read(pInput);
				if (bAMF3 && pVar->GetType() == AMF_BOOL)
				{
					AMFBool *pBool = (AMFBool *)pVar;
					pBool->SetValue(eAmfType == AMF3_TRUE);
					pRetVar.reset(pVar);
				}
				else if (pVar->GetType() == AMF0_AVMPLUS)
				{
					pCodecCtx->SetEncoding(ObjectEncoding::AMF3);
					BC_SAFE_DELETE_PTR(pVar);
					continue;
				}
				else if (bAMF3)
				{
					switch(eAmfType)
					{
					case AMF3_STRING:
						pRetVar.reset(pVar);
						pCodecCtx->PushDecString(pRetVar);
						break;
					case AMF3_DATE:
					case AMF3_BYTEARRAY:
						pRetVar.reset(pVar);
						pCodecCtx->PushDecObject(pRetVar);
						break;
					}
				}
				else if (!pRetVar)
				{
					pRetVar.reset(pVar);
				}
			}
		}
		break;
	}while(true);
	// Restore objectEncoding setting
	pCodecCtx->SetEncoding(eEncoding);

	return pRetVar;
}

AMFVarPtr AMFCodec::GetReference( 
	uint8_t eAmfType, 
	BCIStream* pInput, 
	AMFCodecCtx *pCtx,
	long eEncoding)
{
	AMFVarPtr pRetVar;
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask, nReadLen;

		if ((nReadLen = AMFCodec::DecodeVLUInt32(pInput, &nMask, true)))
		{
			if (IsReference(nMask))
			{
				pInput->Skip(nReadLen);
				switch(eAmfType)
				{
				case AMF3_STRING:
					pRetVar = pCtx->GetDecString(nMask >> 1);
					break;
				case AMF3_DATE:
				case AMF3_ARRAY:
				case AMF3_OBJECT:
				case AMF3_BYTEARRAY:
					pRetVar = pCtx->GetDecObject(nMask >> 1);
					break;
				}
			}
		}
	}
	else if (eAmfType == AMF0_REFERENCE && eEncoding == ObjectEncoding::AMF0)
	{
		uint16_t nIndex;
		pInput->ReadUInt16BE(&nIndex);
		pRetVar = pCtx->GetDecObject(nIndex);
	}
	return pRetVar;
}

void AMFCodec::Encode(BCOStream* pOutput, const AMFVarPtr &var)
{
	if(var)
	{
		AMFCodecCtx *pCtx = (AMFCodecCtx *)pOutput->GetUserData();
		long eEncoding = (long)pCtx->GetEncoding();
		uint8_t eType = var->GetType();
		switch(eEncoding)
		{
		case ObjectEncoding::AMF0:
			if (var->GetEncoding() == ObjectEncoding::AMF0)
			{
				switch(eType)
				{
				case AMF0_STRICTARRAY:
				case AMF0_OBJECT:
				case AMF0_ECMAARRAY:
					{
						int32_t nIndex;

						nIndex = pCtx->GetEncObjectIndex(var);
						if (nIndex >= 0) // Is reference
						{
							pOutput->WriteUInt8(AMF0_REFERENCE);
							pOutput->WriteUInt16BE(nIndex);
						}
						else
						{
							// Add array/object/ecmaarray to reference table
							pCtx->PushEncObject(var);
							var->Write(pOutput);
						}
					}
					break;
				default:
					var->Write(pOutput);
					break;
				}
				break;
			} 
			else
			{
				// Switch encode type
				AMFCodec::Encode(pOutput, AMFVarPtr(new AMFAVMPlus()));
				pCtx->SetEncoding(ObjectEncoding::AMF3);
			}
		case ObjectEncoding::AMF3:
			{
				switch(eType)
				{
				case AMF_STRING:
					AMFCodec::EncodeString(pOutput, var, true);
					break;
				case AMF3_ARRAY:
					{
						int32_t nIndex;

						pOutput->WriteUInt8(AMF3_ARRAY);
						nIndex = pCtx->GetEncObjectIndex(var);
						if (nIndex >= 0) // Is reference
						{
							uint32_t nMask;

							nMask = MakeReference(nIndex, true);
							AMFCodec::EncodeVLUInt32(pOutput, nMask);
						}
						else
						{
							// Add array to reference table
							pCtx->PushEncObject(var);
							var->Write(pOutput);
						}
					}
					break;
				case AMF3_DATE:
					{
						int32_t nIndex;
						uint32_t nMask;

						pOutput->WriteUInt8(AMF3_DATE);
						nIndex = pCtx->GetEncObjectIndex(var);
						if (nIndex >= 0) // Is reference
						{
							nMask = MakeReference(nIndex, true);
							AMFCodec::EncodeVLUInt32(pOutput, nMask);
						}
						else
						{
							nMask = MakeReference(0, false);
							AMFCodec::EncodeVLUInt32(pOutput, nMask);
							// Add data to reference table
							pCtx->PushEncObject(var);
							var->Write(pOutput);
						}
					}
					break;
				case AMF3_OBJECT:
					{
						int32_t nIndex;

						pOutput->WriteUInt8(AMF3_OBJECT);
						nIndex = pCtx->GetEncObjectIndex(var);
						if (nIndex >= 0) // Is reference
						{
							uint32_t nMask;

							nMask = MakeReference(nIndex, true);
							AMFCodec::EncodeVLUInt32(pOutput, nMask);
						}
						else
						{
							// Add object to reference table
							pCtx->PushEncObject(var);
							var->Write(pOutput);
						}
					}
					break;
				case AMF3_BYTEARRAY:
					{
						int32_t nIndex;

						pOutput->WriteUInt8(AMF3_BYTEARRAY);
						nIndex = pCtx->GetEncObjectIndex(var);
						if (nIndex >= 0) // Is reference
						{
							uint32_t nMask;

							nMask = MakeReference(nIndex, true);
							AMFCodec::EncodeVLUInt32(pOutput, nMask);
						}
						else
						{
							// Add bytearray to reference table
							pCtx->PushEncObject(var);
							var->Write(pOutput);
						}
					}
					break;
				default:
					var->Write(pOutput);
					break;
				}
			}
			break;
		}
		pCtx->SetEncoding(eEncoding);
	}
}

const char* AMFCodec::GetTypeName(uint8_t eAmfType, bool bAMF3 /*= false*/)
{
	if (bAMF3)
	{
		switch(eAmfType)
		{
		case AMF3_UNDEFINED:
			return "AMF3::UNDEFINED";
		case AMF3_NULL:
			return "AMF3::NULL";
		case AMF3_FALSE:
			return "AMF3::FALSE";
		case AMF3_TRUE:
			return "AMF3::TRUE";
		case AMF3_INT:
			return "AMF3::INT";
		case AMF3_DOUBLE:
			return "AMF3::DOUBLE";
		case AMF3_STRING:
			return "AMF3::STRING";
		case AMF3_XMLDOC:
			return "AMF3::XMLDOC";
		case AMF3_DATE:
			return "AMF3::DATE";
		case AMF3_ARRAY:
			return "AMF3::ARRAY";
		case AMF3_OBJECT:
			return "AMF3::OBJECT";
		case AMF3_XML:
			return "AMF3::XML";
		case AMF3_BYTEARRAY:
			return "AMF3::BYTEARRAY";
		default:
			return "AMF3::UNKNOWN";
		}
	}
	else
	{
		switch(eAmfType)
		{
		case AMF0_DOUBLE:
			return "AMF0::DOUBLE";
		case AMF0_BOOL:
			return "AMF0::BOOL";
		case AMF0_STRING:
			return "AMF0::UTF8";
		case AMF0_OBJECT:
			return "AMF0::OBJECT";
		case AMF0_MOVIECLIP:
			return "AMF0::MOVIECLIP";
		case AMF0_NULL:
			return "AMF0::NULL";
		case AMF0_UNDEFINED:
			return "AMF0::UNDEFINED";
		case AMF0_REFERENCE:
			return "AMF0::REFERENCE";
		case AMF0_ECMAARRAY:
			return "AMF0::MIXEDARRAY";
		case AMF0_ENDOFOBJECT:
			return "AMF0::ENDOFOBJECT";
		case AMF0_STRICTARRAY:
			return "AMF0::ARRAY";
		case AMF0_DATE:
			return "AMF0::DATE";
		case AMF0_LSTRING:
			return "AMF0::LONGUTF8";
		case AMF0_UNSUPPORTED:
			return "AMF0::UNSUPPORTED";
		case AMF0_RECORDSET:
			return "AMF0::RECORDSET";
		case AMF0_XMLDOC:
			return "AMF0::XMLDOCUMENT";
		case AMF0_TYPEDOBJECT:
			return "AMF0::TYPEDOBJECT";
		case AMF0_AVMPLUS:
			return "AMF0::AVMPLUS";
		default:
			return "AMF0::UNKNOWN";
		}
	}
}

int8_t AMFCodec::GetVLIntegerSize(uint64_t nValue)
{
	if(nValue >= 0x200000)
		return 4;
	if(nValue >= 0x4000)
		return 3;
	if(nValue >= 0x80)
		return 2;
	return 1;
}

int32_t AMFCodec::EncodeVLUInt32(BCOStream *pOutput, uint32_t nValue)
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
		// Int is too big to be encoded by AMF.:(
		return 0;        
	}

	return pOutput->Write(tmp, tmp_size);
}

int32_t AMFCodec::DecodeVLUInt32(
	BCIStream *pInput, 
	uint32_t *pValue,
	bool bPeek /*= false*/)
{
	int32_t result = 0;
	uint32_t byte_cnt = 0;
	uint8_t byte;
	
	if (bPeek)
	{
		if (pInput->PeekUInt8(&byte) != 1)
			return 0;
	} 
	else
	{
		if (pInput->ReadUInt8(&byte) != 1)
			return 0;
	}

	// If 0x80 is set, int includes the next byte, up to 4 total bytes
	while ((byte & 0x80) && (byte_cnt < 3)) 
	{
		result <<= 7;
		result |= byte & 0x7F;
		if (bPeek)
		{
			if (pInput->PeekUInt8(&byte) != 1)
				return 0;
		} 
		else
		{
			if (pInput->ReadUInt8(&byte) != 1)
				return 0;
		}
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

int32_t AMFCodec::EncodeVLUInt64(BCOStream *pOutput, uint64_t nValue)
{
	int32_t result = 0;
	uint8_t shift = (GetVLIntegerSize(nValue)-1)*7;
	bool max = shift>=63; // Can give 10 bytes!
	if(max)
		++shift;

	while(shift >= 7) 
	{
		result = pOutput->WriteUInt8(0x80 | ((nValue>>shift)&0x7F));
		shift -= 7;
	}
	result += pOutput->WriteUInt8(max ? nValue&0xFF : nValue&0x7F);

	return result;
}

int32_t AMFCodec::DecodeVLUInt64(BCIStream *pInput, uint64_t *pValue)
{
	uint64_t result = 0;
	uint8_t byte_cnt = 0;
	uint8_t byte;

	if (pInput->ReadUInt8(&byte) != 1)
		return 0;

	// If 0x80 is set, int includes the next byte, up to 8 total bytes
	while ((byte & 0x80) && (byte_cnt < 8)) 
	{
		result <<= 7;
		result |= byte & 0x7F;
		if (pInput->ReadUInt8(&byte) != 1)
			return 0;
		byte_cnt++;
	}

	// shift bits in last byte
	if (byte_cnt < 8) 
	{
		result <<= 7; // shift by 7, since the 1st bit is reserved for next byte flag
		result |= byte & 0x7F;
	} 
	else 
	{
		result <<= 8; // shift by 8, since no further bytes are possible and 1st bit is not used for flag.
		result |= byte & 0xff;
	}
	result |= byte;

	*pValue = result;
	return byte_cnt + 1;
}

int32_t AMFCodec::DecodeString(
	BCIStream *pInput, 
	AMFVarPtr &refStr)
{
	int32_t result = 0;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
	BOOL bMalformed = FALSE;

	pCodecCtx = (AMFCodecCtx *)pInput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask, nLen;
		result = AMFCodec::DecodeVLUInt32(pInput, &nMask);
		if (result <= 0)
		{
			return result;
		}
		if (nMask == EMPTY_STRING_TYPE)
		{
			return result;
		}
		if (IsReference(nMask))
		{
			refStr = pCodecCtx->GetDecString(nMask >> 1);
			return result;
		}
		else
		{
			// Not reference
			AMFVarPtr pValue(new AMFString());
			BCPString &refValue = AMFCast<AMFString>(pValue)->GetValue();
			nLen = nMask >> 1;
			if (nLen == 0)
			{
				return result;
			}
			if (pInput->RemainingLength() < nLen)
			{
				//ASSERT(0);
				//return 0;
				nLen = pInput->RemainingLength();
				bMalformed = TRUE;
			}
			pInput->Read(refValue.GetWriteBuffer(nLen), nLen);
			refValue.UngetWriteBuffer(nLen);
#ifdef USE_UTF8
			BCUtf8ToOEM(refValue, refValue);
#endif // USE_UTF8
			pCodecCtx->PushDecString(pValue);
			refStr = pValue;
			result += refValue.length();
		}
	}
	else
	{
		uint16_t nLen;
		result = pInput->ReadUInt16BE(&nLen);
		if (result < 2)
		{
			return 0;
		}
		AMFVarPtr pValue(new AMFString());
		BCPString &refValue = AMFCast<AMFString>(pValue)->GetValue();
		refValue.clear();
		if (pInput->RemainingLength() < nLen)
		{
			//ASSERT(0);
			//return 0;
			nLen = (uint16_t)pInput->RemainingLength();
			bMalformed = TRUE;
		}
		pInput->Read(refValue.GetWriteBuffer(nLen), nLen);
		refValue.UngetWriteBuffer(nLen);
		refStr = pValue;
		result += refValue.length();
	}
	if (bMalformed)
	{
		LogError(_LOCAL_, "Malformed AMF0 string");
	}
	return result;
}

int32_t AMFCodec::EncodeString(
	BCOStream *pOutput, 
	const BCPString &refStr,
	bool bHasType /*= false*/)
{
	uint32_t nLen;
	int32_t result = 0;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
#ifdef USE_UTF8
	BCPString strUtf8;
#endif

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	// Allow empty string
#ifdef USE_UTF8
	nLen = BCOEMToUtf8(refStr, strUtf8);
#else
	nLen = refStr.Len();
#endif
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;
		int32_t nIndex;

		if (bHasType)
		{
			result += pOutput->WriteUInt8(AMF3_STRING);
		}
		nIndex = pCodecCtx->GetStringIndex(refStr);
		if (nIndex >= 0) // Is reference
		{
			nMask = MakeReference(nIndex, true);
			result += AMFCodec::EncodeVLUInt32(pOutput, nMask);
		} 
		else
		{
			nMask = MakeReference(nLen, false);
			result += AMFCodec::EncodeVLUInt32(pOutput, nMask);
			if (nLen)
			{
#ifdef USE_UTF8
				result += pOutput->WriteStringExact(strUtf8);
#else // !USE_UTF8
				result += pOutput->WriteStringExact(refStr);
#endif // USE_UTF8
				pCodecCtx->PushEncString(refStr);
			}
		}
	} 
	else
	{
		if (nLen < 0xFFFF)
		{
			if (bHasType)
			{
				result += pOutput->WriteUInt8(AMF0_STRING);
			}
			// UTF8 String
			result += pOutput->WriteUInt16BE(nLen);
#ifdef USE_UTF8
			result += pOutput->WriteStringExact(strUtf8);
#else
			result += pOutput->WriteStringExact(refStr);
#endif
		} 
		else
		{
			if (bHasType)
			{
				result += pOutput->WriteUInt8(AMF0_LSTRING);
			}
			// UTF8 Long String
			result += pOutput->WriteUInt32BE(nLen);
#ifdef USE_UTF8
			result += pOutput->WriteStringExact(strUtf8);
#else
			result += pOutput->WriteStringExact(refStr);
#endif
		}
	}
	return result;
}

int32_t AMFCodec::EncodeString(
	BCOStream *pOutput, 
	const AMFVarPtr &pStr,
	bool bHasType /*= false*/)
{
	if (!pStr)
	{
		return pOutput->WriteUInt8(0x01);
	}
	else if (pStr->GetType() != AMF_STRING)
	{
		ASSERT(0);
		return 0;
	}
	uint32_t nLen;
	int32_t result = 0;
	AMFCodecCtx *pCodecCtx;
	long eEncoding;
#ifdef USE_UTF8
	BCPString strUtf8;
#endif
	BCPString &refStr = AMFCast<AMFString>(pStr)->GetValue();

	pCodecCtx = (AMFCodecCtx *)pOutput->GetUserData();
	ASSERT(pCodecCtx);
	eEncoding = pCodecCtx->GetEncoding();
	// Allow empty string
#ifdef USE_UTF8
	nLen = BCOEMToUtf8(refStr, strUtf8);
#else
	nLen = refStr.Len();
#endif
	if (eEncoding == ObjectEncoding::AMF3)
	{
		uint32_t nMask;
		int32_t nIndex;

		if (bHasType)
		{
			result += pOutput->WriteUInt8(AMF3_STRING);
		}
		nIndex = pCodecCtx->GetStringIndex(refStr);
		if (nIndex >= 0) // Is reference
		{
			nMask = MakeReference(nIndex, true);
			result += AMFCodec::EncodeVLUInt32(pOutput, nMask);
		} 
		else
		{
			nMask = MakeReference(nLen, false);
			result += AMFCodec::EncodeVLUInt32(pOutput, nMask);
			if (nLen)
			{
#ifdef USE_UTF8
				result += pOutput->WriteStringExact(strUtf8);
#else // !USE_UTF8
				result += pOutput->WriteStringExact(refStr);
#endif // USE_UTF8
				pCodecCtx->PushEncString(refStr);
			}
		}
	} 
	else
	{
		if (nLen < 0xFFFF)
		{
			if (bHasType)
			{
				result += pOutput->WriteUInt8(AMF0_STRING);
			}
			// UTF8 String
			result += pOutput->WriteUInt16BE(nLen);
#ifdef USE_UTF8
			result += pOutput->WriteStringExact(strUtf8);
#else
			result += pOutput->WriteStringExact(refStr);
#endif
		} 
		else
		{
			if (bHasType)
			{
				result += pOutput->WriteUInt8(AMF0_LSTRING);
			}
			// UTF8 Long String
			result += pOutput->WriteUInt32BE(nLen);
#ifdef USE_UTF8
			result += pOutput->WriteStringExact(strUtf8);
#else
			result += pOutput->WriteStringExact(refStr);
#endif
		}
	}
	return result;
}

//////////////////////////////////////////////////////////////////////////
/// end of namespace : AMF
//////////////////////////////////////////////////////////////////////////

} // End of namespace AMF

///////////////////////////////////////////////////////////////////////////////
// End of file : AMF.cpp
///////////////////////////////////////////////////////////////////////////////
