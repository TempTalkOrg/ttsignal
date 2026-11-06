
#ifndef BC_USERDATA_H_INCLUDED__
#define BC_USERDATA_H_INCLUDED__

#ifdef _MSC_VER
#pragma warning(disable : 4311) // cutting off in converting 'const void *' to 'LONG'
#pragma warning(disable : 4312) // convert 'LONG' to bigger 'PVOID'
#endif // _MSC_VER

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

#include <BC/Exports.h>

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// BCUserData
///////////////////////////////////////////////////////////////////////////////

class BC_API BCUserData
{
public:

	void *GetUserPtr() const
	{
		//lint -e{50} Attempted to take the address of a non-lvalue
		return (void *)m_pUserData;
	}

	void SetUserPtr(void *pData)
	{
		//lint -e{534} Ignoring return value of function
		//lint -e{522} Expected void type, assignment, increment or decrement
		m_pUserData = (uint64_t)pData;
	}

	uint64_t GetUserData() const
	{
		return m_pUserData;
	}

	void SetUserData(uint64_t data)
	{
		m_pUserData = data;
	}

protected :

	BCUserData() : m_pUserData(0)
	{
		//
	}

	virtual ~BCUserData()
	{
		m_pUserData = 0;
	}

private :

	uint64_t m_pUserData;

	// No copies do not implement
	DECLARE_NO_COPY_CLASS(BCUserData);
};

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_USERDATA_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////

