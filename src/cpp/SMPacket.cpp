




#include "StdAfx.h"
#include <inttypes.h> 
#include "BC/Utils.h"
#include "SMPacket.h"
#include "Runtime.h"
#include "Utils.h"
#include "SMPParser.h"






namespace SMP
{










IMPLEMENT_FIXED_ALLOC(SMPacket, 32);

SMPacket::SMPacket()
	: type(0)
	, timestamp(0)
	, trans_id(0)
	, stream_id(0)
	, origion_data(NULL)
	, packed_jmp_data(NULL)
{
}

BCRESULT SMPacket::Create(BufferPtr data)
{
	origion_data = data;
	return BC_R_SUCCESS;
}

size_t SMPacket::Write(LPCVOID data, size_t size)
{
	if (!origion_data)
	{
		origion_data.reset(new BCBuffer);
	}
	return origion_data->Write(data, size);
}

BCRESULT SMPacket::PackPacket()
{
	return SMPParser::PackPacket(shared_from_this());
}





} 




