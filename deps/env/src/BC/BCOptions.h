
///////////////////////////////////////////////////////////////////////////////
// File : BCOptions.h
///////////////////////////////////////////////////////////////////////////////

#ifndef BC_BCOPTIONS_H_INCLUDED__
#define BC_BCOPTIONS_H_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCNodeList.h>
#include <BC/BCUserData.h>
#include <BC/BCFCodec.h>
#include <BC/BCList.h>
#include <BC/BCHashTable.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros
///////////////////////////////////////////////////////////////////////////////

#define BC_OPTION_DEFINE(opt)	\
	extern BCDLLEXPORT_DATA(LPCSTR)	opt;

#define BC_OPTION_IMPLEMENT(opt)	\
	BCDLLEXPORT_DATA(LPCSTR)	opt = #opt;

#define BC_GET_INT_OPTION(_key, _value)    \
	pValue = pOptions->GetOption(_key);    \
	if (!IS_BCF_INT(pValue)){              \
		BC_SAFE_DELETE_PTR(pValue);        \
		return BC_R_INVALIDARG;            \
	} else {                               \
		_value = GET_BCF_INT(pValue);      \
		BC_SAFE_DELETE_PTR(pValue);        \
	}

#define BC_GET_DOUBLE_OPTION(_key, _value) \
	pValue = pOptions->GetOption(_key);    \
	if (!IS_BCF_DOUBLE(pValue)){           \
		BC_SAFE_DELETE_PTR(pValue);        \
		return BC_R_INVALIDARG;            \
	} else {                               \
		_value = GET_BCF_DOUBLE(pValue);   \
		BC_SAFE_DELETE_PTR(pValue);        \
	}

#define BC_GET_BOOL_OPTION(_key, _value)   \
	pValue = pOptions->GetOption(_key);    \
	if (!IS_BCF_BOOL(pValue)){             \
		BC_SAFE_DELETE_PTR(pValue);        \
		return BC_R_INVALIDARG;            \
	} else {                               \
		_value = GET_BCF_BOOL(pValue);     \
		BC_SAFE_DELETE_PTR(pValue);        \
	}

#define BC_GET_STRING_OPTION(_key, _value)                \
	pValue = pOptions->GetOption(_key);                   \
	if (!IS_BCF_NULL(pValue) && !IS_BCF_STRING(pValue)) { \
		BC_SAFE_DELETE_PTR(pValue);                       \
		return BC_R_INVALIDARG;                           \
	} else {                                              \
		if (IS_BCF_STRING(pValue)) {                      \
			_value = GET_BCF_STRING(pValue).c_str();      \
		} else {                                          \
			_value = NULLPSTRING;                         \
		}                                                 \
		BC_SAFE_DELETE_PTR(pValue);                       \
	}

#define BC_GET_ARRAY_OPTION(_key, _value)  \
	pValue = pOptions->GetOption(_key);    \
	if (!IS_BCF_ARRAY(pValue)){            \
		BC_SAFE_DELETE_PTR(pValue);        \
		return BC_R_INVALIDARG;            \
	} else {                               \
		BC_SAFE_DELETE_PTR(_value);        \
		_value = (BCFArray *)pValue;       \
	}

///////////////////////////////////////////////////////////////////////////////
// Class : BCOptionListener
///////////////////////////////////////////////////////////////////////////////

class BC_API BCOptionListener
{
public:
	BCOptionListener(){};
	virtual ~BCOptionListener(){};

	virtual void	OnOptionChanged(
						const BCPString &strKey, 
						BCFVar *pValue)	= 0;
private:
	DECLARE_NO_COPY_CLASS(BCOptionListener);
};

///////////////////////////////////////////////////////////////////////////////
// Class : BCOptionItem
///////////////////////////////////////////////////////////////////////////////

class BC_API BCOptionItem
{
public:
	BCOptionItem();
	~BCOptionItem();

	BCRESULT		Create(LPCSTR lpszKey);
	BCRESULT		SetOption(BCFVar *pOption);
	inline BCFVar*	GetOption() const
	{
		return m_pOption;
	}
	BCRESULT		AddListener(BCOptionListener *pListener);
	BCRESULT		RemoveListener(BCOptionListener *pListener);

protected:
private:
	DECLARE_NO_COPY_CLASS(BCOptionItem);
	typedef BCList<BCOptionListener *> ListenerList;
	BCSpinMutex			m_sOptionLock;
	LPCSTR				m_lpszKey;
	BCFVar			*	m_pOption;
	ListenerList		m_lstListeners;
};

///////////////////////////////////////////////////////////////////////////////
// Class : BCOptions
///////////////////////////////////////////////////////////////////////////////

class BC_API BCOptions
{
public:
	BCOptions();
	~BCOptions();

	BCRESULT			Create();
	BCRESULT			SetIntOption(LPCSTR lpszKey, uint32_t nValue);
	BCRESULT			SetBooleanOption(LPCSTR lpszKey, bool bValue);
	BCRESULT			SetDoubleOption(LPCSTR lpszKey, float64_t dblValue);
	BCRESULT			SetStringOption(LPCSTR lpszKey, LPCSTR lpszValue);
	BCRESULT			SetOption(LPCSTR lpszKey, BCFVar *pValue);
	BCFVar			*	GetOption(LPCSTR lpszKey);
	BCRESULT			AddListener(
							LPCSTR lpszKey, 
							BCOptionListener *pListener,
							BOOL bCreateIfNotExists = FALSE);
	BCRESULT			RemoveListener(LPCSTR lpszKey, BCOptionListener *pListener);
	void				Clear();

protected:
	BCOptionItem	*	_EnsureOptionExists(LPCSTR lpszKey);
private:
	DECLARE_NO_COPY_CLASS(BCOptions);
	BCSpinMutex			m_sOptionLock;
	KBPool				m_sPool;
	BCStrHashTable		m_htOptions;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

#endif // BC_BCOPTIONS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : BCOptions.h
///////////////////////////////////////////////////////////////////////////////
