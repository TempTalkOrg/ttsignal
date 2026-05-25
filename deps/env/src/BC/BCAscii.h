

#ifndef BC_BCASCII_H_INCLUDED__
#define BC_BCASCII_H_INCLUDED__

#include "BC/Exports.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : BCAscii
//
/// This class contains enumerations and static
/// utility functions for dealing with ASCII characters
/// and their properties.
///
/// The classification functions will also work if
/// non-ASCII character codes are passed to them,
/// but classification will only check for
/// ASCII characters.
///
/// This allows the classification methods to be used
/// on the single bytes of a UTF-8 string, without
/// causing assertions or inconsistent results (depending
/// upon the current locale) on bytes outside the ASCII range,
/// as may be produced by BCAscii::IsSpace(), etc.
///////////////////////////////////////////////////////////////////////////////

class BC_API BCAscii
{
public:
	enum CharacterProperties
		/// ASCII character properties.
	{
		ACP_CONTROL  = 0x0001,
		ACP_SPACE    = 0x0002,
		ACP_PUNCT    = 0x0004,
		ACP_DIGIT    = 0x0008,
		ACP_HEXDIGIT = 0x0010,
		ACP_ALPHA    = 0x0020,
		ACP_LOWER    = 0x0040,
		ACP_UPPER    = 0x0080,
		ACP_GRAPH    = 0x0100,
		ACP_PRINT    = 0x0200
	};

	static int GetProperties(int ch);
		/// Return the ASCII character properties for the
		/// character with the given ASCII value.
		///
		/// If the character is outside the ASCII range
		/// (0 .. 127), 0 is returned.

	static bool HasSomeProperties(int ch, int properties);
		/// Returns true if the given character is
		/// within the ASCII range and has at least one of
		/// the given properties.

	static bool HasProperties(int ch, int properties);
		/// Returns true if the given character is
		/// within the ASCII range and has all of
		/// the given properties.

	static bool IsAscii(int ch);
		/// Returns true iff the given character code is within
		/// the ASCII range (0 .. 127).

	static bool IsSpace(int ch);
		/// Returns true iff the given character is a whitespace.

	static bool IsDigit(int ch);
		/// Returns true iff the given character is a digit.

	static bool IsHexDigit(int ch);
		/// Returns true iff the given character is a hexadecimal digit.

	static bool IsPunct(int ch);
		/// Returns true iff the given character is a punctuation character.

	static bool IsAlpha(int ch);
		/// Returns true iff the given character is an alphabetic character.

	static bool IsAlphaNumeric(int ch);
		/// Returns true iff the given character is an alphabetic character.

	static bool IsLower(int ch);
		/// Returns true iff the given character is a lowercase alphabetic
		/// character.

	static bool IsUpper(int ch);
		/// Returns true iff the given character is an uppercase alphabetic
		/// character.

	static int ToLower(int ch);
		/// If the given character is an uppercase character,
		/// return its lowercase counterpart, otherwise return
		/// the character.

	static int ToUpper(int ch);
		/// If the given character is a lowercase character,
		/// return its uppercase counterpart, otherwise return
		/// the character.

private:
	DECLARE_NO_COPY_CLASS(BCAscii);
	static const int CHARACTER_PROPERTIES[128];
};


//
// inlines
//
inline int BCAscii::GetProperties(int ch)
{
	if (IsAscii(ch))
		return CHARACTER_PROPERTIES[ch];
	else
		return 0;
}


inline bool BCAscii::IsAscii(int ch)
{
	return (static_cast<uint32_t>(ch) & 0xFFFFFF80) == 0;
}


inline bool BCAscii::HasProperties(int ch, int props)
{
	return (GetProperties(ch) & props) == props;
}


inline bool BCAscii::HasSomeProperties(int ch, int props)
{
	return (GetProperties(ch) & props) != 0;
}


inline bool BCAscii::IsSpace(int ch)
{
	return HasProperties(ch, ACP_SPACE);
}


inline bool BCAscii::IsDigit(int ch)
{
	return HasProperties(ch, ACP_DIGIT);
}


inline bool BCAscii::IsHexDigit(int ch)
{
	return HasProperties(ch, ACP_HEXDIGIT);
}


inline bool BCAscii::IsPunct(int ch)
{
	return HasProperties(ch, ACP_PUNCT);
}


inline bool BCAscii::IsAlpha(int ch)
{
	return HasProperties(ch, ACP_ALPHA);
}


inline bool BCAscii::IsAlphaNumeric(int ch)
{
	return HasSomeProperties(ch, ACP_ALPHA | ACP_DIGIT);
}


inline bool BCAscii::IsLower(int ch)
{
	return HasProperties(ch, ACP_LOWER);
}


inline bool BCAscii::IsUpper(int ch)
{
	return HasProperties(ch, ACP_UPPER);
}


inline int BCAscii::ToLower(int ch)
{
	if (IsUpper(ch))
		return ch + 32;
	else
		return ch;
}


inline int BCAscii::ToUpper(int ch)
{
	if (IsLower(ch))
		return ch - 32;
	else
		return ch;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // namespace BC

#endif // BC_BCASCII_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
