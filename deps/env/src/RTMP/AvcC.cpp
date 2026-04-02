///////////////////////////////////////////////////////////////////////////////
// File : AvcC.cpp
///////////////////////////////////////////////////////////////////////////////

#include <math.h>
#include <stdint.h>
#include <BC/Utils.h>
#include <BC/BCMemPool.h>
#include "AvcC.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : 
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// class : AvcC
///////////////////////////////////////////////////////////////////////////////

AvcC::AvcC()
{
	//
}

AvcC::~AvcC()
{
	//
}

const uint8_t *AvcC::_FindStartCodeInternal(
	const uint8_t *p,
	const uint8_t *end)
{
	const uint8_t *a = p + 4 - ((intptr_t)p & 3);

	for (end -= 3; p < a && p < end; p++) {
		if (p[0] == 0 && p[1] == 0 && p[2] == 1)
			return p;
	}

	for (end -= 3; p < end; p += 4) {
		uint32_t x = *(const uint32_t*)p;
		//      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
		//      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
		if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
			if (p[1] == 0) {
				if (p[0] == 0 && p[2] == 1)
					return p;
				if (p[2] == 0 && p[3] == 1)
					return p + 1;
			}
			if (p[3] == 0) {
				if (p[2] == 0 && p[4] == 1)
					return p + 2;
				if (p[4] == 0 && p[5] == 1)
					return p + 3;
			}
		}
	}

	for (end += 3; p < end; p++) {
		if (p[0] == 0 && p[1] == 0 && p[2] == 1)
			return p;
	}

	return end + 3;
}

const uint8_t *AvcC::FindStartCode(const uint8_t *p, const uint8_t *end) 
{
	const uint8_t *out = _FindStartCodeInternal(p, end);
	if (p < out && out < end && !out[-1]) out--;
	return out;
}

int32_t AvcC::ParseNalUnits(
	const uint8_t *buf_in,
	int32_t size,
	BCOStream &refWriter)
{
	const uint8_t *p = buf_in;
	const uint8_t *end = p + size;
	const uint8_t *nal_start, *nal_end;

	size = 0;
	nal_start = FindStartCode(p, end);
	for (;;)
	{
		while (nal_start < end && !*(nal_start++));
		if (nal_start == end)
			break;

		nal_end = FindStartCode(nal_start, end);
		refWriter.WriteUInt32BE(nal_end - nal_start);
		refWriter.Write(nal_start, nal_end - nal_start);
		size += 4 + nal_end - nal_start;
		nal_start = nal_end;
	}
	return size;
}

int32_t AvcC::GetAvcC(
	const uint8_t *data,
	int32_t len,
	BCBuffer &refBAOut)
{
	if (len > 6)
	{
		BCBOStream sWriter(&refBAOut);
		/* check for h264 start code */
		if (BC_UI32BEI(data) == 0x00000001 || BC_UI24BEI(data) == 0x000001)
		{
			uint8_t *buf = NULL, *end;
			uint32_t sps_size = 0, pps_size = 0;
			uint8_t *sps = 0, *pps = 0;
			BCBuffer sByteArray;
			BCBOStream sBuffer(&sByteArray);

			len = ParseNalUnits(data, len, sBuffer);
			buf = (LPBYTE)sByteArray.Base();
			end = buf + len;

			/* look for sps and pps */
			while (end - buf > 4)
			{
				uint32_t size;
				uint8_t nal_type;
				size = BC_UI32BEI(buf);
				size = BCMIN(size, end - buf - 4);
				buf += 4;
				nal_type = buf[0] & 0x1f;
				if (nal_type == 7) /* SPS */
				{
					sps = buf;
					sps_size = size;
				}
				else if (nal_type == 8) /* PPS */
				{
					pps = buf;
					pps_size = size;
				}
				buf += size;
			}
			if (!sps || !pps || sps_size < 4 || sps_size > UINT16_MAX || pps_size > UINT16_MAX)
				return -1;

			sWriter.WriteUInt8(1); /* version */
			sWriter.WriteUInt8(sps[1]); /* profile */
			sWriter.WriteUInt8(sps[2]); /* profile compat */
			sWriter.WriteUInt8(sps[3]); /* level */
			sWriter.WriteUInt8(0xff); /* 6 bits reserved (111111) + 2 bits nal size length - 1 (11) */
			sWriter.WriteUInt8(0xe1); /* 3 bits reserved (111) + 5 bits number of sps (00001) */

			sWriter.WriteUInt16BE(sps_size);
			sWriter.Write(sps, sps_size);
			sWriter.WriteUInt8(1); /* number of pps */
			sWriter.WriteUInt16BE(pps_size);
			sWriter.Write(pps, pps_size);
		}
		else
		{
			sWriter.Write(data, len);
		}
	}
	return 0;
}

bool AvcC::GetSPSPPS(const uint8_t *data, int32_t len, BCOStream &refWriter)
{
	BCFBIStream sReader(data, len);
	sReader.Skip(6);
	uint16_t nLen;
	sReader.ReadUInt16BE(&nLen); // sps size
	refWriter.WriteUInt32BE(1);
	refWriter.WriteFrom(sReader, nLen); // sps
	sReader.Skip(1); // number of pps
	sReader.ReadUInt16BE(&nLen); // pps size
	refWriter.WriteUInt32BE(1);
	refWriter.WriteFrom(sReader, nLen); // pps
	return true;
}

bool AvcC::DecodeAvcC(
	BYTE * buf,
	uint32_t nLen,
	uint32_t &width,
	uint32_t &height)
{
	BCFBIStream sReader(buf, nLen);
	sReader.Skip(6);
	uint16_t nSPSLen;
	sReader.ReadUInt16BE(&nSPSLen);
	return DecodeH264SPS((LPBYTE)sReader.Current(), nSPSLen, width, height);
}

BCRESULT AvcC::GetAnnexbExtradata(const uint8_t *in, uint8_t **buf, int *size)
{
    uint16_t sps_size, pps_size;
    uint8_t *out;
    int out_size;

    *buf = NULL;
    if (*size >= 4 && (BC_UI32BEI(in) == 0x00000001 || BC_UI24BEI(in) == 0x000001))
        return 0;
    if (*size < 11 || in[0] != 1)
        return BC_R_INVALIDARG;

    sps_size = BC_UI16BEI(&in[6]);
    if (11 + sps_size > *size)
	{
        return BC_R_INVALIDARG;
	}
	pps_size = BC_UI16BEI(&in[9 + sps_size]);
    if (11 + sps_size + pps_size > *size)
	{
        return BC_R_INVALIDARG;
	}
    out_size = 8 + sps_size + pps_size;
    out = (uint8_t *)malloc(out_size);
    if (!out)
	{
        return BC_R_NOMEMORY;
	}
	memzero(out, out_size);
    BC_UI32BEO(&out[0], 0x00000001);
    memcpy(out + 4, &in[8], sps_size);
    BC_UI32BEO(&out[4 + sps_size], 0x00000001);
    memcpy(out + 8 + sps_size, &in[11 + sps_size], pps_size);
    *buf = out;
    *size = out_size;
    return BC_R_SUCCESS;
}

uint32_t AvcC::_ReadUe(BYTE *pBuff, uint32_t nLen, uint32_t &nStartBit)
{
	//����0bit�ĸ���
	uint32_t nZeroNum = 0;
	while (nStartBit < nLen * 8)
	{
		if (pBuff[nStartBit / 8] & (0x80 >> (nStartBit % 8))) //&:��λ�룬%ȡ��
		{
			break;
		}
		nZeroNum++;
		nStartBit++;
	}
	nStartBit++;

	//������
	uint32_t dwRet = 0;
	for (uint32_t i=0; i<nZeroNum; i++)
	{
		dwRet <<= 1;
		if (pBuff[nStartBit / 8] & (0x80 >> (nStartBit % 8)))
		{
			dwRet += 1;
		}
		nStartBit++;
	}
	return (1 << nZeroNum) - 1 + dwRet;
}

int AvcC::_ReadSe(BYTE *pBuff, uint32_t nLen, uint32_t &nStartBit)
{
	int UeVal = _ReadUe(pBuff,nLen,nStartBit);
	double k=UeVal;
	int nValue=ceil(k/2);//ceil������ceil��������������С�ڸ���ʵ������С������ceil(2)=ceil(1.2)=cei(1.5)=2.00
	if (UeVal % 2==0)
		nValue=-nValue;
	return nValue;
}


uint32_t AvcC::_ReadBit(uint32_t BitCount,BYTE * buf,uint32_t &nStartBit)
{
	uint32_t dwRet = 0;
	for (uint32_t i=0; i<BitCount; i++)
	{
		dwRet <<= 1;
		if (buf[nStartBit / 8] & (0x80 >> (nStartBit % 8)))
		{
			dwRet += 1;
		}
		nStartBit++;
	}
	return dwRet;
}

bool AvcC::DecodeH264SPS(BYTE * buf, uint32_t nLen,uint32_t &width,uint32_t &height)
{
	uint32_t StartBit=0; 
	int forbidden_zero_bit=_ReadBit(1,buf,StartBit);
	int nal_ref_idc=_ReadBit(2,buf,StartBit);
	int nal_unit_type=_ReadBit(5,buf,StartBit);
	if(nal_unit_type==7)
	{
		int profile_idc=_ReadBit(8,buf,StartBit);
		int constraint_set0_flag=_ReadBit(1,buf,StartBit);//(buf[1] & 0x80)>>7;
		int constraint_set1_flag=_ReadBit(1,buf,StartBit);//(buf[1] & 0x40)>>6;
		int constraint_set2_flag=_ReadBit(1,buf,StartBit);//(buf[1] & 0x20)>>5;
		int constraint_set3_flag=_ReadBit(1,buf,StartBit);//(buf[1] & 0x10)>>4;
		int reserved_zero_4bits=_ReadBit(4,buf,StartBit);
		int level_idc=_ReadBit(8,buf,StartBit);

		int seq_parameter_set_id=_ReadUe(buf,nLen,StartBit);

		if( profile_idc == 100 || profile_idc == 110 ||
			profile_idc == 122 || profile_idc == 144 )
		{
			int chroma_format_idc=_ReadUe(buf,nLen,StartBit);
			if( chroma_format_idc == 3 )
				int residual_colour_transform_flag=_ReadBit(1,buf,StartBit);
			int bit_depth_luma_minus8=_ReadUe(buf,nLen,StartBit);
			int bit_depth_chroma_minus8=_ReadUe(buf,nLen,StartBit);
			int qpprime_y_zero_transform_bypass_flag=_ReadBit(1,buf,StartBit);
			int seq_scaling_matrix_present_flag=_ReadBit(1,buf,StartBit);

			int seq_scaling_list_present_flag[8];
			if( seq_scaling_matrix_present_flag )
			{
				for( int i = 0; i < 8; i++ ) {
					seq_scaling_list_present_flag[i]=_ReadBit(1,buf,StartBit);
				}
			}
		}
		int log2_max_frame_num_minus4=_ReadUe(buf,nLen,StartBit);
		int pic_order_cnt_type=_ReadUe(buf,nLen,StartBit);
		if( pic_order_cnt_type == 0 )
			int log2_max_pic_order_cnt_lsb_minus4=_ReadUe(buf,nLen,StartBit);
		else if( pic_order_cnt_type == 1 )
		{
			int delta_pic_order_always_zero_flag=_ReadBit(1,buf,StartBit);
			int offset_for_non_ref_pic=_ReadSe(buf,nLen,StartBit);
			int offset_for_top_to_bottom_field=_ReadSe(buf,nLen,StartBit);
			int num_ref_frames_in_pic_order_cnt_cycle=_ReadUe(buf,nLen,StartBit);

			KBPool sPool;
			int *offset_for_ref_frame= (int *)sPool.Calloc(sizeof(int)*num_ref_frames_in_pic_order_cnt_cycle);
			for( int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++ )
				offset_for_ref_frame[i]=_ReadSe(buf,nLen,StartBit);
		}
		int num_ref_frames=_ReadUe(buf,nLen,StartBit);
		int gaps_in_frame_num_value_allowed_flag=_ReadBit(1,buf,StartBit);
		int pic_width_in_mbs_minus1=_ReadUe(buf,nLen,StartBit);
		int pic_height_in_map_units_minus1 = _ReadUe(buf, nLen, StartBit);
		int frame_mbs_only_flag = _ReadBit(1, buf, StartBit);
		if (!frame_mbs_only_flag)
		{
			int mb_adaptive_frame_field_flag = _ReadBit(1, buf, StartBit);
		}
		int direct_8x8_inference_flag = _ReadBit(1, buf, StartBit);
		int frame_cropping_flag = _ReadBit(1, buf, StartBit);
		if (frame_cropping_flag)
		{
			uint32_t frame_crop_left_offset = _ReadUe(buf, nLen, StartBit);
			uint32_t frame_crop_right_offset = _ReadUe(buf, nLen, StartBit);
			uint32_t frame_crop_top_offset = _ReadUe(buf, nLen, StartBit);
			uint32_t frame_crop_bottom_offset = _ReadUe(buf, nLen, StartBit);
			width = ((pic_width_in_mbs_minus1 + 1) * 16) - frame_crop_right_offset * 2 - frame_crop_left_offset * 2;
			height = ((2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 + 1) * 16) - (frame_crop_top_offset * 2) - (frame_crop_bottom_offset * 2);
		}
		else
		{
			width = (pic_width_in_mbs_minus1 + 1) * 16;
			height = (2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 + 1) * 16;
		}

		return true;
	}
	else
	{
		return false;
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : AvcC.cpp
///////////////////////////////////////////////////////////////////////////////
