


















#ifndef TT_TLS_CONTEXT_H
#define TT_TLS_CONTEXT_H

#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <BC/Config.h>

struct TlsConfig
{
    std::string caCertsPem;                 
    std::string spkiPin;                    
    std::string clientCertFile;             
    std::string clientKeyFile;
    std::string clientKeyPassword;
    bool        insecureSkipVerify = false; 
    void*       loggerCtx          = nullptr;
};

class TlsContext
{
public:
    
    
    
    
    
    
    
    
    
    
    
    
    
    static SSL_CTX* Create(const TlsConfig& cfg, std::string& outErr);

    
    
    static BCRESULT PrepareSsl(SSL* ssl,
                               const std::string& host,
                               const TlsConfig& cfg,
                               std::string& outErr);

    
    
    
    
    
    
    
    
    
    
    static BCRESULT VerifyChain(const std::vector<X509*>& chain,
                                const std::string& host,
                                const TlsConfig& cfg,
                                std::string& outErr);

    
    static std::string ComputeSpkiPinBase64(X509* leaf);
    static std::string CanonicalizeSpkiPin(const char* pin, size_t len);
    static int         AddTrustedCAsToStore(X509_STORE* store,
                                            const std::vector<X509*>& certs,
                                            void* loggerCtx);
    
    static int         ParseCAsFromPem(const std::string& pem,
                                       std::vector<X509*>& out,
                                       std::string& outErr);
};

#endif 




