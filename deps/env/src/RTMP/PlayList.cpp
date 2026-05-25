
#include "Precompile.h"
#include <RTMP/FLVUtils.h>
#include <RTMP/PlayList.h>

using namespace FLVUtils;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

///////////////////////////////////////////////////////////////////////////////
/// class : PlayItem
///////////////////////////////////////////////////////////////////////////////

//IMPLEMENT_FIXED_ALLOC(PlayItem, 100);

PlayItem::PlayItem()
	: m_nStart(-2)
	, m_nDuration(-1)
{
	//
}

PlayItem::PlayItem(
	const BCPString &strStrmName,
	int32_t nStart /* = -2 */,
	int32_t nDuration /* = -1 */)
		: m_sUri(strStrmName)
		, m_nStart(nStart)
		, m_nDuration(nDuration)
{
	//
}

PlayItem::PlayItem(const PlayItem &playItem)
	: m_sUri(playItem.m_sUri)
	, m_nStart(playItem.m_nStart)
	, m_nDuration(playItem.m_nDuration)
{
	//
}

PlayItem::~PlayItem()
{
	//
}

void PlayItem::operator = (const PlayItem &playItem)
{
	m_sUri = playItem.m_sUri;
	m_nStart = playItem.m_nStart;
	m_nDuration = playItem.m_nDuration;
}

///////////////////////////////////////////////////////////////////////////////
/// class : PlayList
///////////////////////////////////////////////////////////////////////////////

//IMPLEMENT_FIXED_ALLOC(PlayList, 100);

PlayList::PlayList()
	: m_nCrrentItem(0)
{
	//
}

PlayList::~PlayList()
{
	Flush();
}

uint32_t PlayList::AddItem(
	const BCPString &strStrmName,
	int32_t nStart /* = -2 */,
	int32_t nDuration /* = -1 */)
{
	PlayItem *pNewItem;

	pNewItem = new PlayItem(strStrmName, nStart, nDuration);
	m_vecPlayList.push_back(pNewItem);
	return m_vecPlayList.size();
}

PlayItem * PlayList::Current()
{
	return m_vecPlayList[m_nCrrentItem];
}

PlayItem * PlayList::NextItem()
{
	if (m_nCrrentItem + 2 <= m_vecPlayList.size())
	{
		m_nCrrentItem++;
		return m_vecPlayList[m_nCrrentItem];
	}
	return NULL;
}

void PlayList::Rewind()
{
	m_nCrrentItem = 0;
}

uint32_t PlayList::Flush()
{
	uint32_t nSize = m_vecPlayList.size();
	for (uint32_t i = 0;i < nSize;i++)
	{
		PlayItem *pItem = m_vecPlayList[i];
		if (pItem)
		{
			delete pItem;
			m_vecPlayList[i] = NULL;
		}
	}
	m_vecPlayList.clear();
	return nSize;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
