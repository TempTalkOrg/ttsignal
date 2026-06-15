///////////////////////////////////////////////////////////////////////////////
// file   : NetworkRouteLookup.cpp
// author : anto
//
// Catch-all fallback definition of tt_route_lookup_ifindex().
//
// Real implementations live in:
//   * apple/AppleRouteLookup.cpp   — macOS PF_ROUTE / RTM_GET
//   * linux/LinuxRouteLookup.cpp   — Linux NETLINK_ROUTE / RTM_GETROUTE
//   * win32/WinRouteLookup.cpp     — Windows GetBestInterfaceEx
//
// The Apple, Linux, and Windows builds pull in their respective platform
// file (via the per-platform CMakeLists in src/cpp/apple/, linux/,
// win32/). This top-level file then compiles to an empty translation
// unit on those platforms, so the linker only ever sees one definition.
//
// The Android JNI build (android/app/src/main/cpp/CMakeLists.txt) globs
// only ${src}/cpp/*.cpp and does NOT pick up the subdirectory files;
// this file IS picked up by that glob, and the stub satisfies the link
// reference from UDPSender.cpp. On Android the function returns 0,
// which UDPSender already treats as "lookup unavailable" — the
// IP_BOUND_IF / IP_UNICAST_IF binding (if any, set up via
// android_setsocknetwork by the JNI Network.bindSocket() path) is left
// untouched.
//
// iOS / iPadOS / tvOS / watchOS also fall under __APPLE__ and rely on
// the stub branch inside AppleRouteLookup.cpp itself (PF_ROUTE headers
// are not exposed by the iOS SDK). So this file is empty there too.
///////////////////////////////////////////////////////////////////////////////

#include "NetworkRouteLookup.h"

#if !defined(__APPLE__) &&                                          \
    !(defined(__linux__) && !defined(__ANDROID__) && !defined(OS_ANDROID)) && \
    !defined(_WIN32)

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* /*dst*/,
                                            socklen_t /*dst_len*/)
{
    return 0;
}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
