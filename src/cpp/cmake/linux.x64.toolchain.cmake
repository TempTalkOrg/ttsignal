# toolchain.cmake - 交叉编译工具链配置文件

# 设置目标系统
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 设置交叉编译工具链路径
# 请根据实际安装路径修改以下变量
set(CROSS_COMPILE_PREFIX "/opt/homebrew/bin/x86_64-unknown-linux-gnu")

# 指定C/C++编译器
set(CMAKE_C_COMPILER ${CROSS_COMPILE_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE_PREFIX}-g++)

# 指定汇编器和链接器
set(CMAKE_ASM_COMPILER ${CROSS_COMPILE_PREFIX}-gcc)
set(CMAKE_LINKER ${CROSS_COMPILE_PREFIX}-ld)

# 指定其他工具
set(CMAKE_OBJCOPY ${CROSS_COMPILE_PREFIX}-objcopy)
set(CMAKE_OBJDUMP ${CROSS_COMPILE_PREFIX}-objdump)
set(CMAKE_STRIP ${CROSS_COMPILE_PREFIX}-strip)
set(CMAKE_RANLIB ${CROSS_COMPILE_PREFIX}-ranlib)
set(CMAKE_AR ${CROSS_COMPILE_PREFIX}-ar)

# 设置目标系统根目录（sysroot）
# 如果你有完整的系统根目录，请取消注释并设置正确的路径
set(CMAKE_SYSROOT "/opt/homebrew/opt/x86_64-unknown-linux-gnu/toolchain/x86_64-unknown-linux-gnu/sysroot")

# 设置查找路径
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 添加编译标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC" CACHE STRING "C flags")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC" CACHE STRING "C++ flags")

# 兼容性设置
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 输出信息
message(STATUS "Cross-compiling for: ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "Using C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Using CXX compiler: ${CMAKE_CXX_COMPILER}")

# 针对该项目的特殊设置
add_definitions(
    -D__STDC_LIMIT_MACROS 
    -DHAVE_PTHREAD 
    -DCR_USE_FALLBACKS_FOR_GCC_WITH_LIBCXX
    -DNOMINMAX
    -DXQC_ENABLE_BBR2
    -DXQC_ENABLE_RENO
    -DXQC_ENABLE_UNLIMITED
    -DXQC_ENABLE_COPA
)