///////////////////////////////////////////////////////////////////////////////
// file : IHandler.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#include "IHandler.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

//////////////////////////////////////////////////////////////////////////
/// class : RAWEvent
//////////////////////////////////////////////////////////////////////////

RAWEvent::RAWEvent() 
	: IAVPacket(NULL)
{
	//
}

RAWEvent::RAWEvent(const BCEventItemS &refEvent)
	: IAVPacket(NULL)
	, m_sEvent(refEvent)
{
	//
}

RAWEvent::~RAWEvent()
{
	//
}

BCRESULT RAWEvent::Create() {
	return BC_R_SUCCESS;
}

IAVPacket	*	RAWEvent::Clone() {
	RAWEvent *pPkt = new RAWEvent();
	pPkt->m_sEvent = m_sEvent;
	return pPkt;
}

//////////////////////////////////////////////////////////////////////////
/// class : RAWFrame
//////////////////////////////////////////////////////////////////////////

RAWFrame::RAWFrame() 
	: IAVPacket(NULL)
{
	Reset();
}

RAWFrame::RAWFrame(const RAWFrame &other)
	: IAVPacket(NULL)
{
	operator=(other);
}

RAWFrame::~RAWFrame()
{
	Reset();
}

BCRESULT RAWFrame::Create(
	uint8_t **data_, 
	int *linesize_, 
	int format_, 
	int width_, 
	int height_)
{
	type = MTYPE_VIDEODATA;
	format = format_;
	width = width_;
	height = height_;
	switch (format)
	{
	case BC_PIX_YUVA420P:
		data[3] = (LPBYTE)pool.memdup(data_[3], linesize_[3] * height);
		linesize[3] = linesize_[3];
	case BC_PIX_YUV420P:
		data[0] = (LPBYTE)pool.memdup(data_[0], linesize_[0] * height);
		linesize[0] = linesize_[0];
		data[1] = (LPBYTE)pool.memdup(data_[1], linesize_[1] * height / 2);
		linesize[1] = linesize_[1];
		data[2] = (LPBYTE)pool.memdup(data_[2], linesize_[2] * height / 2);
		linesize[2] = linesize_[2];
		break;
	default:
		break;
	}
	return BC_R_SUCCESS;
}


BCRESULT RAWFrame::Create(
	uint8_t **data_, 
	int *linesize_, 
	int format_, 
	int channels_, 
	int sampleRate, 
	int samples)
{
	type = MTYPE_AUDIODATA;
	format = format_;
	channels = channels_;
	sample_rate = sampleRate;
	nb_samples = samples;
	linesize[0] = linesize_[0];
	switch (format)
	{
	case BC_SAMPLE_S16P:
		for (int i = 0; i < channels; i++)
		{
			data[i] = (LPBYTE)pool.memdup(data_[i], linesize[0]);
		}
		break;
	default:
		break;
	}
	return BC_R_SUCCESS;
}

IAVPacket	*	RAWFrame::Clone() {
	RAWFrame *pPkt = new RAWFrame(*this);
	return pPkt;
}

RAWFrame &RAWFrame::operator=(const RAWFrame &other)
{
	Reset();
	type = other.type;
	width = other.width;
	height = other.height;
	nb_samples = other.nb_samples;
	format = other.format;
	key_frame = other.key_frame;
	pts = other.pts;
	pkt_dts = other.pkt_dts;
	quality = other.quality;
	opaque = other.opaque;
	sample_rate = other.sample_rate;
	channel_layout = other.channel_layout;
	flags = other.flags;
	channels = other.channels;
	switch (type)
	{
	case MTYPE_AUDIODATA:
		{
			linesize[0] = other.linesize[0];
			switch (format)
			{
			case BC_SAMPLE_S16P:
				for (int i = 0; i < channels; i++)
				{
					data[i] = (LPBYTE)pool.memdup(other.data[i], other.linesize[0]);
				}
				break;
			default:
				break;
			}
		}
		break;
	case MTYPE_VIDEODATA:
		{
			memcpy2(linesize, other.linesize, sizeof(linesize));
			switch (format)
			{
			case BC_PIX_YUVA420P:
				data[3] = (LPBYTE)pool.memdup(other.data[3], linesize[3] * height);
			case BC_PIX_YUV420P:
				data[0] = (LPBYTE)pool.memdup(other.data[0], linesize[0] * height);
				data[1] = (LPBYTE)pool.memdup(other.data[1], linesize[1] * height/2);
				data[2] = (LPBYTE)pool.memdup(other.data[2], linesize[2] * height/2);
				break;
			default:
				break;
			}
		}
		break;
	default:
		break;
	}
	return *this;
}

void RAWFrame::Reset()
{
	type = MTYPE_INVALID;
	memset(data, 0, sizeof(data));
	memset(linesize, 0, sizeof(linesize));
	extended_data = NULL;
	width = 0;
	height = 0;
	nb_samples = 0;
	format = 0;
	key_frame = 0;
	pts = 0;
	pkt_dts = 0;
	quality = 0;
	opaque = NULL;
	sample_rate = 0;
	channel_layout = 0;
	flags = 0;
	channels = 0;
	pool.Clear();
}

///////////////////////////////////////////////////////////////////////////////
// Class : IPacketSource
///////////////////////////////////////////////////////////////////////////////

IPacketSource::IPacketSource()
{
		//
}

IPacketSource::~IPacketSource()
{
	RemoveAllDataListeners();
}

BCRESULT IPacketSource::AddDataListener(IPacketHandler *pHandler, bool cocall)
{
	BCRESULT result = BC_R_SUCCESS;

	if (!pHandler)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sListenersLock);
	PacketHandlerQueue::iterator iter, iterEnd;
	iter = m_vecDataListeners.begin();
	iterEnd = m_vecDataListeners.end();
	for (; iter != iterEnd; iter++)
	{
		if (pHandler == *iter)
		{
			result = BC_R_EXISTS;
			break;
		}
	}
	if (result == BC_R_SUCCESS)
	{
		m_vecDataListeners.push_back(pHandler);
		if (cocall)
		{
			result = pHandler->AttachPacketSource(this, false);
		}
		OnAddDataListener(pHandler);
	}
	return result;
}

BCRESULT IPacketSource::RemoveDataListener(IPacketHandler *pHandler, bool cocall)
{
	BCRESULT result = BC_R_NOTFOUND;

	if (!pHandler)
	{
		return BC_R_INVALIDARG;
	}
	else
	{
		BCSpinMutex::Owner lock(m_sListenersLock);
		PacketHandlerQueue::iterator iter, iterEnd;
		iter = m_vecDataListeners.begin();
		iterEnd = m_vecDataListeners.end();
		for (; iter != iterEnd; iter++)
		{
			if (pHandler == *iter)
			{
				m_vecDataListeners.erase(iter);
				result = BC_R_SUCCESS;
				break;
			}
		}
	}
	if (result == BC_R_SUCCESS)
	{
		if (cocall)
		{
			pHandler->DetachPacketSource(false);
		} 
		OnRemoveDataListener(pHandler);
	}
	return result;
}

void IPacketSource::RemoveAllDataListeners()
{
	PacketHandlerQueue::iterator iter, iterEnd;
	BCSpinMutex::Owner lock(m_sListenersLock);
	iter = m_vecDataListeners.begin();
	iterEnd = m_vecDataListeners.end();
	for (; iter != iterEnd; iter++)
	{
		if (*iter)
		{
			(*iter)->DetachPacketSource(false);
		}
	}
	m_vecDataListeners.clear();
}

void IPacketSource::SendPacketToListeners(IAVPacket *pPkt)
{
	if (pPkt)
	{
		BCSpinMutex::Owner lock(m_sListenersLock);
		if (!m_vecDataListeners.size())
		{
			BC_SAFE_DELETE_PTR(pPkt);
		}
		else
		{
			PacketHandlerQueue::iterator iter, iterEnd;

			// Notify data handlers
			iter = m_vecDataListeners.begin();
			iterEnd = m_vecDataListeners.end();
			for (size_t i = 0; iter != iterEnd; iter++)
			{
				if (*iter)
				{
					i++;
					if (i < m_vecDataListeners.size())
					{
						(*iter)->HandlePacket(pPkt->Clone());
					}
					else
					{
						(*iter)->HandlePacket(pPkt);
					}
				}
			}
		}
	}
}

void IPacketSource::SendPacketToListeners(PPacket *pPacket)
{
	if (pPacket)
	{
		ScopedPointer<PPacket> dtor(pPacket);

		BCSpinMutex::Owner lock(m_sListenersLock);
		if (m_vecDataListeners.size() > 0)
		{
			IAVPacket *pPkt = new RTMPPacket(dtor.Release());
			if (pPkt)
			{
				PacketHandlerQueue::iterator iter, iterEnd;
				// Notify data handlers
				iter = m_vecDataListeners.begin();
				iterEnd = m_vecDataListeners.end();
				for (size_t i = 0; iter != iterEnd; iter++)
				{
					if (*iter)
					{
						i++;
						if (i < m_vecDataListeners.size())
						{
							(*iter)->HandlePacket(pPkt->Clone());
						}
						else
						{
							(*iter)->HandlePacket(pPkt);
						}
					}
				}
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Class : IPacketHandler
///////////////////////////////////////////////////////////////////////////////

IPacketHandler::IPacketHandler()
	: m_pPacketSource(NULL)
{
	//
}

IPacketHandler::~IPacketHandler()
{
	if (m_pPacketSource)
	{
		m_pPacketSource->RemoveDataListener(this, false);
		m_pPacketSource = NULL;
	}
}

BCRESULT IPacketHandler::AttachPacketSource(
	IPacketSource *pSrc, 
	bool cocall)
{
	BCRESULT result;

	if (!pSrc)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sSourceLock);
	if (m_pPacketSource)
	{
		m_pPacketSource->RemoveDataListener(this, false);
	}
	m_pPacketSource = pSrc;
	result = BC_R_SUCCESS;
	if (cocall)
	{
		result = m_pPacketSource->AddDataListener(this, false);
	}
	return result;
}

IPacketSource	*IPacketHandler::DetachPacketSource(bool cocall)
{
	IPacketSource *pSrc = NULL;

	BCSpinMutex::Owner lock(m_sSourceLock);
	pSrc = m_pPacketSource;
	m_pPacketSource = NULL;
	if (pSrc && cocall)
	{
		pSrc->RemoveDataListener(this, false);
	}
	return pSrc;
}

BCRESULT IPacketHandler::PostListenerEvent(const BCEventItemS &refEvent)
{
	BCSpinMutex::Owner lock(m_sSourceLock);
	if (m_pPacketSource)
	{
		m_pPacketSource->PostSourceEvent(refEvent);
		return BC_R_SUCCESS;
	}
	return BC_R_INVALIDPTR;
}


///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP


///////////////////////////////////////////////////////////////////////////////
// End of file : IHandler.cpp
///////////////////////////////////////////////////////////////////////////////