
#include "ByteOrder.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace :
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : ByteOrder
///////////////////////////////////////////////////////////////////////////////

uint16_t ByteOrder::swapBytesShort(uint16_t val)
{
#if 1
	uint8_t octets[2];
	memcpy(octets,&val,2);
	swapValues(octets[0],octets[1]);
	return *(uint16_t *)octets;
#else
	return ByteSwap(val);
#endif

}
uint32_t ByteOrder::swapBytesLong(uint32_t val)
{
#if 1
	uint8_t octets[4];
	memcpy(octets,&val,4);
	swapValues(octets[0],octets[3]);
	swapValues(octets[1],octets[2]);
	return *(uint32_t *)octets;
#else
	return ByteSwap(val);
#endif

}
uint64_t ByteOrder::swapBytesLongLong(uint64_t val)
{
#if 1
	uint8_t octets[8];
	memcpy(octets,&val,8);
	swapValues(octets[0],octets[7]);
	swapValues(octets[1],octets[6]);
	swapValues(octets[2],octets[5]);
	swapValues(octets[3],octets[4]);
	return *(uint64_t *)octets;
#else
	return ByteSwap(val);
#endif

}
float32_t ByteOrder::swapBytesFloat(float32_t val)
{
	uint8_t octets[4];
	float32_t newVal;

	memcpy(octets,&val,4);
	swapValues(octets[0],octets[3]);
	swapValues(octets[1],octets[2]);
	memcpy(&newVal, octets, 4);
	return newVal;

}
float64_t ByteOrder::swapBytesDouble(float64_t val)
{
	uint8_t octets[8];
	float64_t newVal;

	memcpy(octets,&val,8);
	swapValues(octets[0],octets[7]);
	swapValues(octets[1],octets[6]);
	swapValues(octets[2],octets[5]);
	swapValues(octets[3],octets[4]);
    memcpy(&newVal, octets, 8);

	return newVal;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////
};

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
