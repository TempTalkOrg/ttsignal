#!/bin/bash

# Android NDK路径配置
NDK_PATH="${ANDROID_NDK_HOME:-/opt/android-ndk}"
if [ ! -d "$NDK_PATH" ]; then
    echo "错误: 未找到Android NDK，请设置ANDROID_NDK_HOME环境变量"
    exit 1
fi

# 自动检测宿主平台 (darwin-x86_64, darwin-aarch64, linux-x86_64, ...)
detect_host_tag() {
    local os arch
    case "$(uname -s)" in
        Darwin) os="darwin" ;;
        Linux)  os="linux" ;;
        *)      echo "错误: 不支持的操作系统 $(uname -s)"; exit 1 ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64)  arch="x86_64" ;;
        arm64|aarch64) arch="aarch64" ;;
        *)             echo "错误: 不支持的架构 $(uname -m)"; exit 1 ;;
    esac

    local tag="${os}-${arch}"
    local prebuilt_dir="$NDK_PATH/prebuilt/${tag}"

    # NDK 可能没有当前架构的 prebuilt，尝试 fallback 到 x86_64
    if [ ! -d "$prebuilt_dir" ]; then
        local fallback="${os}-x86_64"
        local fallback_dir="$NDK_PATH/prebuilt/${fallback}"
        if [ -d "$fallback_dir" ]; then
            echo "注意: ${tag} 不存在, 使用 ${fallback}" >&2
            tag="$fallback"
            prebuilt_dir="$fallback_dir"
        fi
    fi

    if [ ! -d "$prebuilt_dir" ]; then
        echo "错误: 找不到 NDK prebuilt 目录, 尝试过: ${os}-${arch}, ${os}-x86_64" >&2
        exit 1
    fi
    echo "$tag"
}

HOST_TAG=$(detect_host_tag)
echo "检测到宿主平台: $HOST_TAG"

# 工作目录配置
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE_LISTS_PATH="$PROJECT_ROOT/app/src/main/cpp"
BUILD_ROOT="$PROJECT_ROOT/build"

# Android配置
MIN_SDK_VERSION=21
CMAKE_VERSION="3.22.1"

# 工具链配置
TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake"

# 支持的架构
ARCHITECTURES=("arm64-v8a" "armeabi-v7a")
ABI_TRIPLETS=("aarch64-linux-android" "arm-linux-androideabi")
MIN_SDK_VERSIONS=("21" "21")

# Strip工具配置 (使用检测到的平台)
STRIP_TOOL="$NDK_PATH/toolchains/llvm/prebuilt/${HOST_TAG}/bin/llvm-strip"

# 编译函数
build_for_architecture() {
    local arch=$1
    local abi_triplet=$2
    local min_sdk_version=$3
    local build_dir="$BUILD_ROOT/$arch"

    echo "开始编译 $arch 架构..."

    # 创建构建目录
    mkdir -p "$build_dir"
    cd "$build_dir"

    if [ -f "libsignal.so" ]; then
        echo "删除已存在libsignal.so"
        rm -f "libsignal.so"
        if [ $? -ne 0 ]; then
            echo "错误: 删除libsignal.so失败"
            exit 1
        fi
    fi

    # 配置CMake（仅在Makefile不存在或CMakeLists.txt比Makefile新时）
    if [ ! -f "Makefile" ] || [ "$CMAKE_LISTS_PATH/CMakeLists.txt" -nt "Makefile" ]; then
        echo "首次配置或CMakeLists.txt已更新，正在重新配置..."

        cmake "$CMAKE_LISTS_PATH" \
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
            -DANDROID_ABI="$arch" \
            -DANDROID_PLATFORM="android-$min_sdk_version" \
            -DCMAKE_BUILD_TYPE=Release \
            -DANDROID_NDK="$NDK_PATH" \
            -DCMAKE_MAKE_PROGRAM="$NDK_PATH/prebuilt/${HOST_TAG}/bin/make" \
            -DCMAKE_C_COMPILER="$NDK_PATH/toolchains/llvm/prebuilt/${HOST_TAG}/bin/$abi_triplet$min_sdk_version-clang" \
            -DCMAKE_CXX_COMPILER="$NDK_PATH/toolchains/llvm/prebuilt/${HOST_TAG}/bin/$abi_triplet$min_sdk_version-clang++"

        if [ $? -ne 0 ]; then
            echo "错误: $arch 架构CMake配置失败"
            exit 1
        fi
    fi

    # 编译
    cmake --build . --target all -j8

    if [ $? -ne 0 ]; then
        echo "错误: $arch 架构编译失败"
        exit 1
    fi

    echo "$arch 架构编译完成"
}

# Strip函数 - 减小.so文件大小
strip_so_files() {
    local arch=$1
    local build_dir="$BUILD_ROOT/$arch"

    # 检查strip工具是否存在
    if [ ! -f "$STRIP_TOOL" ]; then
        echo "警告: Strip工具 $STRIP_TOOL 不存在，跳过strip处理"
        return
    fi

    echo "开始strip $arch 架构的.so文件..."

    # 查找并strip所有.so文件
    find "$build_dir" -name "*.so" -type f | while read -r so_file; do
        echo "正在处理: $so_file"
        "$STRIP_TOOL" --strip-unneeded "$so_file"
        if [ $? -eq 0 ]; then
            echo "成功strip: $so_file"
        else
            echo "警告: strip $so_file 失败"
        fi
    done

    echo "$arch 架构strip处理完成"
}

# 主编译流程
main() {
    echo "开始为Android平台编译C++代码"
    echo "NDK路径: $NDK_PATH"
    echo "宿主平台: $HOST_TAG"
    echo "项目路径: $PROJECT_ROOT"

    # 遍历所有架构并编译
    for i in "${!ARCHITECTURES[@]}"; do
        build_for_architecture "${ARCHITECTURES[$i]}" "${ABI_TRIPLETS[$i]}" "${MIN_SDK_VERSIONS[$i]}"
        # 编译完成后进行strip处理
        strip_so_files "${ARCHITECTURES[$i]}"
    done

    echo "所有架构编译完成"
    echo "输出文件位于: $BUILD_ROOT"
}

# 清理函数
clean() {
    echo "清理构建目录..."
    rm -rf "$BUILD_ROOT"
    echo "清理完成"
}

# 参数处理
case "$1" in
    clean)
        clean
        ;;
    "")
        main
        ;;
    *)
        echo "用法: $0 [clean]"
        echo "  clean: 清理所有构建产物"
        exit 1
        ;;
esac
