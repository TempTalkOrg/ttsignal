
#ifndef BC_BYTEORDER_INCLUDE__
#define BC_BYTEORDER_INCLUDE__

#include "BC/Exports.h"
#include <stdint.h>

#include "build/build_config.h"

#if defined(COMPILER_MSVC)
#include <stdlib.h>
#endif

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

// Returns a value with all bytes in |x| swapped, i.e. reverses the endianness.
inline uint16_t ByteSwap(uint16_t x) {
#if defined(COMPILER_MSVC)
	return _byteswap_ushort(x);
#else
	return __builtin_bswap16(x);
#endif
}

inline uint32_t ByteSwap(uint32_t x) {
#if defined(COMPILER_MSVC)
	return _byteswap_ulong(x);
#else
	return __builtin_bswap32(x);
#endif
}

inline uint64_t ByteSwap(uint64_t x) {
#if defined(COMPILER_MSVC)
	return _byteswap_uint64(x);
#else
	return __builtin_bswap64(x);
#endif
}

// Converts the bytes in |x| from host order (endianness) to little endian, and
// returns the result.
inline uint16_t ByteSwapToLE16(uint16_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return x;
#else
	return ByteSwap(x);
#endif
}
inline uint32_t ByteSwapToLE32(uint32_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return x;
#else
	return ByteSwap(x);
#endif
}
inline uint64_t ByteSwapToLE64(uint64_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return x;
#else
	return ByteSwap(x);
#endif
}

// Converts the bytes in |x| from network to host order (endianness), and
// returns the result.
inline uint16_t NetToHost16(uint16_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}
inline uint32_t NetToHost32(uint32_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}
inline uint64_t NetToHost64(uint64_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}

// Converts the bytes in |x| from host to network order (endianness), and
// returns the result.
inline uint16_t HostToNet16(uint16_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}
inline uint32_t HostToNet32(uint32_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}
inline uint64_t HostToNet64(uint64_t x) {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
	return ByteSwap(x);
#else
	return x;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// class : ByteOrder
///////////////////////////////////////////////////////////////////////////////

class BC_API ByteOrder
{
public:
	/// swap two values
	/// the type T must have an assignment operator (=)
	template<class T>
	static void swapValues(T& t1, T& t2)
	{
		T tmp = t1;
		t1 = t2;
		t2 = tmp;
	}
	static uint16_t		swapBytesShort(uint16_t val);
	static uint32_t		swapBytesLong(uint32_t val);
	static uint64_t		swapBytesLongLong(uint64_t val);
	static float32_t		swapBytesFloat(float32_t val);
	static float64_t		swapBytesDouble(float64_t val);
private:
	DECLARE_NO_COPY_CLASS(ByteOrder);
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

};
#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
