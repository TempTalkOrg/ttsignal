///////////////////////////////////////////////////////////////////////////////
// file   : TlsContext.h
// author : anto
//
// SSL_CTX 工厂与证书校验。校验逻辑从 SMPConnector.cpp:3441-3536 抽出，改写
// 成不依赖 xquic 回调签名的独立函数，供 QUIC 侧与 TCP 侧（HTTP/WS）共用。
//
// 与 SMPConnector 的唯一行为差异：caCertsPem 为空时，SMP 是"跳过校验"，
// 本模块是"回落系统信任库"。理由是 https/wss 常用于访问公网 API，默认不
// 校验存在中间人风险；SMP 连的是自家 SFU，行为保持不变。
//
// 依赖说明：本文件及 TlsContext.cpp 刻意不 #include "Utils.h"、不调用
// BC 的 LogQ/LogCustom 日志族——Utils.h 会引入 <xquic/xquic.h>，且
// LogQ 系列符号需要链接 libenv，两者都与"不依赖 xquic 的独立模块"这一
// 设计目标冲突（TlsContext_test.cpp 头部的编译命令也证明了这一点：只链
// -lssl -lcrypto，不链 libenv/xquic）。因此本模块只通过 outErr 返回错误
// 详情；调用方（如 SMPConnector.cpp，本就链接了 libenv）负责把 outErr
// 转交给自己的 LogQ 做落地。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_TLS_CONTEXT_H
#define TT_TLS_CONTEXT_H

#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <BC/Config.h>

struct TlsConfig
{
    std::string caCertsPem;                 // 空 → 回落系统信任库
    std::string spkiPin;                    // base64 SPKI pin，可选
    std::string clientCertFile;             // 双向 TLS，可选
    std::string clientKeyFile;
    std::string clientKeyPassword;
    bool        insecureSkipVerify = false; // 仅供自签调试
    void*       loggerCtx          = nullptr;
};

class TlsContext
{
public:
    // 建 SSL_CTX。返回值由调用方 SSL_CTX_free。失败返回 nullptr 并填 outErr。
    //
    // 固定项：SSL_CTX_set_min_proto_version(TLS1_2_VERSION)、SSL_VERIFY_PEER
    // （insecureSkipVerify 时为 SSL_VERIFY_NONE）、SSL_OP_NO_COMPRESSION。
    //
    // caCertsPem 为空时依次尝试：
    //   1. SSL_CTX_set_default_verify_paths()
    //   2. Android 额外加 /system/etc/security/cacerts（目录形式）
    //   3. 两者都没拿到 CA 时不让 Create() 失败——"绝不静默地不校验"由
    //      SSL_VERIFY_PEER 保证（空 store 下握手必然失败），这里再加硬失败
    //      守卫是多余的且有假阴性风险（X509_STORE 在部分平台惰性加载）。
    //
    // 只有畸形的 caCertsPem 会让本函数失败 —— 那是调用方明确传错了东西。
    static SSL_CTX* Create(const TlsConfig& cfg, std::string& outErr);

    // 每条连接调用一次：设 SNI + hostname 校验期望值。
    // host 是 IP 字面量（v4 或 v6）时不设 SNI —— RFC 6066 禁止。
    static BCRESULT PrepareSsl(SSL* ssl,
                               const std::string& host,
                               const TlsConfig& cfg,
                               std::string& outErr);

    // 校验证书链。chain[0] 是 leaf。
    // spkiPin 非空时只比对 pin，不做链校验（与 SMPConnector 现有语义一致）。
    //
    // 所有权约定：chain 由调用方拥有，本函数只借用，不释放 chain 里的任何
    // 元素（也不会释放 chain[0]）。调用方在拿到返回值之后自己决定何时
    // X509_free。这是刻意的——早前的实现内部用 sk_X509_pop_free(...,
    // X509_free) 清理临时的 intermediates 容器，误把借来的证书也真的释放
    // 掉了，导致调用方后续再释放一次时发生双重释放（服务端证书链只要带一张
    // 中间证书就必崩，见 TlsContext_test.cpp 的
    // test_verify_chain_does_not_free_borrowed_certs）。
    static BCRESULT VerifyChain(const std::vector<X509*>& chain,
                                const std::string& host,
                                const TlsConfig& cfg,
                                std::string& outErr);

    // 从 SMPConnector.cpp 移过来的 helper，SMPConnector 改为引用这里。
    static std::string ComputeSpkiPinBase64(X509* leaf);
    static std::string CanonicalizeSpkiPin(const char* pin, size_t len);
    static int         AddTrustedCAsToStore(X509_STORE* store,
                                            const std::vector<X509*>& certs,
                                            void* loggerCtx);
    // 把 PEM 文本解析成一组 X509*。调用方负责 X509_free。
    static int         ParseCAsFromPem(const std::string& pem,
                                       std::vector<X509*>& out,
                                       std::string& outErr);
};

#endif // TT_TLS_CONTEXT_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
