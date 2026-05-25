///////////////////////////////////////////////////////////////////////////////
// file : AppleNetworkMonitor.h
// author : anto
//
// Apple-specific (iOS + macOS) implementation of the platform-agnostic
// INetworkPathMonitor interface, backed by Network.framework's
// nw_path_monitor_t. Other platforms (Linux netlink, Windows
// NotifyIpInterfaceChange) live in their own peer headers but expose the
// same `tt_netmon_*` symbols, so SMPConnector can call them uniformly.
//
// This header exists primarily so the iOS Swift binding's module map can
// pick the API up via a stable filename. macOS NAPI builds use the
// platform-agnostic interface in INetworkPathMonitor.h directly.
///////////////////////////////////////////////////////////////////////////////
#ifndef TTSIGNAL_APPLE_NETWORK_MONITOR_H_INCLUDED__
#define TTSIGNAL_APPLE_NETWORK_MONITOR_H_INCLUDED__

#include "INetworkPathMonitor.h"

#endif // TTSIGNAL_APPLE_NETWORK_MONITOR_H_INCLUDED__
