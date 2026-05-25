# iOS toolchain for ttsignal core. Style-matched with macos.x64.toolchain.cmake.
#
# The caller must set IOS_PLATFORM to one of:
#   OS64               -> iphoneos arm64 (device)
#   SIMULATOR_ARM64    -> iphonesimulator arm64 (Apple-silicon Mac)
#   SIMULATOR_X64      -> iphonesimulator x86_64 (Intel Mac)
#
# Optional:
#   IOS_DEPLOYMENT_TARGET  default 13.0 (matches LiveKit iOS SDK floor)
#
# This wrapper uses CMake 3.14+'s native iOS cross-compile support — it's
# simpler than the upstream deps/jquic/cmake/ios.toolchain.cmake (which
# does not have a "simulator arm64" PLATFORM out of the box).

if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS64" CACHE STRING "Target iOS slice")
endif()

if(NOT DEFINED IOS_DEPLOYMENT_TARGET)
    set(IOS_DEPLOYMENT_TARGET "13.0" CACHE STRING "iOS deployment target")
endif()

if(IOS_PLATFORM STREQUAL "OS64")
    set(_ttsignal_sdk    "iphoneos")
    set(_ttsignal_arch   "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR_ARM64")
    set(_ttsignal_sdk    "iphonesimulator")
    set(_ttsignal_arch   "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR_X64")
    set(_ttsignal_sdk    "iphonesimulator")
    set(_ttsignal_arch   "x86_64")
else()
    message(FATAL_ERROR
        "Unknown IOS_PLATFORM='${IOS_PLATFORM}'. "
        "Use OS64 / SIMULATOR_ARM64 / SIMULATOR_X64.")
endif()

set(CMAKE_SYSTEM_NAME       iOS)
set(CMAKE_SYSTEM_VERSION    ${IOS_DEPLOYMENT_TARGET})
set(CMAKE_SYSTEM_PROCESSOR  ${_ttsignal_arch})
set(CMAKE_OSX_SYSROOT       ${_ttsignal_sdk})
set(CMAKE_OSX_ARCHITECTURES ${_ttsignal_arch} CACHE STRING "Build architecture for iOS" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET ${IOS_DEPLOYMENT_TARGET}
        CACHE STRING "Minimum iOS deployment target" FORCE)

# Resolve the actual SDK path so dependency CMakeLists that read
# CMAKE_OSX_SYSROOT_INT (e.g. boringssl helpers) find headers.
execute_process(COMMAND xcrun --sdk ${_ttsignal_sdk} --show-sdk-path
    OUTPUT_VARIABLE _ttsignal_sdk_path
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_ttsignal_sdk_path)
    set(CMAKE_OSX_SYSROOT     ${_ttsignal_sdk_path} CACHE STRING "" FORCE)
    set(CMAKE_OSX_SYSROOT_INT ${_ttsignal_sdk_path} CACHE INTERNAL "")
endif()

# Pick clang from the same SDK.
execute_process(COMMAND xcrun --sdk ${_ttsignal_sdk} -find clang
    OUTPUT_VARIABLE _ttsignal_clang
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND xcrun --sdk ${_ttsignal_sdk} -find clang++
    OUTPUT_VARIABLE _ttsignal_clangxx
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_ttsignal_clang)
    set(CMAKE_C_COMPILER   ${_ttsignal_clang}   CACHE FILEPATH "" FORCE)
endif()
if(_ttsignal_clangxx)
    set(CMAKE_CXX_COMPILER ${_ttsignal_clangxx} CACHE FILEPATH "" FORCE)
endif()

# Use Apple's libtool — same trick the deps/jquic upstream toolchain uses,
# necessary for static libraries from Xcode 7+.
execute_process(COMMAND xcrun --sdk ${_ttsignal_sdk} -find libtool
    OUTPUT_VARIABLE _ttsignal_libtool
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_ttsignal_libtool)
    set(CMAKE_C_CREATE_STATIC_LIBRARY
        "${_ttsignal_libtool} -static -o <TARGET> <LINK_FLAGS> <OBJECTS> ")
    set(CMAKE_CXX_CREATE_STATIC_LIBRARY
        "${_ttsignal_libtool} -static -o <TARGET> <LINK_FLAGS> <OBJECTS> ")
endif()

# Cross-compile search rules — only look at the iOS sysroot for libs and
# includes, but allow programs (perl, go, etc. used by boringssl) on the
# host system.
set(CMAKE_FIND_ROOT_PATH ${CMAKE_OSX_SYSROOT_INT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Disable bitcode by default — Apple deprecated it in Xcode 14 and our
# downstream xcframework consumers don't want it.
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE "NO" CACHE INTERNAL "")

# Disable code signing for the static archives we build.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED   "NO" CACHE INTERNAL "")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED    "NO" CACHE INTERNAL "")

# Match upstream behaviour: pthread is always linked.
set(CMAKE_THREAD_LIBS_INIT "-lpthread")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_PTHREADS_INIT 1)

# Re-export to the rest of the build (used in src/CMakeLists.txt to fold
# ios bridge sources / link libs into the framework target).
set(TARGET_PLATFORM_IOS ON)
add_definitions(-DTARGET_OS_IOS=1)

message(STATUS "ttsignal iOS toolchain: PLATFORM=${IOS_PLATFORM} "
               "ARCH=${_ttsignal_arch} SDK=${_ttsignal_sdk} "
               "MIN=${IOS_DEPLOYMENT_TARGET} SYSROOT=${CMAKE_OSX_SYSROOT}")
