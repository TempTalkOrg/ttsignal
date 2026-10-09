///////////////////////////////////////////////////////////////////////////////
// file : SSLayer.h
// author : zhoukai88@jd.com
///////////////////////////////////////////////////////////////////////////////
  
#ifndef SSLAYER_H_INCLUDED__
#define SSLAYER_H_INCLUDED__

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <BC/BCFCodec.h>

///////////////////////////////////////////////////////////////////////////////
// Options : 
///////////////////////////////////////////////////////////////////////////////

#define SSL_EWOULDBLOCK  -1

///////////////////////////////////////////////////////////////////////////////
// Class : ISSLayerHandler
///////////////////////////////////////////////////////////////////////////////

class ISSLayerHandler
{
public:
	ISSLayerHandler() {}
	virtual ~ISSLayerHandler() {}

	virtual int	    OnSSLWrite(const void *data, size_t len)        = 0;
    virtual void    OnRecvDataFromSSL(const void* data, size_t size)= 0;
    virtual void    OnSSLReady()                                    = 0;
    virtual void    OnSSLFinished()                                 = 0;
    virtual void    OnSSLError(
                        const char* file, 
                        int line, 
                        const char* data, 
                        int flags)                                  = 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : SSLayer
///////////////////////////////////////////////////////////////////////////////
class SSLayer
{
    typedef enum {
        SSL_STATE_INIT          = 0,
        SSL_STATE_ACCEPTING     = 1,
        SSL_STATE_CONNECTING    = 1,
        SSL_STATE_RW            = 2,
    }SSLState;
public:
	SSLayer(bool accept = true);
	~SSLayer();

    static BIO      *   NewBIO(SSLayer* sslayer);
	BCRESULT			Create(SSL * ssl, ISSLayerHandler *pHandler);
    void                AcceptSSL();
    void                ConnectSSL();
    void                ReadFromSSL(const void* data, size_t size);
    BCRESULT            WriteToSSL(std::shared_ptr<BCBuffer> data);
    void                Close();
protected:
    int                 buffer_read(void* buffer, int size);

    static int          sock_new(BIO* bio);
    static int          sock_free(BIO* bio);
    static int          sock_read(BIO* b, char* out, int outl);
    static int          sock_write(BIO* b, const char* in, int inl);
    static long         sock_ctrl(BIO* b, int cmd, long num, void* ptr);
    static const BIO_METHOD methods_sockp;
private:
	DECLARE_NO_COPY_CLASS(SSLayer);
    bool                        accept_ = true;
	ISSLayerHandler		    *	handler_ = NULL;
    SSL                     *   ssl_ = NULL;
    SSLState                    ssl_state_ = SSL_STATE_INIT;
    std::shared_ptr<BCBuffer>   buffer_;
    std::shared_ptr<BCBuffer>   data_;

};

#endif // SSLAYER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : SSLayer.h
///////////////////////////////////////////////////////////////////////////////
