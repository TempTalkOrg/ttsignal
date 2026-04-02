///////////////////////////////////////////////////////////////////////////////
// file : StdAfx.h
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#ifndef STDAFX_H_INCLUDED__
#define STDAFX_H_INCLUDED__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN     
#endif // WIN32_LEAN_AND_MEAN

#include <BC/Config.h>
#include <BC/Exports.h>
#include <BC/Utils.h>
#include <BC/BCBuffer.h>
#include <BC/BCList.h>
#include <BC/BCMap.h>
#include <BC/BCVector.h>
#include <BC/ByteOrder.h>
#include <BC/BCStream.h>
#include <BC/BCNodeList.h>
#include <BC/BCTask.h>
#include <BC/BCThread.h>
#include <BC/BCTimer.h>
#include <BC/BCStrPtrLen.h>
#include <BC/BCLog.h>
#include <BC/BCFCodec.h>

namespace node{}
using namespace node;

#ifdef _MSC_VER
#pragma warning(disable:4996)
#if _MSC_VER < 1800
#define snprintf	_snprintf
#endif // _MSC_VER
#include <winscard.h>
#endif // _MSC_VER

#ifndef UINT64_C
#define UINT64_C(x)		((x) + (UINT64_MAX - UINT64_MAX))
#endif // UINT64_C


#if defined(linux) || defined(__linux) || defined(__linux__)
#undef __LINUX__
#define __LINUX__	1
#endif


#endif // STDAFX_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : StdAfx.h
///////////////////////////////////////////////////////////////////////////////
