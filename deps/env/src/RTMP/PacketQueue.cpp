///////////////////////////////////////////////////////////////////////////////
// file : PacketQueue.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#include <cstdlib> // for std::abs
#include <BC/base/atomic_ref_count.h>
#include <BC/BCLog.h>
#include <RTMP/FLVUtils.h>
#include <RTMP/IHandler.h>
#include <RTMP/AvcC.h>
#include "RTMP/RTPUtils.h"
#include <RTMP/PacketQueue.h>

using namespace Base;
using namespace FLVUtils;



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

const char *SZ_absRecordTime	= "absRecordTime";
const char *SZ_onMetaData		= "onMetaData";
const char *SZ_audiocodecid		= "audiocodecid";
const char *SZ_videocodecid		= "videocodecid";
const char *SZ_width			= "width";
const char *SZ_height			= "height";
const char *SZ_audiosamplerate	= "audiosamplerate";
const char *SZ_framerate		= "framerate";
const char *SZ_stereo			= "stereo";
const char *SZ_audiosamplesize	= "audiosamplesize";

///////////////////////////////////////////////////////////////////////////////
// File-scope macros & utilities
///////////////////////////////////////////////////////////////////////////////

static const char* pkt_type_to_string(int type)
{
	const char* type_str = "unknown";
	switch (type)
	{
	case MTYPE_METADATA:
		type_str = "metadata";
		break;
	case MTYPE_AUDIODATA:
		type_str = "audio";
		break;
	case MTYPE_VIDEODATA:
		type_str = "video";
		break;
	default:
		break;
	}
	return type_str;
}

///////////////////////////////////////////////////////////////////////////////
// class: PacketAnalyzer
///////////////////////////////////////////////////////////////////////////////

PacketAnalyzer::PacketAnalyzer()
	: m_pHandler(NULL)
	, m_nTotalTime(0)
	, m_nTotalAudTime(0)
	, m_nTotalVidTime(0)
	, m_nFirstSeqNumber(0)
	, m_nNextSeqNumber(0)
	, m_bNormalVidPkt(false)
	, m_bNormalAudPkt(false)
	, m_tmAbsRecordTime(0)
	, m_tmLatestKeyFrame(0)
	, m_nAVFlags(0)
	, m_nSampleRate(0)
	, m_nSamplesPerFrame(0)
	, m_nTotalAudPkt(0)
	, m_eAVFilterFlags(FILTER_FLAG_NONE)
	, m_nWidth(0)
	, m_nHeight(0)
{
	//
}

PacketAnalyzer::~PacketAnalyzer()
{
	Cleanup();
}

BCRESULT PacketAnalyzer::Create(
	LPCSTR lpszStreamName, 
	PAVQueueHandler *pHandler)
{
	m_strStreamName = lpszStreamName;
	m_pHandler = pHandler;
	return BC_R_SUCCESS;
}

void PacketAnalyzer::Cleanup()
{
	m_pMetaObj = AMFVarPtr();
}

uint32_t PacketAnalyzer::Analyze(PPacket *pPacket, uint32_t nLevel)
{
	PHeader &refHeader = *pPacket->m_pHeader;
	ScopedPointer<PPacket> dtor(pPacket);
	bool bKeyFrame = false;

	m_nTotalTime = refHeader.m_nTotalTime;

	switch(refHeader.m_eDataType)
	{
	case FLVUtils::PTYPE_AUDIODATA:
		{
			if (m_eAVFilterFlags & FILTER_FLAG_DISABLEAUDIO)
			{
				return m_nNextSeqNumber;
			}
			if (!(m_nAVFlags & AVINFO_HAS_AUDIO))
			{
				m_nAVFlags |= AVINFO_HAS_AUDIO;
			}

			if (refHeader.m_nDataSize >= 2)
			{
				uint8_t *pData;
				FLVAudioInfoS sAInfo;

				pData = (uint8_t *)pPacket->m_sBody.Current();
				sAInfo = FLVInfo::AnalyseAudio(pData[0]);
				switch (sAInfo.eCodecId)
				{
				case FLV_AAC_AUDIO:
					if (pData[1]== AACPTYPE_SEQHEADER)
					{
						// Log timestamp check
						if (m_eAVFilterFlags & FILTER_FLAG_LOGTIMESTAMP)
						{
							LogInfo(_LOCAL_, "[%s]Type : %2d(%s:%s); playback timestamp : %6d; "
								"datasize : %6d; accumulated time : %9" _S64BITARG_
								"; total time : %9" _S64BITARG_"; m_pHandler : 0x%p.\n",
								m_strStreamName.c_str(),
								refHeader.m_eDataType, pkt_type_to_string(refHeader.m_eDataType), 
								refHeader.IsAKFrame() ? "I" : "P", refHeader.m_nTimestamp, 
								refHeader.m_nDataSize, m_nTotalAudTime, refHeader.m_nTotalTime, m_pHandler);
							LogBuffer(_LOCAL_, &pPacket->m_sBody);
						}
						if (!m_pMetaObj)
						{
							m_pMetaObj = AMFVarPtr(new AMF0Object());
						}
						AMFVarPtr pAudioMeta(new AMF0Object());
						pPacket->Clone(m_sAACSeqHeader);
						m_sAACSeqHeader.m_pHeader->SetASeqHdr(true);
						m_sAACSeqHeader.m_pHeader->m_nChannelId = 0;
						// Notify AAC sequence header
						if (m_pHandler)
						{
							KBPool sPool;
							LPVOID lpBuf = sPool.Calloc(refHeader.m_nDataSize);
							BCBIStream sReader(&m_sAACSeqHeader.m_sBody);
							sReader.Read(lpBuf, refHeader.m_nDataSize);
							sReader.Rewind();
							AACConfigS config = FLVInfo::ParseAACSeqHeader((uint8_t *)pData + 2, 
								refHeader.m_nDataSize - 2);
							m_nSampleRate = config.sampleRate;//sampleRate[sInfo.nRate];
							m_nSamplesPerFrame  = config.frameLength;
							m_nAVFlags |= AVINFO_HAS_AACHDR;
							LogDebug(_LOCAL_, "Set av flags with AVINFO_HAS_AACHDR");
							m_pHandler->OnAACSeqHdr(lpBuf, refHeader.m_nDataSize);
							AMFCast<AMF0Object>(pAudioMeta)->PutString(SZ_audiocodecid, "mp4a");
							AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplerate, config.sampleRate);
							AMFCast<AMF0Object>(pAudioMeta)->PutBool(SZ_stereo, config.chconf != 1);
							AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplesize, 16);
							m_pHandler->OnAudioStart(pAudioMeta);
						}

						return 0;
					} 
					else
					{
						if (!(m_nAVFlags & AVINFO_HAS_AACHDR))
						{
							if (m_pHandler && !(m_nAVFlags & AVINFO_REPORT_NOAACHDR))
							{
								BCException sExcept(__FUNCTION__, "ENOAACHDR");
								m_pHandler->OnException(sExcept);
								m_nAVFlags |= AVINFO_REPORT_NOAACHDR;
								LogDebug(_LOCAL_, "Report ENOAACHDR error[%" _U32BITARG_ "]", m_nAVFlags);
							}
							return 0;
						}
					}
					break;
				case FLV_MP3_AUDIO:
					{
						if (!(m_nAVFlags & AVINFO_PARSE_MP3))
						{
							if (!m_pMetaObj)
							{
								m_pMetaObj = AMFVarPtr(new AMF0Object());
							}
							AMFVarPtr pAudioMeta(new AMF0Object());
							uint32_t nRemainingSize = pPacket->m_sBody.RemainingLength();
							MP3ConfigS config = FLVInfo::ParseMP3Info((uint8_t *)pData + 1,
								nRemainingSize - 1);
							m_nSampleRate = config.sampleRate;
							m_nSamplesPerFrame = config.samples;
							m_nAVFlags |= AVINFO_PARSE_MP3;
							if (m_pHandler)
							{
								m_pHandler->OnMP3Audio(pData, nRemainingSize);
								AMFCast<AMF0Object>(pAudioMeta)->PutString(SZ_audiocodecid, "mp3");
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplerate, config.sampleRate);
								AMFCast<AMF0Object>(pAudioMeta)->PutBool(SZ_stereo, sAInfo.eType != 0);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplesize, 16);
								m_pHandler->OnAudioStart(pAudioMeta);
							}
							if (config.frameSize > nRemainingSize - 1)
							{
								// drop invalid packet
								return 0;
							}
						}
					}
					break;
				case FLV_SPEEX_AUDIO:
					{
						if (!(m_nAVFlags & AVINFO_PARSE_SPEEX))
						{
							if (!m_pMetaObj)
							{
								m_pMetaObj = AMFVarPtr(new AMF0Object());
							}
							AMFVarPtr pAudioMeta(new AMF0Object());
							uint32_t nRemainingSize = pPacket->m_sBody.RemainingLength();
							//SpeexHeader config = FLVInfo::ParseSpeexHeader((uint8_t *)pData + 1,
							//	nRemainingSize - 1);
							//m_nSampleRate = config.rate;
							//m_nSamplesPerFrame = config.frame_size;
							//static uint32_t sampleRate[] = {5500, 11025, 22050, 44100};
							int spx_mode = 1;
							m_nSampleRate = 16000;
							m_nSamplesPerFrame = 160 << spx_mode;;
							m_nAVFlags |= AVINFO_PARSE_SPEEX;
							if (m_pHandler)
							{
								//m_pHandler->OnMP3Audio(pData, nRemainingSize);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiocodecid, FLV_SPEEX_AUDIO);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplerate, m_nSampleRate);
								AMFCast<AMF0Object>(pAudioMeta)->PutBool(SZ_stereo, sAInfo.eType != 0);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplesize, 16);
								m_pHandler->OnAudioStart(pAudioMeta);
							}
							//if (config.frame_size > nRemainingSize - 1)
							//{
							//	// drop invalid packet
							//	return 0;
							//}
						}
					}
					break;
				case FLV_DEVICE_SPECIFIC_SOUND_AUDIO:
					{
						if (!(m_nAVFlags & AVINFO_PARSE_DEVICE_SPECIFIC))
						{
							if (!m_pMetaObj)
							{
								m_pMetaObj = AMFVarPtr(new AMF0Object());
							}
							AMFVarPtr pAudioMeta(new AMF0Object());
							GetDeviceSpecificAudioCodecParameters(pPacket->m_sBody, 
								m_nSampleRate, m_nSamplesPerFrame);
							m_nAVFlags |= AVINFO_PARSE_DEVICE_SPECIFIC;
							if (m_pHandler)
							{
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiocodecid, 
									FLV_DEVICE_SPECIFIC_SOUND_AUDIO);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplerate, 
									m_nSampleRate);
								AMFCast<AMF0Object>(pAudioMeta)->PutBool(SZ_stereo, 
									sAInfo.eType != 0);
								AMFCast<AMF0Object>(pAudioMeta)->PutDouble(SZ_audiosamplesize, 16);
								m_pHandler->OnAudioStart(pAudioMeta);
							}
						}
					}
					break;
				default:
					if (m_pHandler && !(m_nAVFlags & AVINFO_ERROR_AUDIOUNSUPPORT))
					{
						BCException sExcept(__FUNCTION__, "ENOAUDIOCODEC");
						m_pHandler->OnException(sExcept);
						m_nAVFlags |= AVINFO_ERROR_AUDIOUNSUPPORT;
					}
					return 0;
					break;
				}

				m_nTotalAudPkt++;
				// All audio packet is key frame
				refHeader.SetAKFrame(true);
				refHeader.m_nAbsTime = refHeader.m_nTotalTime + m_tmAbsRecordTime;
				// Log timestamp check
				if (m_eAVFilterFlags & FILTER_FLAG_LOGTIMESTAMP)
				{
					if (refHeader.IsAbsTime())
					{
						m_nTotalAudTime = refHeader.m_nTimestamp;
					} 
					else
					{
						m_nTotalAudTime += refHeader.m_nTimestamp;
					}
					LogInfo(_LOCAL_, "[%s]Type : %2d(%s:%s); playback timestamp : %6d; "
						"datasize : %6d; accumulated time : %9" _S64BITARG_
						"; total time : %9" _S64BITARG_".\n", m_strStreamName.c_str(),
						refHeader.m_eDataType, pkt_type_to_string(refHeader.m_eDataType), 
						refHeader.IsAKFrame() ? "I" : "P", refHeader.m_nTimestamp, 
						refHeader.m_nDataSize, m_nTotalAudTime, refHeader.m_nTotalTime);
				}
				if (m_pHandler)
				{
					m_pHandler->OnAudioPacket(dtor.Release());
				}
			}
		}
		break;
	case FLVUtils::PTYPE_VIDEODATA:
		{
			if (m_eAVFilterFlags & FILTER_FLAG_DISABLEVIDEO)
			{
				return m_nNextSeqNumber;
			}
			bool bNormalVideo = true;
			if (!(m_nAVFlags & AVINFO_HAS_VIDEO))
			{
				m_nAVFlags |= AVINFO_HAS_VIDEO;
			}

			if (refHeader.m_nDataSize >= 2)
			{
				uint8_t *pData;
				FLVVideoInfoS videoInfo;

				pData = (uint8_t *)pPacket->m_sBody.Current();
				videoInfo = FLVInfo::AnalyseVideo(*pData);
				switch (videoInfo.eCodecId)
				{
				case FLV_RTP_RTCP_PACKET:
					break;
				case FLV_H264VIDEOPACKET:
					if (pData[1] == AVCPTYPE_SEQHEADER)
					{
						if (m_sAVCSeqHeader.m_pHeader->m_nDataSize == refHeader.m_nDataSize)
						{
							// Compare avc sequence data
							if (!memcmp(m_sAVCSeqHeader.m_sBody.Base(), 
								pPacket->m_sBody.Base(), refHeader.m_nDataSize))
							{
								break;
							}
						}
						if (!m_pMetaObj)
						{
							m_pMetaObj = AMFVarPtr(new AMF0Object());
						}
						AMFVarPtr pVideoMeta(new AMF0Object());
						pPacket->Clone(m_sAVCSeqHeader);
						m_sAVCSeqHeader.m_pHeader->SetVSeqHdr(true);
						m_sAVCSeqHeader.m_pHeader->SetVKFrame(true);
						m_sAVCSeqHeader.m_pHeader->m_nChannelId = 0;
						// Set latest video key frame timestamp
						m_tmLatestKeyFrame = m_nTotalTime;
						// Notify AVC sequence header
						if (m_pHandler)
						{
							KBPool sPool;
							LPVOID lpBuf = sPool.Calloc(refHeader.m_nDataSize);
							BCBIStream sReader(&m_sAVCSeqHeader.m_sBody);
							uint32_t nWidth = 0, nHeight = 0;

							sReader.Read(lpBuf, refHeader.m_nDataSize);
							sReader.Rewind();
							// set right width & height to metadata
							if (AvcC::DecodeAvcC((LPBYTE)lpBuf + 5, refHeader.m_nDataSize - 5, nWidth, nHeight))
							{
								if (m_pMetaObj)
								{
									AMFTable *pTable = NULL;
									if (m_pMetaObj->GetType() == AMF0_OBJECT)
									{
										pTable = AMFCast<AMF0Object>(m_pMetaObj);
									}
									else
									{
										pTable = AMFCast<AMF0ECMAArray>(m_pMetaObj);
									}
									if (m_nWidth != nWidth)
									{
										m_nWidth = nWidth;
									}
									if (m_nHeight != nHeight)
									{
										m_nHeight = nHeight;
									}
									pTable->PutString(SZ_videocodecid, "avc1");// ?
									pTable->PutDouble(SZ_width, nWidth);
									pTable->PutDouble(SZ_height, nHeight);
								}
								AMFCast<AMF0Object>(pVideoMeta)->PutString(SZ_videocodecid, "avc1");
								AMFCast<AMF0Object>(pVideoMeta)->PutDouble(SZ_width, nWidth);
								AMFCast<AMF0Object>(pVideoMeta)->PutDouble(SZ_height, nHeight);
								m_nAVFlags |= AVINFO_HAS_AVCHDR;
							}
							m_pHandler->OnAVCSeqHdr(lpBuf, refHeader.m_nDataSize);

							m_pHandler->OnVideoStart(pVideoMeta);
						}

						return 0;
					} 
					else
					{
						if (!(m_nAVFlags & AVINFO_HAS_AVCHDR))
						{
							if (m_pHandler && !(m_nAVFlags & AVINFO_REPORT_NOAVCHDR))
							{
								BCException sExcept(__FUNCTION__, "ENOAVCHDR");
								m_pHandler->OnException(sExcept);
								m_nAVFlags |= AVINFO_REPORT_NOAVCHDR;
							}
							return 0;
						}
					}
					break;
				default:
					if (m_pHandler && !(m_nAVFlags & AVINFO_ERROR_VIDEOUNSUPPORT))
					{
						BCException sExcept(__FUNCTION__, "ENOVIDEOCODEC");
						m_pHandler->OnException(sExcept);
						m_nAVFlags |= AVINFO_ERROR_VIDEOUNSUPPORT;
					}
					return 0;
					break;
				}

				if (FLVUtils::FLV_KEYFRAME == videoInfo.eFrameType)
				{
					bKeyFrame = true;
				}
				refHeader.SetVKFrame(bKeyFrame);
				refHeader.m_nAbsTime = refHeader.m_nTotalTime + m_tmAbsRecordTime;

				if (bKeyFrame)
				{
					// Set latest video key frame timestamp
					m_tmLatestKeyFrame = refHeader.m_nTotalTime;
				}
				// Debug
				if (m_eAVFilterFlags & FILTER_FLAG_LOGTIMESTAMP)
				{
					if (refHeader.IsAbsTime())
					{
						m_nTotalVidTime = refHeader.m_nTimestamp;
					} 
					else
					{
						m_nTotalVidTime += refHeader.m_nTimestamp;
					}
					LogInfo(_LOCAL_, "[%s]Type : %2d(%s:%s); playback timestamp : %6d; "
						"datasize : %6d; accumulated time : %9" _S64BITARG_
						"; total time : %9" _S64BITARG_".\n", m_strStreamName.c_str(),
						refHeader.m_eDataType, pkt_type_to_string(refHeader.m_eDataType), 
						refHeader.IsVKFrame() ? "I" : "P", refHeader.m_nTimestamp, 
						refHeader.m_nDataSize, m_nTotalVidTime, refHeader.m_nTotalTime);
				}
				if (m_pHandler)
				{
					m_pHandler->OnVideoPacket(dtor.Release());
				}
			}
		}
		break;
	case FLVUtils::PTYPE_METADATA:
		{
			if (!TrySetDataFrame(*pPacket, m_sMetaData))
			{
				if (IsMetaData(*pPacket))
				{
					pPacket->Clone(m_sMetaData);
				} 
				else
				{
					AMFCodecCtx sCtx;
					BCBIStream sReader(&pPacket->m_sBody);
					sReader.SetUserData(&sCtx);
					AMFVarPtr pKey = AMFCodec::Decode(&sReader);
					AMFVarPtr pMeta = AMFCodec::Decode(&sReader);
					sReader.Rewind();
					m_pHandler->OnCommonMetaData(dtor.Release(), pKey, pMeta);
					break;
				}
			}
			_Reset();
			// Get absolute record time
			AMFCodecCtx sCtx;
			BCBIStream sReader(&m_sMetaData.m_sBody);
			sReader.SetUserData(&sCtx);
			AMFVarPtr pMeta = AMFCodec::Decode(&sReader);
			if (pMeta && pMeta->GetType() == AMF_STRING && 
				AMFCast<AMFString>(pMeta)->GetValue() == SZ_onMetaData)
			{
				AMFVarPtr pObj = AMFCodec::Decode(&sReader);
				if (pObj && (pObj->GetType() == AMF0_OBJECT
					|| pObj->GetType() == AMF0_ECMAARRAY))
				{
					AMFTable *pTable = NULL;
					if (pObj->GetType() == AMF0_OBJECT)
					{
						pTable = AMFCast<AMF0Object>(pObj);
					} 
					else
					{
						pTable = AMFCast<AMF0ECMAArray>(pObj);
					}
					// Apply audio/video packet filter
					if (m_eAVFilterFlags & FILTER_FLAG_DISABLEAUDIO)
					{
						pTable->Remove(SZ_audiocodecid);
						pTable->Remove(SZ_audiosamplerate);
					}
					else if (m_eAVFilterFlags & FILTER_FLAG_DISABLEVIDEO)
					{
						pTable->Remove(SZ_videocodecid);
						pTable->Remove(SZ_framerate);
						pTable->Remove(SZ_width);
						pTable->Remove(SZ_height);
					}
					AMFVarPtr pTime = pTable->Get(SZ_absRecordTime);
					if (pTime)
					{
						// Reset time recorder
						_Reset();
						if (pTime->GetType() == AMF_NUMBER)
						{
							m_tmAbsRecordTime = (uint64_t)AMFCast<AMFNumber>(pTime)->GetDoubleValue();
						} 
						else if (pTime->GetType() == AMF_STRING)
						{
							m_tmAbsRecordTime = atoll(AMFCast<AMFString>(pTime)->GetValue());
						}
					}
					else
					{
						m_tmAbsRecordTime = bc_time_now()/1000;
					}
					// Set audio&video mask
					AMFVarPtr pCodec = pTable->Get(SZ_audiocodecid);
					if (pCodec && (pCodec->GetType() == AMF_STRING || pCodec->GetType() == AMF_NUMBER))
					{
						m_nAVFlags |= AVINFO_HAS_AUDIO;
					}
					pCodec = pTable->Get(SZ_videocodecid);
					if (pCodec && (pCodec->GetType() == AMF_STRING || pCodec->GetType() == AMF_NUMBER))
					{
						m_nAVFlags |= AVINFO_HAS_VIDEO;
					}
					AMFVarPtr pWidth = pTable->Get(SZ_width);
					if (pCodec && (pCodec->GetType() == AMF_NUMBER))
					{
						m_nWidth = AMFCast<AMFNumber>(pWidth)->GetDoubleValue();
					}
					AMFVarPtr pHeight = pTable->Get(SZ_height);
					if (pCodec && (pCodec->GetType() == AMF_NUMBER))
					{
						m_nHeight = AMFCast<AMFNumber>(pHeight)->GetDoubleValue();
					}
					m_pMetaObj.reset(pObj->Clone());
					// Notify onMetaData
					m_pHandler->OnMetaData(pObj);
				}
			}
			sReader.Rewind();
		}
		break;
	default:
		ASSERT(0);
		LogError(_LOCAL_, "Try to add NONE VIDEO/AUDIO/META packet into packet queue!");
		break;
	}

	return m_nNextSeqNumber++;
}

PPacket *PacketAnalyzer::GetVidSeqHdr(bool bClone)
{
	if (m_sAVCSeqHeader.m_pHeader->m_nDataSize > 0)
	{
		if (bClone)
		{
			return new PPacket(m_sAVCSeqHeader);
		} 
		else
		{
			return &m_sAVCSeqHeader;
		}
	}
	return NULL;
}

PPacket *PacketAnalyzer::GetAudSeqHdr(bool bClone)
{
	if (m_sAACSeqHeader.m_pHeader->m_nDataSize > 0)
	{
		if (bClone)
		{
			return new PPacket(m_sAACSeqHeader);
		} 
		else
		{
			return &m_sAACSeqHeader;
		}
	}
	return NULL;
}

uint64_t PacketAnalyzer::GetDuration() const
{
	return m_nTotalTime;
}


void PacketAnalyzer::SetAVFilter(uint8_t eFilterFlags)
{
	m_eAVFilterFlags = eFilterFlags;
	if ((m_eAVFilterFlags & FILTER_FLAG_DISABLEAUDIO) ||
		(m_eAVFilterFlags & FILTER_FLAG_DISABLEVIDEO))
	{
		if (m_pMetaObj && (m_pMetaObj->GetType() == AMF0_OBJECT
			|| m_pMetaObj->GetType() == AMF0_ECMAARRAY))
		{
			AMFTable *pTable = NULL;
			if (m_pMetaObj->GetType() == AMF0_OBJECT)
			{
				pTable = AMFCast<AMF0Object>(m_pMetaObj);
			}
			else
			{
				pTable = AMFCast<AMF0ECMAArray>(m_pMetaObj);
			}
			if (m_eAVFilterFlags & FILTER_FLAG_DISABLEAUDIO)
			{
				pTable->Remove(SZ_audiocodecid);
				pTable->Remove(SZ_audiosamplerate);
			}
			else if (m_eAVFilterFlags & FILTER_FLAG_DISABLEVIDEO)
			{
				pTable->Remove(SZ_videocodecid);
				pTable->Remove(SZ_framerate);
				pTable->Remove(SZ_width);
				pTable->Remove(SZ_height);
			}
		}
	}
}

bool PacketAnalyzer::TrySetDataFrame(
	const PPacket &refPacket,
	PPacket &refDstPkt)
{
	BCBIStream sReader(&refDstPkt.m_sBody);
	static const char bufSetDataFrame[] = 
	{
		0x02, 0x00, 0x0D, '@', 's', 'e', 't', 'D', 
		'a', 't', 'a', 'F', 'r', 'a', 'm', 'e'
	};
	char sBuffer[sizeof(bufSetDataFrame)];

	refPacket.Clone(refDstPkt);
	if (sReader.RemainingLength() < sizeof(bufSetDataFrame))
	{
		return false;
	}
	sReader.Read(sBuffer, sizeof(bufSetDataFrame));
	if (memcmp(bufSetDataFrame, sBuffer, sizeof(bufSetDataFrame)) == 0)
	{
		refDstPkt.m_sBody.RemoveConsumed();
		refDstPkt.m_pHeader->m_nDataSize = sReader.RemainingLength();

		return true;
	}
	return false;
}

bool PacketAnalyzer::IsMetaData(const PPacket &refPacket)
{
	BCBIStream sReader((BCBuffer *)&refPacket.m_sBody);
	static const char bufOnMetadata[] =
	{
		0x02, 0x00, 0x0A, 'o', 'n', 'M', 'e', 't',
		'a', 'D', 'a', 't', 'a'
	};
	char sBuffer[sizeof(bufOnMetadata)];

	if (sReader.RemainingLength() < sizeof(bufOnMetadata))
	{
		return false;
	}
	sReader.Read(sBuffer, sizeof(bufOnMetadata));
	sReader.Rewind();
	if (memcmp(bufOnMetadata, sBuffer, sizeof(bufOnMetadata)) == 0)
	{
		return true;
	}
	return false;
}

void PacketAnalyzer::flv2annexb(LPVOID lpData, uint32_t nLen)
{
	BCFBIStream sReader(lpData, nLen);
	BCFBOStream sWriter(lpData, nLen);
	uint32_t nDateLen = 0, nReadLen = 0;
	nReadLen = sReader.ReadUInt32BE(&nDateLen);
	while (nReadLen > 0)
	{
		sWriter.WriteUInt32BE(1);
		sWriter.Skip(nDateLen);
		sReader.Skip(nDateLen);
		if (!sReader.RemainingLength())
		{
			break;
		}
		nReadLen = sReader.ReadUInt32BE(&nDateLen);
	}
}

void PacketAnalyzer::_Reset()
{
	return; // Some pusher send metadata after audio data cause flags reset
	m_pMetaObj = AMFVarPtr();
	m_sAACSeqHeader.Reset();
	m_sAVCSeqHeader.Reset();
	m_nTotalTime = 0;
	m_nTotalAudTime = 0;
	m_nTotalVidTime = 0;
	m_nFirstSeqNumber = 0;
	m_nNextSeqNumber = 0;
	m_bNormalVidPkt = false;
	m_bNormalAudPkt = false;
	m_tmAbsRecordTime = 0;
	m_tmLatestKeyFrame = 0;
	m_nAVFlags = 0;
}

BCRESULT PacketAnalyzer::CreateMetaData(PPacket &refPacket)
{
	if (m_pMetaObj && (m_pMetaObj->GetType() == AMF0_OBJECT
		|| m_pMetaObj->GetType() == AMF0_ECMAARRAY))
	{
		AMFTable *pTable = NULL;
		AMFVarPtr pMeta(m_pMetaObj->Clone());
		if (pMeta->GetType() == AMF0_OBJECT)
		{
			pTable = AMFCast<AMF0Object>(pMeta);
		} 
		else
		{
			pTable = AMFCast<AMF0ECMAArray>(pMeta);
		}
		pTable->PutDouble(SZ_absRecordTime, m_tmAbsRecordTime + m_tmLatestKeyFrame);
		BCBOStream sWriter(&refPacket.m_sBody);
		AMFCodecCtx sCtx;
		BCPString onMetaData(SZ_onMetaData);
		sWriter.SetUserData(&sCtx);
		AMFCodec::EncodeString(&sWriter, onMetaData, true);
		AMFCodec::Encode(&sWriter, pMeta);
		refPacket.m_pHeader->m_eDataType = MTYPE_METADATA;
		refPacket.m_pHeader->m_nDataSize = refPacket.m_sBody.UsedLength();
		refPacket.m_pHeader->m_nTimestamp = 0;

		return BC_R_SUCCESS;
	}
	return BC_R_FAILURE;
}

///////////////////////////////////////////////////////////////////////////////
// class: PPacketQueue
///////////////////////////////////////////////////////////////////////////////

Base::AtomicRefCount				G_totalInput = 0;
Base::AtomicRefCount				G_totalOutput = 0;
Base::AtomicRefCount				G_totalFreed = 0;

#ifndef DEBUG_PACKET_QUEUE
#define AtomicRefCountInc(x)
#endif // DEBUG_PACKET_QUEUE

BCFObject *GetPacketStats()
{
	BCFObject* pStats = new BCFObject();
	if (pStats)
	{
		pStats->PutInt("PPacket_TotalInput", G_totalInput);
		pStats->PutInt("PPacket_TotalOutput", G_totalOutput);
		pStats->PutInt("PPacket_TotalFreed", G_totalFreed);
	}
	return pStats;
}


PPacketQueue::PPacketQueue()
	: m_nTotalTime(0)
	, m_nAVFlags(0)
	//, m_pDumpFD(NULL)
	, m_nAudPktInput(0)
	, m_nVidTotalInput(0)
	, m_nVidTotalDropped(0)
	, m_nAudPktDropped(0)
	, m_nLastSentOrDroppedTimestamp(0)
	, m_nAudPktOutput(0)
	, m_nVidTotalOutput(0)
	, m_nAudPktCache(0)
	, m_nVidTotalCache(0)
	, m_bRecvVKFrame(false)
{
	//
}

PPacketQueue::~PPacketQueue()
{
	Cleanup();
	//if (m_pDumpFD)
	//{
	//	::fclose(m_pDumpFD);
	//	m_pDumpFD = NULL;
	//}	
}

BCRESULT PPacketQueue::Create()
{
	return BC_R_SUCCESS;
}

void PPacketQueue::Cleanup()
{
	PPacket *iter;
	while((iter = m_lstPackets.PopFront()) != NULL)
	{
		_ReduceTotalTime(iter);
		BC_SAFE_DELETE_PTR(iter);
		AtomicRefCountInc(&G_totalFreed);
	}
	while ((iter = m_lstMustSendPackets.PopFront()) != NULL)
	{
		_ReduceTotalTime(iter);
		BC_SAFE_DELETE_PTR(iter);
		AtomicRefCountInc(&G_totalFreed);
	}
}

uint32_t PPacketQueue::PushBack(PPacket *pPacket)
{
	const PHeader &refHeader = *pPacket->m_pHeader;

	switch(refHeader.m_eDataType)
	{
	case FLVUtils::PTYPE_AUDIODATA:
		if (!m_nAudPktInput)
		{
			m_nAVFlags |= AVINFO_HAS_AUDIO;
		}
		m_nAudPktInput++;
		m_lstPackets.PushBack(pPacket);
		AtomicRefCountInc(&G_totalInput);
		break;
	case FLVUtils::PTYPE_VIDEODATA:
		if (m_bRecvVKFrame)
		{
			m_nVidTotalInput += refHeader.m_nTimestamp;
			m_lstPackets.PushBack(pPacket);
			AtomicRefCountInc(&G_totalInput);
		}
		else if (refHeader.IsVKFrame())
		{
			m_nVidTotalInput += refHeader.m_nTimestamp;
			m_lstPackets.PushBack(pPacket);
			AtomicRefCountInc(&G_totalInput);
			m_bRecvVKFrame = true;
			m_nAVFlags |= AVINFO_HAS_VIDEO;
		}
		break;
	case FLVUtils::PTYPE_METADATA:
		m_lstPackets.PushBack(pPacket);
		AtomicRefCountInc(&G_totalInput);
		break;
	default:
		ASSERT(0);
		LogError(_LOCAL_, "Try to add NONE-AUDIO/VIDEO/METADATA packet into packet queue");
		break;
	}

	return GetDuration();
}

PPacket *PPacketQueue::_PopAndDropFront()
{
	PPacket *pPacket = m_lstPackets.PopFront();
	
	_ReduceTotalTime(pPacket);
	
	return pPacket;
}

PPacket	* PPacketQueue::PopAndSendFront()
{
	PPacket *pPacket = NULL;
	
	if (m_lstMustSendPackets.Count() > 0)
	{
		pPacket = m_lstMustSendPackets.PopFront();
		PHeader &refHeader = *pPacket->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			ASSERT(m_nAudPktCache > 0);
			m_nAudPktCache--;
			break;
		case MTYPE_VIDEODATA:
			m_nVidTotalCache -= refHeader.m_nTimestamp;
			break;
		default:
			break;
		}
	} 
	else
	{
		pPacket = m_lstPackets.PopFront();
	}

	if (pPacket)
	{
		PHeader &refHeader = *pPacket->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			m_nAudPktOutput++;
			break;
		case MTYPE_VIDEODATA:
			m_nVidTotalOutput += refHeader.m_nTimestamp;
			break;
		default:
			break;
		}
		_ReduceTotalTime(pPacket);
		//if (m_nSampleRate > 0)
		//{
		//	uint64_t audioPTS = m_nAudPktOutput*m_nSamplesPerFrame * 1000 / m_nSampleRate;
		//	LogDebug(_LOCAL_, "audio pts : %6"_U64BITARG_"; video pts : %6"_U64BITARG_,
		//		audioPTS, m_nVidTotalOutput);
		//}
		AtomicRefCountInc(&G_totalOutput);
	}

	return pPacket;
}

void PPacketQueue::_ReduceTotalTime(PPacket *pPacket)
{
	if (pPacket)
	{
		PHeader &refHeader = *pPacket->m_pHeader;
		//if (refHeader.IsAbsTime() || m_nTotalTime < refHeader.m_nTimestamp)
		//{
		//	m_nTotalTime = 0;
		//}
		//else
		//{
		//	m_nTotalTime -= refHeader.m_nTimestamp;
		//}
		m_nLastSentOrDroppedTimestamp = refHeader.m_nTotalTime;
	}
#if 0 // too frequently
	if (m_lstPackets.IsEmpty() && m_pHandler)
	{
		m_pHandler->OnStreamBufferEvent(BUFFER_EVENT_EMPTY);
	}
#endif // 0
}

uint32_t PPacketQueue::_RemoveOnce(AVDropStatS &refInfo)
{
	uint32_t nCount;
	bool bFirst;
	PPacket *iter, *iterEnd, *pItem;

	nCount = 0;
	bFirst = true;
	iter = m_lstPackets.Begin();
	iterEnd = m_lstPackets.End();
	for (;iter != iterEnd;)
	{
		if (bFirst)
		{
			bFirst = false;
			pItem = iter;
			iter = m_lstPackets.Next(iter);
			pItem->RemoveFromList();
			_ReduceTotalTime(pItem);
			switch(pItem->m_pHeader->m_eDataType)
			{
			case MTYPE_AUDIODATA:
				refInfo.nAudioPacketCount++;
				break;
			case MTYPE_VIDEODATA:
				refInfo.nVideoPacketCount++;
				break;
			}
			refInfo.nLastDropTotalTime = pItem->m_pHeader->m_nTotalTime;
			BC_SAFE_DELETE_PTR(pItem);
			AtomicRefCountInc(&G_totalFreed);
			nCount++;
		}
		else if (iter->m_pHeader->IsVKFrame())
		{
			break;
		}
		else
		{
			pItem = iter;
			iter = m_lstPackets.Next(iter);
			pItem->RemoveFromList();
			_ReduceTotalTime(pItem);
			switch(pItem->m_pHeader->m_eDataType)
			{
			case MTYPE_AUDIODATA:
				refInfo.nAudioPacketCount++;
				break;
			case MTYPE_VIDEODATA:
				refInfo.nVideoPacketCount++;
				break;
			}
			refInfo.nLastDropTotalTime = pItem->m_pHeader->m_nTotalTime;
			BC_SAFE_DELETE_PTR(pItem);
			AtomicRefCountInc(&G_totalFreed);
			nCount++;
		}
	}
	return (nCount);
}

uint32_t PPacketQueue::_RemoveOnceWithVideo(AVDropStatS &refInfo)
{
	uint32_t nCount, nAudPktSend = 0;
	int64_t nVidPTSSend = 0;
	PPacket *pIter, *pIterEnd, *pIterFirst = NULL, *pIterLast = NULL;
	if (!m_lstMustSendPackets.Count() && (m_nAVFlags & AVINFO_HAS_AUDIO))
	{
		std::list<PPacket *>	vecMustSend;

		nCount = 0;
		pIter = m_lstPackets.Begin();
		pIterEnd = m_lstPackets.End();
		for (;pIter != pIterEnd; pIter = m_lstPackets.Next(pIter))
		{
			if (pIter->m_pHeader->IsVKFrame())
			{
				if (!pIterFirst)
				{
					pIterFirst = pIter;
				}
				else
				{
					pIterLast = pIter;
				}
			}
			else
			{
				if (pIter->m_pHeader->m_eDataType == MTYPE_VIDEODATA)
				{
					nVidPTSSend += pIter->m_pHeader->m_nTimestamp;
				}
				vecMustSend.push_back(pIter);
				if (pIter->m_pHeader->m_eDataType == MTYPE_AUDIODATA)
				{
					nAudPktSend++;
				}
			}
			if (pIterFirst && pIterLast)
			{
				break;
			}
		}
		if (!pIterLast)
		{
			return 0;
		}
		// ���뷢�Ͷ���
		if (vecMustSend.size() > 0)
		{
			bool bFindEnd = false;
			PPacket *pMustSendEnd, *pSend = NULL;

			pMustSendEnd = vecMustSend.back();
			while ((pIter = m_lstPackets.Begin()) != pIterFirst)
			{
				pSend = m_lstPackets.PopFront();
				if (bFindEnd)
				{
					_DropPacket(pSend, refInfo);
					nCount++;
				} 
				else
				{
					m_lstMustSendPackets.PushBack(pSend);
					if (pIter->m_pHeader->m_eDataType == MTYPE_AUDIODATA)
					{
						m_nAudPktCache++;
					}
					else
					{
						m_nVidTotalCache += pIter->m_pHeader->m_nTimestamp;
					}
				}
				if (pIter == pMustSendEnd)
				{
					bFindEnd = true;
				}
			}
		}
	}
	else
	{
		nCount = 0;
		pIter = m_lstPackets.Begin();
		pIterEnd = m_lstPackets.End();
		for (;pIter != pIterEnd; pIter = m_lstPackets.Next(pIter))
		{
			if (pIter->m_pHeader->IsVKFrame())
			{
				if (!pIterFirst)
				{
					pIterFirst = pIter;
				}
				else
				{
					pIterLast = pIter;
				}
			}
			if (pIterFirst && pIterLast)
			{
				break;
			}
		}
	}
	if (!pIterLast)
	{
		return 0;
	}
	
	// ��һ���ؼ�֡����
	while ((pIter = m_lstPackets.Begin()) != pIterLast)
	{
		_DropPacket(m_lstPackets.PopFront(), refInfo);
		nCount++;
	}

	return (nCount);
}

int PPacketQueue::_QSortFunc(LPCVOID lpParam1, LPCVOID lpParam2)
{
	SyncPairS *pPacket1 = (SyncPairS *)lpParam1;
	SyncPairS *pPacket2 = (SyncPairS *)lpParam2;

	if (pPacket1->ptsDiff < pPacket2->ptsDiff)
	{
		return -1;
	}
	else if (pPacket1->ptsDiff == pPacket2->ptsDiff)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

void PPacketQueue::_DropPacket(PPacket *pPacket, AVDropStatS &refInfo)
{
	const PHeader &refHeader = *pPacket->m_pHeader;

	switch (refHeader.m_eDataType)
	{
	case FLVUtils::PTYPE_AUDIODATA:
		m_nAudPktDropped++;
		refInfo.nAudioPacketCount++;
		break;
	case FLVUtils::PTYPE_VIDEODATA:
		m_nVidTotalDropped += refHeader.m_nTimestamp;
		refInfo.nVideoPacketCount++;
		break;
	default:
		break;
	}
	BC_SAFE_DELETE_PTR(pPacket);
	AtomicRefCountInc(&G_totalFreed);
}

void PPacketQueue::FlushTimeout(uint32_t nEpoch, AVDropStatS &refInfo)
{
	uint32_t nCount = 0;

	refInfo.nAudioPacketCount = 0;
	refInfo.nVideoPacketCount = 0;
	if (!m_nAVFlags)
	{
		return;
	}
	if (AVINFO_HAS_VIDEO & m_nAVFlags) // Has video data
	{
		uint32_t nDropped = 0;
		while(GetDuration() > nEpoch && m_lstPackets.Count() > 1)
		{
			nDropped = _RemoveOnceWithVideo(refInfo);
			if (!nDropped)
			{
				break;
			}
			nCount += nDropped;
		}
	}
	else // only audio
	{
		PPacket *pItem;

		while ((GetDuration() > nEpoch) && (pItem = _PopAndDropFront()))
		{
			switch(pItem->m_pHeader->m_eDataType)
			{
			case MTYPE_AUDIODATA:
				refInfo.nAudioPacketCount++;
				break;
			case MTYPE_VIDEODATA:
				refInfo.nVideoPacketCount++;
				break;
			}
			refInfo.nLastDropTotalTime = pItem->m_pHeader->m_nTotalTime;
			_DropPacket(pItem, refInfo);
			nCount++;
		}
	}
	if (nCount > 0 && m_lstPackets.Count() > 0)
	{
		refInfo.nFirstTotalTime = m_lstPackets.Begin()->m_pHeader->m_nTotalTime;
	}
#ifdef _DEBUG
	//LogDebug(_LOCAL_, "Audio packets number : %d", m_lstPackets.Count());
#endif
}

void PPacketQueue::Align(AVDropStatS &refInfo)
{
	if (!m_nAVFlags)
	{
		return;
	}
	if (AVINFO_HAS_VIDEO & m_nAVFlags) // Has video data
	{
		uint32_t nCount = 0;
		PPacket *pItem = m_lstPackets.Begin();

		if (pItem != m_lstPackets.End() &&
			!pItem->m_pHeader->IsVKFrame())
		{
			nCount += _RemoveOnceWithVideo(refInfo);
		}
		if (m_lstPackets.Count() > 0)
		{
			refInfo.nFirstTotalTime = m_lstPackets.Begin()->m_pHeader->m_nTotalTime;
		}
	}
}

uint64_t PPacketQueue::GetDuration()
{
	PPacket *pFirst = m_lstPackets.Front();
	PPacket *pBack = m_lstPackets.Back();
	if (pFirst && pBack)
	{
		uint64_t nFirstTotalTime = pFirst->m_pHeader->m_nTotalTime;
		uint64_t nEndTotalTime = pBack->m_pHeader->m_nTotalTime;
		return nEndTotalTime - nFirstTotalTime;
	}
	return 0;
}

void PPacketQueue::GetDuration(
	uint64_t &refVideoDuration,
	uint64_t &refAudioDuration) const
{
	int64_t audioStart = -1, audioEnd = -1, videoStart = -1, videoEnd = -1;
	PPacket *pIter;

#if 0
	for (pIter = m_lstPackets.Begin();pIter != m_lstPackets.End();pIter = m_lstPackets.Next(pIter))
	{
		const PHeader &refHeader = *pIter->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			if (audioStart < 0)
			{
				audioStart = refHeader.m_nTotalTime;
			}
			audioEnd = refHeader.m_nTotalTime;
			break;
		case MTYPE_VIDEODATA:
			if (videoStart < 0)
			{
				videoStart = refHeader.m_nTotalTime;
			}
			videoEnd = refHeader.m_nTotalTime;
			break;
		default:
			break;
		}
	}
#else // !0
	for (pIter = m_lstPackets.Begin();
		pIter != m_lstPackets.End();
		pIter = m_lstPackets.Next(pIter))
	{
		const PHeader &refHeader = *pIter->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			if (audioStart < 0)
			{
				audioStart = refHeader.m_nTotalTime;
			}
			break;
		case MTYPE_VIDEODATA:
			if (videoStart < 0)
			{
				videoStart = refHeader.m_nTotalTime;
			}
			break;
		default:
			break;
		}
		if ((audioStart >= 0 && videoStart >= 0) ||
			(audioStart >= 0 && (!(m_nAVFlags & AVINFO_HAS_VIDEO))) ||
			(videoStart >= 0 && (!(m_nAVFlags & AVINFO_HAS_AUDIO))))
		{
			break;
		}
	}
	for (pIter = m_lstPackets.Back(); pIter; pIter = m_lstPackets.Prev(pIter))
	{
		const PHeader &refHeader = *pIter->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			if (audioEnd < 0)
			{
				audioEnd = refHeader.m_nTotalTime;
			}
			break;
		case MTYPE_VIDEODATA:
			if (videoEnd < 0)
			{
				videoEnd = refHeader.m_nTotalTime;
			}
			break;
		default:
			break;
		}
		if ((audioEnd >= 0 && videoEnd >= 0) ||
			((audioEnd >= 0) && (!(m_nAVFlags & AVINFO_HAS_VIDEO))) ||
			((videoEnd >= 0) && (!(m_nAVFlags & AVINFO_HAS_AUDIO))) ||
			pIter == m_lstPackets.Begin())
		{
			break;
		}
	}
#endif // 0
	refVideoDuration = videoEnd - videoStart;
	refAudioDuration = audioEnd - audioStart;
}

///////////////////////////////////////////////////////////////////////////////
// class: PPacketQueueSimple
///////////////////////////////////////////////////////////////////////////////

PPacketQueueSimple::PPacketQueueSimple()
	: m_nTotalTime(0)
	, m_nAVFlags(0)
	//, m_pDumpFD(NULL)
	, m_nAudPktInput(0)
	, m_nVidTotalInput(0)
	, m_nVidTotalDropped(0)
	, m_nAudPktDropped(0)
	, m_nSampleRate(0)
	, m_nSamplesPerFrame(0)
	, m_nLastSentOrDroppedTimestamp(0)
	, m_nAudPktOutput(0)
	, m_nVidTotalOutput(0)
	, m_nAudPktCache(0)
	, m_nVidTotalCache(0)
	, m_nFeedPackets(0)
	, m_nSentPackets(0)
	, m_nDroppedPackets(0)
{
	//
}

PPacketQueueSimple::~PPacketQueueSimple()
{
	Cleanup();
	//if (m_pDumpFD)
	//{
	//	::fclose(m_pDumpFD);
	//	m_pDumpFD = NULL;
	//}	
}

BCRESULT PPacketQueueSimple::Create()
{
	return BC_R_SUCCESS;
}

void PPacketQueueSimple::Cleanup()
{
	PPacket *iter;
	while((iter = m_lstPackets.PopFront()) != NULL)
	{
		_ReduceTotalTime(iter);
		BC_SAFE_DELETE_PTR(iter);
	}
}

uint32_t PPacketQueueSimple::PushBack(PPacket *pPacket)
{
	//uint64_t nDuration = GetDuration();
	//LogDebug(_LOCAL_, "feed : %" _U64BITARG_"; sent : %" _U64BITARG_
	//	"; dropped : %" _U64BITARG_"; duration : %" _U64BITARG_,
	//	m_nFeedPackets, m_nSentPackets, m_nDroppedPackets, nDuration);

	const PHeader &refHeader = *pPacket->m_pHeader;

	switch(refHeader.m_eDataType)
	{
	case FLVUtils::PTYPE_AUDIODATA:
		m_nAudPktInput++;
		m_lstPackets.PushBack(pPacket);
		m_nFeedPackets++;
		break;
	case FLVUtils::PTYPE_VIDEODATA:
		m_nVidTotalInput += refHeader.m_nTimestamp;
		m_lstPackets.PushBack(pPacket);
		m_nFeedPackets++;
		break;
	default:
		ASSERT(0);
		LogError(_LOCAL_, "Try to add NONE-AUDIO/VIDEO packet into packet queue");
		break;
	}

	return GetDuration();
}

PPacket *PPacketQueueSimple::_PopAndDropFront()
{
	PPacket *pPacket = m_lstPackets.PopFront();
	
	_ReduceTotalTime(pPacket);
	
	return pPacket;
}

PPacket	* PPacketQueueSimple::PopAndSendFront()
{
	PPacket *pPacket = NULL;
	
	if (m_lstMustSendPackets.Count() > 0)
	{
		pPacket = m_lstMustSendPackets.PopFront();
		PHeader &refHeader = *pPacket->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			ASSERT(m_nAudPktCache > 0);
			m_nAudPktCache--;
			break;
		case MTYPE_VIDEODATA:
			m_nVidTotalCache -= refHeader.m_nTimestamp;
			break;
		default:
			break;
		}
	} 
	else
	{
		pPacket = m_lstPackets.PopFront();
	}

	if (pPacket)
	{
		PHeader &refHeader = *pPacket->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			m_nAudPktOutput++;
			break;
		case MTYPE_VIDEODATA:
			m_nVidTotalOutput += refHeader.m_nTimestamp;
			break;
		default:
			break;
		}
		_ReduceTotalTime(pPacket);
		m_nSentPackets++;
		//if (m_nSampleRate > 0)
		//{
		//	uint64_t audioPTS = m_nAudPktOutput*m_nSamplesPerFrame * 1000 / m_nSampleRate;
		//	LogDebug(_LOCAL_, "audio pts : %6"_U64BITARG_"; video pts : %6"_U64BITARG_,
		//		audioPTS, m_nVidTotalOutput);
		//}
	}

	return pPacket;
}

void PPacketQueueSimple::_ReduceTotalTime(PPacket *pPacket)
{
	if (pPacket)
	{
		PHeader &refHeader = *pPacket->m_pHeader;
		//if (refHeader.IsAbsTime() || m_nTotalTime < refHeader.m_nTimestamp)
		//{
		//	m_nTotalTime = 0;
		//}
		//else
		//{
		//	m_nTotalTime -= refHeader.m_nTimestamp;
		//}
		m_nLastSentOrDroppedTimestamp = refHeader.m_nTotalTime;
	}
#if 0 // too frequently
	if (m_lstPackets.IsEmpty() && m_pHandler)
	{
		m_pHandler->OnStreamBufferEvent(BUFFER_EVENT_EMPTY);
	}
#endif // 0
}

uint32_t PPacketQueueSimple::_RemoveOnce(AVDropStatS &refInfo)
{
	uint32_t nCount;
	bool bFirst;
	PPacket *iter, *iterEnd, *pItem;

	nCount = 0;
	bFirst = true;
	iter = m_lstPackets.Begin();
	iterEnd = m_lstPackets.End();
	for (;iter != iterEnd;)
	{
		if (bFirst)
		{
			bFirst = false;
			pItem = iter;
			iter = m_lstPackets.Next(iter);
			pItem->RemoveFromList();
			_ReduceTotalTime(pItem);
			_DropPacket(pItem, refInfo);
			nCount++;
		}
		else if (iter->m_pHeader->IsVKFrame())
		{
			break;
		}
		else
		{
			pItem = iter;
			iter = m_lstPackets.Next(iter);
			pItem->RemoveFromList();
			_ReduceTotalTime(pItem);
			_DropPacket(pItem, refInfo);
			nCount++;
		}
	}
	return (nCount);
}

uint32_t PPacketQueueSimple::_RemoveOnceWithVideo(AVDropStatS &refInfo)
{
	uint32_t nCount, nAudPktSend = 0;
	int64_t nVidPTSSend = 0;
	PPacket *pIter, *pIterEnd, *pIterFirst = NULL, *pIterLast = NULL;
	if (!m_lstMustSendPackets.Count() && (m_nAVFlags & AVINFO_HAS_AUDIO))
	{
		std::list<PPacket *>	vecMustSend;

		nCount = 0;
		pIter = m_lstPackets.Begin();
		pIterEnd = m_lstPackets.End();
		for (;pIter != pIterEnd; pIter = m_lstPackets.Next(pIter))
		{
			if (pIter->m_pHeader->IsVKFrame())
			{
				if (!pIterFirst)
				{
					pIterFirst = pIter;
				}
				else
				{
					pIterLast = pIter;
				}
			}
			else
			{
				if (!pIterFirst)
				{
					if (pIter->m_pHeader->m_eDataType == MTYPE_VIDEODATA)
					{
						nVidPTSSend += pIter->m_pHeader->m_nTimestamp;
					}
					vecMustSend.push_back(pIter);
					if (pIter->m_pHeader->m_eDataType == MTYPE_AUDIODATA)
					{
						nAudPktSend++;
					}
				}
			}
			if (pIterFirst && pIterLast)
			{
				break;
			}
		}
		if (!pIterLast)
		{
			return 0;
		}
		// ���뷢�Ͷ���
		if (vecMustSend.size() > 0)
		{
			bool bFindEnd = false;
			PPacket *pMustSendEnd, *pSend = NULL;

			pMustSendEnd = vecMustSend.back();
			while ((pIter = m_lstPackets.Begin()) != pIterFirst)
			{
				pSend = m_lstPackets.PopFront();
				if (bFindEnd)
				{
					_DropPacket(pSend, refInfo);
					nCount++;
				} 
				else
				{
					m_lstMustSendPackets.PushBack(pSend);
					if (pIter->m_pHeader->m_eDataType == MTYPE_AUDIODATA)
					{
						m_nAudPktCache++;
					}
					else
					{
						m_nVidTotalCache += pIter->m_pHeader->m_nTimestamp;
					}
				}
				if (pIter == pMustSendEnd)
				{
					bFindEnd = true;
				}
			}
		}
	}
	else
	{
		nCount = 0;
		pIter = m_lstPackets.Begin();
		pIterEnd = m_lstPackets.End();
		for (;pIter != pIterEnd; pIter = m_lstPackets.Next(pIter))
		{
			if (pIter->m_pHeader->IsVKFrame())
			{
				if (!pIterFirst)
				{
					pIterFirst = pIter;
				}
				else
				{
					pIterLast = pIter;
				}
			}
			if (pIterFirst && pIterLast)
			{
				break;
			}
		}
	}
	if (!pIterLast)
	{
		return 0;
	}
	
	// ��һ���ؼ�֡����
	while ((pIter = m_lstPackets.Begin()) != pIterLast)
	{
		_DropPacket(m_lstPackets.PopFront(), refInfo);
		nCount++;
	}

	return (nCount);
}

int PPacketQueueSimple::_QSortFunc(LPCVOID lpParam1, LPCVOID lpParam2)
{
	SyncPairS *pPacket1 = (SyncPairS *)lpParam1;
	SyncPairS *pPacket2 = (SyncPairS *)lpParam2;

	if (pPacket1->ptsDiff < pPacket2->ptsDiff)
	{
		return -1;
	}
	else if (pPacket1->ptsDiff == pPacket2->ptsDiff)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

void PPacketQueueSimple::_DropPacket(PPacket *pPacket, AVDropStatS &refInfo)
{
	const PHeader &refHeader = *pPacket->m_pHeader;

	if (!refInfo.nFirstTotalTime)
	{
		refInfo.nFirstTotalTime = pPacket->m_pHeader->m_nTotalTime;
	}
	switch (refHeader.m_eDataType)
	{
	case FLVUtils::PTYPE_AUDIODATA:
		m_nAudPktDropped++;
		refInfo.nAudioPacketCount++;
		break;
	case FLVUtils::PTYPE_VIDEODATA:
		m_nVidTotalDropped += refHeader.m_nTimestamp;
		refInfo.nVideoPacketCount++;
		break;
	default:
		break;
	}
	if (m_lstPackets.Count() > 0)
	{
		refInfo.nLastDropTotalTime = m_lstPackets.Begin()->m_pHeader->m_nTotalTime;
	}
	m_nDroppedPackets++;
	BC_SAFE_DELETE_PTR(pPacket);
}

void PPacketQueueSimple::FlushTimeout(uint32_t nEpoch, AVDropStatS &refInfo)
{
	uint32_t nCount = 0;

	refInfo.nAudioPacketCount = 0;
	refInfo.nVideoPacketCount = 0;
	if (AVINFO_HAS_VIDEO & m_nAVFlags) // Has video data
	{
		uint32_t nDropped = 0;
		while(GetDuration() > nEpoch && m_lstPackets.Count() > 1)
		{
			nDropped = _RemoveOnceWithVideo(refInfo);
			if (!nDropped)
			{
				break;
			}
			nCount += nDropped;
		}
	}
	else // only audio
	{
		PPacket *pItem;

		while ((GetDuration() > nEpoch) && (pItem = _PopAndDropFront()))
		{
			_DropPacket(pItem, refInfo);
			nCount++;
		}
	}
#ifdef _DEBUG
	//LogDebug(_LOCAL_, "Audio packets number : %d", m_lstPackets.Count());
#endif
}

void PPacketQueueSimple::Align(AVDropStatS &refInfo)
{
	if (AVINFO_HAS_VIDEO & m_nAVFlags) // Has video data
	{
		uint32_t nCount = 0;
		PPacket *pItem = m_lstPackets.Begin();

		if (pItem != m_lstPackets.End() &&
			!pItem->m_pHeader->IsVKFrame())
		{
			nCount += _RemoveOnceWithVideo(refInfo);
		}
	}
}

uint64_t PPacketQueueSimple::GetDuration() const
{
	const PPacket *pFirst = m_lstPackets.Front();
	const PPacket *pBack = m_lstPackets.Back();
	if (pFirst && pBack && pFirst != pBack)
	{
		uint64_t nFirstTotalTime = pFirst->m_pHeader->m_nTotalTime;
		uint64_t nEndTotalTime = pBack->m_pHeader->m_nTotalTime;
		return nEndTotalTime - nFirstTotalTime;
	}
	return 0;
}

void PPacketQueueSimple::GetDuration(
	uint64_t &refVideoDuration,
	uint64_t &refAudioDuration) const
{
	int64_t audioStart = -1, audioEnd = -1, videoStart = -1, videoEnd = -1;
	const PPacket *pIter;

	for (pIter = m_lstPackets.Begin(); pIter != m_lstPackets.End(); pIter = m_lstPackets.Next(pIter))
	{
		const PHeader &refHeader = *pIter->m_pHeader;
		switch (refHeader.m_eDataType)
		{
		case MTYPE_AUDIODATA:
			if (audioStart < 0)
			{
				audioStart = refHeader.m_nTotalTime;
			}
			audioEnd = refHeader.m_nTotalTime;
			break;
		case MTYPE_VIDEODATA:
			if (videoStart < 0)
			{
				videoStart = refHeader.m_nTotalTime;
			}
			videoEnd = refHeader.m_nTotalTime;
			break;
		default:
			break;
		}
	}
	refVideoDuration = videoEnd - videoStart;
	refAudioDuration = audioEnd - audioStart;
}

///////////////////////////////////////////////////////////////////////////////
// class : PPacketSender
///////////////////////////////////////////////////////////////////////////////

#define DEFAULT_BUFFER_LENGTH		5000
#define MIN_BUFFER_LENGTH			1000
#define MAX_BUFFER_LENGTH			60000

PPacketSender::PPacketSender()
	: m_pHandler(NULL)
	, m_bPaused(false)
	, m_nSendCount(0)
{
}

PPacketSender::~PPacketSender()
{
}

BCRESULT PPacketSender::Create(
	IPPacketSenderHandler *pHandler,
	PPacketSender::Config &refConfig)
{
	m_sConfig = refConfig;
	m_pHandler = pHandler;
	m_lstAVPackets.Create();
	m_lstAVPackets.SetParams(m_sConfig.flags, m_sConfig.sampleRate, 
		m_sConfig.samplesPerFrame);
	return BC_R_SUCCESS;
}

BCRESULT PPacketSender::Reconfig(BCFObject *pConfig)
{
	m_sConfig.Init(pConfig);
	if (m_sConfig.bufferLength < MIN_BUFFER_LENGTH)
	{
		m_sConfig.bufferLength = MIN_BUFFER_LENGTH;
	}
	else if (m_sConfig.bufferLength > MAX_BUFFER_LENGTH)
	{
		m_sConfig.bufferLength = MAX_BUFFER_LENGTH;
	}
	return BC_R_SUCCESS;
}

uint32_t PPacketSender::GetBufferLength() const
{
	//return m_nBufferLength;
	return m_lstAVPackets.GetDuration();
}

void PPacketSender::Pause(bool bPause)
{
	m_bPaused = bPause;
	if (bPause)
	{
		AVDropStatS stats;
		m_lstAVPackets.Align(stats);
		if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
		{
			m_pHandler->OnBufferEvent(m_sConfig.streamId, BUFFER_EVENT_FULL, stats);
		}
	}
	else
	{
		PPacket *pPacket;

		if (!m_bPaused && !m_nSendCount && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
		{
			m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
			m_nSendCount++;
		}
	}
}

void PPacketSender::PauseWithoutCounter(bool bPause)
{
	m_bPaused = bPause;
	if (bPause)
	{
		AVDropStatS stats;
		m_lstAVPackets.Align(stats);
		if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
		{
			m_pHandler->OnBufferEvent(m_sConfig.streamId, BUFFER_EVENT_FULL, stats);
		}
	}
	else
	{
		PPacket *pPacket;

		if (!m_bPaused && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
		{
			m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
		}
	}
}

void PPacketSender::FeedPacket(PPacket *pPacket)
{
	uint64_t nBufferLength = 0;
	AVDropStatS stats;
	m_lstAVPackets.PushBack(pPacket);
	// Flush time out packets
	if (pPacket->m_pHeader->IsVKFrame() && m_lstAVPackets.Count() > 1)
	{
		m_lstAVPackets.FlushTimeout(m_sConfig.bufferLength, stats);
	}
	else if (!(AVINFO_HAS_VIDEO & m_lstAVPackets.GetFlags()))
	{
		m_lstAVPackets.FlushTimeout(m_sConfig.bufferLength, stats);
	}
	if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
	{
		m_pHandler->OnBufferEvent(m_sConfig.streamId, BUFFER_EVENT_FULL, stats);
	}

	while (!m_bPaused && m_nSendCount < m_sConfig.flyPackets && 
		(pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
	{
		m_nSendCount++;
		m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
	}
	nBufferLength = m_lstAVPackets.GetDuration();
	if (m_sConfig.bufferOverflow > 0 && nBufferLength > m_sConfig.bufferOverflow && m_pHandler)
	{
		uint64_t audioDuration, videoDuration;

		m_lstAVPackets.GetDuration(videoDuration, audioDuration);
		BCFObject *pInfo = new BCFObject();
		pInfo->PutInt("audioBufferLength", audioDuration);
		pInfo->PutInt("videoBufferLength", videoDuration);
		m_pHandler->OnBufferOverflow(m_sConfig.streamId, pInfo);
	}
}

void PPacketSender::FeedPacketWithoutCounter(PPacket *pPacket)
{
	AVDropStatS stats;
	m_lstAVPackets.PushBack(pPacket);
	// Flush time out packets
	if (pPacket->m_pHeader->IsVKFrame() && m_lstAVPackets.Count() > 1)
	{
		m_lstAVPackets.FlushTimeout(m_sConfig.bufferLength, stats);
	}
	else if (!(AVINFO_HAS_VIDEO & m_lstAVPackets.GetFlags()))
	{
		m_lstAVPackets.FlushTimeout(m_sConfig.bufferLength, stats);
	}
	if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
	{
		m_pHandler->OnBufferEvent(m_sConfig.streamId, BUFFER_EVENT_FULL, stats);
	}

	if (!m_bPaused && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
	{
		m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
	}
}

void PPacketSender::SendPacket()
{
	PPacket *pPacket;

	ASSERT(m_nSendCount);
	m_nSendCount--;
	while (!m_bPaused && m_nSendCount < m_sConfig.flyPackets && 
		(pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
	{
		m_nSendCount++;
		m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
	}
}

void PPacketSender::SendPacketWithoutCounter()
{
	PPacket *pPacket;

	if (!m_bPaused && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
	{
		m_pHandler->OnPacketReady(m_sConfig.streamId, pPacket);
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : PacketQueue.cpp
///////////////////////////////////////////////////////////////////////////////
