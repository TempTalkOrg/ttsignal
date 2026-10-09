///////////////////////////////////////////////////////////////////////////////
// file : SSLayer.cpp
// author : zhoukai88@jd.com
///////////////////////////////////////////////////////////////////////////////
#include <openssl/base.h>
#include "SSLayer.h"



///////////////////////////////////////////////////////////////////////////////
// Options : 
///////////////////////////////////////////////////////////////////////////////


#if defined(__cplusplus)
extern "C" {
#endif

// BIO_clear_socket_error clears the last system socket error.
//
// TODO(fork): remove all callers of this.
void bio_clear_socket_error(void);

// BIO_sock_error returns the last socket error on |sock|.
int bio_sock_error(int sock);

// BIO_fd_should_retry returns non-zero if |return_value| indicates an error
// and |errno| indicates that it's non-fatal.
int bio_fd_should_retry(int return_value);


#if defined(__cplusplus)
}  // extern C
#endif



static int my_bio_should_retry(int i) {
    switch (i) {
    case SSL_EWOULDBLOCK:
        return 1;
    default:
        return 0;
    }
    return 0;
}


///////////////////////////////////////////////////////////////////////////////
// class : SSLayer
///////////////////////////////////////////////////////////////////////////////
SSLayer::SSLayer(bool accept)
	: accept_(accept)
    , handler_(NULL)
    , ssl_(NULL)
{
}

SSLayer::~SSLayer()
{
	//
}

BIO* SSLayer::NewBIO(SSLayer *sslayer) {
    BIO* ret;

    ret = BIO_new(&methods_sockp);
    if (ret == NULL) {
        return NULL;
    }
    BIO_set_fd(ret, 1, BIO_NOCLOSE);
    BIO_set_data(ret, sslayer);
    return ret;
}

BCRESULT SSLayer::Create(SSL *ssl, ISSLayerHandler *pHandler)
{
    BIO* bio = NULL;

    if (!ssl || !pHandler)
    {
        return BC_R_INVALIDARG;
    }
    bio = NewBIO(this);
    ssl_ = ssl;
    handler_ = pHandler;
    SSL_set_bio(ssl, bio, bio);
    ssl_state_ = SSL_STATE_ACCEPTING;
    buffer_.reset(new BCBuffer);
    data_.reset(new BCBuffer);

	return BC_R_SUCCESS;
}

void SSLayer::AcceptSSL()
{
    if (ssl_ && ssl_state_ == SSL_STATE_ACCEPTING && buffer_)
    {
        int ret = SSL_accept(ssl_);
        if (ret == 1)
        {
            ssl_state_ = SSL_STATE_RW;
            SSL_clear_mode(ssl_, SSL_MODE_AUTO_RETRY);
            if (handler_)
            {
                handler_->OnSSLReady();
            }
        }
        else if (ret < 0)
        {
            const char* file = NULL, * data = NULL;
            int line = 0, flags = 0;
            uint32_t ret = ERR_get_error_line_data(&file, &line, &data, &flags);
            if (handler_ && ret > 0 && data)
            {
                handler_->OnSSLError(file, line, data, flags);
            }
        }
    }
}

void SSLayer::ConnectSSL()
{
    if (ssl_ && ssl_state_ == SSL_STATE_CONNECTING && buffer_)
    {
        int ret = SSL_connect(ssl_);
        if (ret == 1)
        {
            ssl_state_ = SSL_STATE_RW;
            SSL_clear_mode(ssl_, SSL_MODE_AUTO_RETRY);
            if (handler_)
            {
                handler_->OnSSLReady();
            }
        }
        else if (ret < 0)
        {
            const char* file = NULL, * data = NULL;
            int line = 0, flags = 0;
            uint32_t ret = ERR_get_error_line_data(&file, &line, &data, &flags);
            if (handler_ && ret > 0 && data)
            {
                handler_->OnSSLError(file, line, data, flags);
            }
        }
    }
}

void SSLayer::ReadFromSSL(const void *data, size_t size)
{
    if (!buffer_ || buffer_->RemainingLength() == 0)
    {
        buffer_.reset(new BCBuffer);
    }
    buffer_->Write(data, size);
    if (accept_)
    {
        AcceptSSL();
    }
    else
    {
        ConnectSSL();
    }
    if (ssl_state_ == SSL_STATE_RW)
    {
        uint32_t nReadSize;
        int nSize;
        void* buf;
        bool finished = false;

        while (buffer_ && buffer_->RemainingLength() > 0)
        {
            nReadSize = INFINITE;
            buf = data_->GetWritableBlock(nReadSize);
            nSize = SSL_read(ssl_, buf, nReadSize);
            if (nSize > 0)
            {
                data_->UngetWritableBlock(nSize);
            }
            else if (nSize < 0)
            {
                const char* file = NULL, * data = NULL;
                int line = 0, flags = 0;
                uint32_t ret = ERR_get_error_line_data(&file, &line, &data, &flags);
                if (handler_ && ret > 0 && data)
                {
                    handler_->OnSSLError(file, line, data, flags);
                }
            }
            else
            {
                finished = true;
                break;
            }
        }
        if (data_ && data_->RemainingLength() > 0 && handler_)
        {
            void* lpData;
            uint32_t nReadSize = 0;
            while ((lpData = data_->ReadBlock(INFINITE, nReadSize)) && nReadSize > 0)
            {
                handler_->OnRecvDataFromSSL(lpData, nReadSize);
            }
            data_->Reset(0);
        }
        if (finished && handler_)
        {
            handler_->OnSSLFinished();
        }
    }
}

BCRESULT SSLayer::WriteToSSL(std::shared_ptr<BCBuffer> buffer)
{
    if (ssl_)
    {
        uint32_t nReadSize = INFINITE;
        void* data;
        while ((data = buffer->ReadBlock(INFINITE, nReadSize)) && nReadSize > 0)
        {
            // Do SSL write
            SSL_write(ssl_, data, nReadSize);
        }
        return BC_R_SUCCESS;
    }
    return BC_R_FAILURE;
}

void SSLayer::Close()
{
    if (ssl_)
    {
        SSL_shutdown(ssl_);
    }
}

int SSLayer::buffer_read(void* buffer, int size)
{
    size_t nread;

    if (buffer_ && buffer_->RemainingLength() > 0)
    {
        BCBIStream sReader(buffer_.get());
        nread = sReader.Read(buffer, size);
        if (sReader.RemainingLength() == 0)
        {
            buffer_.reset();
        }
        if (nread > 0)
        {
            return nread;
        }
    }
    return SSL_EWOULDBLOCK;
}

int SSLayer::sock_new(BIO* bio) {
    bio->init = 0;
    bio->num = 0;
    bio->ptr = NULL;
    bio->flags = 0;
    return 1;
}

int SSLayer::sock_free(BIO* bio) {
    if (bio == NULL) {
        return 0;
    }

    bio->init = 0;
    bio->flags = 0;
    bio->shutdown = 0;
    bio->num = 0;
    return 1;
}

int SSLayer::sock_read(BIO* b, char* out, int outl) {
    int ret = 0;
    SSLayer* _this = (SSLayer *)BIO_get_data(b);
    if (!_this || !_this->handler_ || out == NULL)
    {
        return 0;
    }

    bio_clear_socket_error();
    ret = _this->buffer_read(out, outl);
    BIO_clear_retry_flags(b);
    if (ret <= 0) {
        if (my_bio_should_retry(ret)) {
            BIO_set_retry_read(b);
        }
    }
    return ret;
}

int SSLayer::sock_write(BIO* b, const char* in, int inl) {
    int ret;
    SSLayer* _this = (SSLayer*)BIO_get_data(b);
    if (!_this || !_this->handler_ || in == NULL)
    {
        return 0;
    }

    bio_clear_socket_error();
    ret = _this->handler_->OnSSLWrite(in, inl);
    BIO_clear_retry_flags(b);
    if (ret <= 0) {
        if (my_bio_should_retry(ret)) {
            BIO_set_retry_write(b);
        }
    }
    return ret;
}

long SSLayer::sock_ctrl(BIO* b, int cmd, long num, void* ptr) {
    long ret = 1;
    int* ip;

    switch (cmd) {
    case BIO_C_SET_FD:
        sock_free(b);
        b->num = *((int*)ptr);
        b->shutdown = (int)num;
        b->init = 1;
        break;
    case BIO_C_GET_FD:
        if (b->init) {
            ip = (int*)ptr;
            if (ip != NULL) {
                *ip = b->num;
            }
            ret = b->num;
        }
        else {
            ret = -1;
        }
        break;
    case BIO_CTRL_GET_CLOSE:
        ret = b->shutdown;
        break;
    case BIO_CTRL_SET_CLOSE:
        b->shutdown = (int)num;
        break;
    case BIO_CTRL_FLUSH:
        ret = 1;
        break;
    default:
        ret = 0;
        break;
    }
    return ret;
}

const BIO_METHOD SSLayer::methods_sockp = {
    BIO_TYPE_SOCKET, "socket",
    sock_write,      sock_read,
    NULL /* puts */, NULL /* gets, */,
    sock_ctrl,       sock_new,
    sock_free,       NULL /* callback_ctrl */,
};

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
