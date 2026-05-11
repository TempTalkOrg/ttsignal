# TTSignal.xcframework 与宿主 OpenSSL 符号冲突修复报告

> **状态**：已修复，对应 commit
> [`b3cc96a fix: symbols confict with boringssl`](https://github.com/3th1UOYgUtJkurSZ/ttsignal/commit/b3cc96a)。
>
> 修复仅改一个文件：[`ios/scripts/build-xcframework.sh`](./scripts/build-xcframework.sh)。
> 不影响 build-deps.sh / build-core.sh / 任何 C/C++/Swift 源代码，
> xcframework 公开 ABI（`tt_*` C 符号 + Swift 包装层）零变化。
>
> 本报告留作以后再踩到"内嵌 boringssl 的 iOS 静态库被宿主 app
> 的 OpenSSL 抢符号"这个坑时的参考。

## 1. 背景与症状

宿主 iOS app 把 `TTSignal.xcframework` 集成进来（SwiftPM / CocoaPods 都一样）后，
**第一次构造 `TTSignalConnector(config:)` 即崩**：

```
Thread 1 Crashed (SIGSEGV @ 0x73910043fd):
  0  0x73910043fd
  1  ::SSL_CTX_new(const SSL_METHOD *)
  2  xqc_create_client_ssl_ctx
  3  …
  6  tt_connector_create
  7  closure #1 in TTSignalConnector.init(config:)
 11  TTSignalConfig.withCConfig<OpaquePointer?>(_:)
 12  TTSignalConnector.init(config:)
```

环境关键事实：

* TTSignal.xcframework 里**自带**一份 BoringSSL（`libssl.a` + `libcrypto.a`，
  build-deps.sh 编出来），jquic 是用 BoringSSL 的头文件编译的。
* **宿主 app 早就集成了另一份 OpenSSL**——可能是直接的 `OpenSSL.xcframework` /
  CocoaPod，也可能跟着 gRPC、Firebase、Sentry、某加密 SDK 一起被传递引入。
* macOS NAPI / Linux / Android 路径都没事——它们没有这个"另一份 OpenSSL"。
  问题只在 iOS xcframework 上出现。

观察：用户用 Xcode 反汇编看 `SSL_CTX_new` 的指令，**确实是我们 BoringSSL 的代码**，
但崩在 `SSL_CTX_new` 内部某条调用上。

## 2. 根因分析

### 2.1 老的打包方式（修复前）

```bash
# build-xcframework.sh 老版本核心一行：
libtool -static -o libTTSignal.a \
    libttsignal_ios.a libjquic.a libssl.a libcrypto.a libenv.a
```

`libtool -static` 只是把多个 `.a` 的 member 拼到一个 `.a` 里——**不**改写
任何符号属性。结果：

* BoringSSL 的 5000+ 个公开 API（`_SSL_CTX_new`、`_TLS_method`、`_BIO_new`、
  `_X509_*`、`_EVP_*`、`_OPENSSL_malloc/free` …）以默认 `default` 可见性
  原样暴露在 `libTTSignal.a` 里，对宿主链接器**全部可见**。
* jquic 编译产物 `xqc_tls_ctx.c.o` 里调 BoringSSL 的每一个函数，都是
  `ARM64_RELOC_BRANCH26 extern=True` 的延迟引用——final-link 时由 ld 决定指向谁。

```
$ otool -rv xqc_tls_ctx.c.o
  3 × ARM64_RELOC_BRANCH26 _SSL_CTX_new           ← 等 ld 解析
  2 × ARM64_RELOC_BRANCH26 _SSL_CTX_set_min_proto_version
  3 × ARM64_RELOC_BRANCH26 _SSL_CTX_free
  …
```

### 2.2 ld 的 archive 解析行为

Apple `ld64` 对静态库的解析是 **按需 lazy load**：

1. 处理命令行 `-framework OpenSSL` → dylib 注册 `_SSL_CTX_new` 等导出符号。
2. 处理 `libTTSignal.a` → 拉入 `ttsignal_ios.o`（提供 `_tt_connector_create`）。
3. 链式拉到 `xqc_tls_ctx.c.o` → 它有 `U _SSL_CTX_new`。
4. ld 在 `.a` 的 SYMDEF 里找到 `ssl_lib.cc.o` 提供 `_SSL_CTX_new` →
   **strong static > dylib**，拉入 `ssl_lib.cc.o`。

到这一步，`SSL_CTX_new` 的入口确实是我们 BoringSSL 的——这是用户在 Xcode
里反汇编看到的。**但故事还没完**：

5. `ssl_lib.cc.o` 自己又有 122 个 `U` 符号（`_BIO_new`、`_CRYPTO_BUFFER_*`、
   `_OPENSSL_malloc`、`_ERR_put_error`、`_CBB_*`、`_CBS_*` …）。
6. ld 在解析这批 `U` 时，**先看已经加载的源**：dylib（OpenSSL.framework）
   已经提供了 `_BIO_new` / `_OPENSSL_malloc` 等等。
7. **dylib 已能满足 → ld 不再去 `libcrypto.a` 拉对应 `bio.cc.o` /
   `mem.cc.o`**。BoringSSL 自己的 BIO/mem 实现压根没被加载进 binary。
8. 结果：`ssl_lib.cc.o` 里所有 `bl _BIO_new` / `bl _OPENSSL_malloc` 都被
   reloc 到 OpenSSL.dylib 的实现。

### 2.3 崩溃机制（"半 OpenSSL 半 BoringSSL 的混合对象"）

```
BoringSSL SSL_CTX_new 入口     ← 我们的代码 ✓ (用户看到的)
    │
    ├── OPENSSL_malloc(sizeof SSL_CTX)
    │       ↓ ld 劫持
    │   OpenSSL.dylib 的 OPENSSL_malloc
    │       → 返回一块附带 OpenSSL debug-malloc 元数据头的内存
    │
    ├── 按 BoringSSL struct 偏移写字段
    │       → 字段写到了"看似对的位置"，但跟元数据头错位
    │
    ├── BIO_new(method)
    │       ↓ ld 劫持
    │   OpenSSL.dylib 的 BIO_new
    │       → 返回 OpenSSL 风格 BIO* （字段顺序 / refcount 位置都跟 BoringSSL 不一样）
    │
    ├── 把 BIO* 塞进 SSL_CTX，按 BoringSSL 期望的偏移写
    │       → 又一次错位
    │
    ├── ssl_create_cipher_list() / 任何一个内部初始化失败
    │       → 进 fail 分支
    │
    └── SSL_CTX_free(ret)            ← 这次是 BoringSSL 自己的 SSL_CTX_free！
            (同 .cc 同 .o 同 __TEXT,__text section，
             clang 在编译期已把 BL 立即偏移直接写死，
             不留 reloc，host OpenSSL 想替也替不了)
            │
            ├── 沿 BoringSSL 偏移读 ref counter / cipher_list / x509_method 等
            │       → 拿到的是 OpenSSL 写在那位置的内部状态
            ├── 把那个状态当指针解引用
            └── SIGSEGV @ 0x73910043fd
```

注意第三层"用户看到的崩溃栈顶"：

* **`SSL_CTX_free` 本身**是我们 BoringSSL 的代码，host OpenSSL 替换不了它。
  实证：`otool -rv ssl_lib.cc.o | grep _SSL_CTX_free` → 0 条 external reloc，
  因为 clang 对同 .cc 同 section 的 BL 直接写死立即偏移，连 reloc 都不留。
* 真正的破坏者是 `SSL_CTX_new` 内部那 122 个被 ld 劫持的辅助函数，
  它们在退出 `SSL_CTX_new` 之前就把 SSL_CTX 内存写成了混合状态。
* `SSL_CTX_free` 只是"最先访问到错位字段"的不幸者，相当于 **死后尸检的伤口**，
  不是凶器。

## 3. 修复方案选型

| 方案 | 改动范围 | 干净度 | 工作量 |
|---|---|---|---|
| **A. BoringSSL Symbol Prefix** | 改 build-deps.sh + build-core.sh，给 BoringSSL 和 jquic 都加 `-DBORINGSSL_PREFIX=TTBSSL_`，所有 `SSL_CTX_new` 在编译期被 macro 替换成 `TTBSSL_SSL_CTX_new` | 完全隔离。**gRPC、Firebase、Cronet 都用这个** | 中 |
| **B. xcframework 打包阶段 partial-link + localize symbols** | 只改 build-xcframework.sh：把每个 slice 的 `.a` 解开后 `ld -r -exported_symbols_list keep_tt.txt`，把除 `_tt_*` 之外的全部全局符号 demote 成 private extern | 工业标准，不如 prefix 干净，但效果等价 | 小 |

最终选**方案 B**——单文件改动，不需要改任何 C/C++ 代码，对 build-deps.sh /
build-core.sh / 各 deps 子项目零侵入。如果将来发现某个非 SSL 类的内部符号
也需要被宿主直接调用（目前没有这种 case），随时可以再叠加方案 A 进一步加固。

## 4. 修复实现

只改一个文件：[`ios/scripts/build-xcframework.sh`](./scripts/build-xcframework.sh)。
核心是把"原来 libtool 直接合并"换成"先 ld -r partial link、再 libtool 包装"。

### 4.1 KEEP_LIST：仅一行 glob

```bash
KEEP_LIST="$(mktemp -t ttsignal_keep.XXXXXX)"
cat > "${KEEP_LIST}" <<'EOF'
_tt_*
EOF
```

依赖项目早就建立好的命名约定：每一个公开 C 符号都以 `tt_` 开头
（Mach-O 加 leading underscore 后变成 `_tt_*`）。见 `ios_bridge.h` /
`ios_logger.h` / `AppleNetworkMonitor.mm`。

### 4.2 partial-link

```bash
merge_archive() {
    local out="$1" arch="$2" plat="$3" sdk="$4"
    shift 4

    # 1) 把每个 .a 解到独立子目录（避免 .o 同名互相覆盖）
    # 2) 收集所有 .o 路径到 -filelist
    # ...

    # 3) 关键一步：partial link，写死跨 .o 引用 + localize 非 _tt_ 全局
    xcrun ld -r \
        -arch "${arch}" \
        -platform_version "${plat}" "${DEPLOYMENT_TARGET}" "${sdk_version}" \
        -exported_symbols_list "${KEEP_LIST}" \
        -filelist "${objlist}" \
        -o "${merged}"

    # 4) 把单个 merged.o 包成静态归档（保持 xcframework 一 slice 一 .a 的结构）
    libtool -static -o "${out}" "${merged}"

    # 5) sanity check：nm -g 看 external symbol 是不是仅剩 _tt_*
    sanity_check_localized "${out}" "${arch}"
}
```

### 4.3 sanity check

```bash
sanity_check_localized() {
    local archive="$1" arch="$2"
    local leaks
    leaks=$(nm -g -arch "${arch}" "${archive}" \
        | awk '$2 != "U" && $3 !~ /^_tt_/ && $3 != "" { print $3 }' \
        | sort -u)
    if [ -n "${leaks}" ]; then
        echo "ERROR: ${archive} still exports non-_tt_ external symbols:" >&2
        echo "${leaks}" | head -n 40 | sed 's/^/  /' >&2
        exit 1
    fi
    echo "[xcframework] ${archive} (${arch}): only _tt_* exported (OK)"
}
```

意图：**新加公开 API 时如果忘了 `tt_` 前缀，构建会立刻在打包阶段失败**，
不让带病 xcframework 流出。这条强制的是"命名约定"而不仅仅是"链接干净"。

## 5. 实证

### 5.1 修复前后符号面对比（device-arm64 slice）

| 项目 | 修复前（libtool 直接合并） | 修复后（partial link） |
|---|---|---|
| `_SSL_CTX_new` 等 BoringSSL 全局函数 | `T`（external，宿主可见） | `t`（private extern，宿主不可见） |
| `_xqc_*` jquic 内部函数 | `T` | `t` |
| 已定义的 external 符号总数 | **5000+** | **19**（全部 `_tt_*` 公开 API） |
| `_SSL_CTX_new` 等的外部 reloc | 由 ld final-link 解析 | 0（partial link 已 hard-bind 到 `.a` 内偏移） |

### 5.2 修复后实测

```
=== 19 个 _tt_* 公开 API（全部 T） ===
_tt_connection_close, _tt_connection_close_stream, _tt_connection_connect,
_tt_connection_destroy, _tt_connection_restart, _tt_connection_send_packet,
_tt_connector_close, _tt_connector_close_sync, _tt_connector_create,
_tt_connector_create_connection, _tt_connector_destroy, _tt_connector_get_stats,
_tt_get_sdk_version, _tt_logger_set_callback,
_tt_netmon_query_default_ifindex, _tt_netmon_start, _tt_netmon_stop,
_tt_packet_create, _tt_packet_destroy

=== 验证以前导致冲突的符号已从外部不可见 ===
已定义的 external（非 _tt_）：0 行   ← 外部冲突面已彻底关闭

=== 残留的 295 个非 _tt_ external 全部是 U（undefined） ===
对 libSystem 的依赖（malloc、pthread_*、socket、recv、send …）
对 libc++ 的依赖（std::*、__cxa_* C++ ABI …）
对 Network.framework 的依赖（nw_path_monitor_*）
对 Apple 系统的依赖（dispatch_*、os_log_*、objc_* …）
全部必须保留为 U——宿主 app final-link 时由 iOS SDK 解析。
```

### 5.3 内部跨 .o 引用确实被 hard-bind

```
$ otool --reloc merged.o | grep -E "_(OPENSSL_malloc|OPENSSL_free|SSL_CTX_free|SSL_CTX_new|ERR_put_error|CRYPTO_new_ex_data)$"
(0 条 external reloc)
```

`ld -r` 阶段把所有跨 `.o` 的 `BR26` 全部 in-place 解析到本地偏移，
后续 host app final-link 没有任何符号"待解析"了。

## 6. 副作用与已知告警

### 6.1 ld warning（良性）

```
ld: warning: object file (...BCTaskEvent.cpp.o) was built for newer iOS
    Simulator version (14.0) than being linked (13.0)
```

来源：`build-deps.sh` / `build-core.sh` / `build-xcframework.sh` 三个脚本
默认 `IOS_DEPLOYMENT_TARGET=13.0`，但 deps 内部某些 `.o`（boringssl 部分文件、
env 部分文件）声明 14.0。`ld -r` 时显式 `-platform_version ios 13.0` 与
14.0 不一致 → 警告。**不影响产物**，最终 `LC_BUILD_VERSION` 取较低的 13.0，
xcframework 在 iOS 14+ 上正常运行。

要消掉的话：`IOS_DEPLOYMENT_TARGET=14.0 ./ios/scripts/build-xcframework.sh`，
或把三个脚本默认值都升到 14.0。这是 deployment 决策，跟 LiveKit iOS team
对齐之后再改。

### 6.2 backtrace 可读性

partial link 之后所有内部函数仍以小写 `t` / `s` / `d` 留在符号表里，
**dSYM / 崩溃栈解析不受影响**。如果将来想进一步压缩 binary 体积、
追求极致 isolation，可以再加 `ld -r -x` strip 掉所有内部符号——但那时崩溃
backtrace 就只能看到 `_tt_*` 这一层，再往下都是 `<unknown>`，划不来，
**目前不打算做**。

## 7. 后续维护要点

* **新加公开 C API 必须以 `tt_` 前缀命名**。否则 `sanity_check_localized`
  会在打包阶段直接报错并打印泄漏的符号列表，构建不会过。
* **不要把任何 BoringSSL / xquic 的接口直接暴露给 Swift**。Swift 应该只
  通过 `tt_*` 这一层访问。一旦在 Swift 里 import OpenSSL 类型或调
  `SSL_CTX_*`，Module 就会要求那些符号 external，整套 firewall 失效。
* **rtc-client（Node.js / NAPI 路径）不受这次修复影响**。NAPI 走 dynamic
  library + dlopen，链接顺序不会触发同样的劫持；后台 server 也不存在
  "宿主 app 自带 OpenSSL"这个场景。
* **livekit-client-swift 集成时也是同样路径**——只要它通过 SwiftPM 引入
  `TTSignal.xcframework`、且整个流程不暴露 `_SSL_*`，自动免疫。

## 8. 参考

* gRPC iOS 处理同类问题的 prefix 方案：
  <https://github.com/grpc/grpc/blob/master/templates/src/objective-c/BoringSSL-GRPC.podspec.template.j2>
* WebRTC iOS 也用同样的 partial-link + localize 套路：
  <https://webrtc.googlesource.com/src/+/main/api/BUILD.gn>
* Apple ld64 关于 `-r` / `-exported_symbols_list` / private extern 的语义：
  `man ld` "PARTIAL LINKING" 节
* BoringSSL 官方对 PREFIX 机制的说明：
  [`deps/boringssl/src/BUILDING.md`](../deps/boringssl/src/BUILDING.md)（搜 `BORINGSSL_PREFIX`）
