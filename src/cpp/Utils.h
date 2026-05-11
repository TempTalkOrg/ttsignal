///////////////////////////////////////////////////////////////////////////////
// file : Utils.h
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#ifndef UTILS_H_INCLUDED__
#define UTILS_H_INCLUDED__

#include <vector>
#include <string>
#include <xquic/xquic.h>
#include <BC/BCStrPtrLen.h>
#include <BC/BCFCodec.h>
#include <BC/BCJson.h>
#include <BC/BCPString.h>

namespace BC
{
	class BCBuffer;
}

using namespace BC;

extern "C" {
extern xqc_usec_t xqc_now(); // export from jquic
}

#define LOG_LEVEL_DEBUG    0x01
#define LOG_LEVEL_INFO     0x02
#define LOG_LEVEL_WARN     0x03
#define LOG_LEVEL_ERROR    0x04
#define LOG_LEVEL_FATAL    0x05


inline bool operator==(const xqc_cid_t& cid1, const xqc_cid_t& cid2) {
    return (cid1.cid_len == cid2.cid_len && 0 == memcmp(cid1.cid_buf, cid2.cid_buf, cid1.cid_len));
}

namespace std
{
    template<>
    struct hash<xqc_cid_t> {
        size_t operator()(const xqc_cid_t& s) const noexcept
        {
            uint64_t hash = 0;
            const uint8_t* cid_buf = s.cid_buf;
            uint8_t cid_len = s.cid_len;
            while (cid_len) {
                if (cid_len >= 8) {
                    hash ^= *(const uint64_t*)cid_buf;
                    cid_buf += 8;
                    cid_len -= 8;

                }
                else if (cid_len >= 4) {
                    hash ^= *(const uint32_t*)cid_buf;
                    cid_buf += 4;
                    cid_len -= 4;

                }
                else if (cid_len >= 2) {
                    hash ^= *(const uint16_t*)cid_buf;
                    cid_buf += 2;
                    cid_len -= 2;

                }
                else {
                    hash ^= *(const uint8_t*)cid_buf;
                    cid_buf += 1;
                    cid_len -= 1;
                }
            }

            return hash;
        }
    };
}


namespace EngCallbacks {

    void* lock_create();

    void lock_acquire(void* lock);

    void lock_release(void* lock);

    void lock_destory(void* lock);

    void print_stack(const char* line, void* engine_user_data);

}

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{


///////////////////////////////////////////////////////////////////////////////
// global constant variables
///////////////////////////////////////////////////////////////////////////////

extern BCStrPtrLen kConnect;
extern BCStrPtrLen kConnected;
extern BCStrPtrLen kInstCount;
extern BCStrPtrLen kENOMEM;
extern BCStrPtrLen kEINITSTUB;
extern BCStrPtrLen kECONNECT;
extern BCStrPtrLen kENOTCONNECTED;
extern BCStrPtrLen kECLOSED;
extern BCStrPtrLen kESTATE;
extern BCStrPtrLen kECONNECTING;
extern BCStrPtrLen kECONNECTED;
extern BCStrPtrLen kEINTERNAL;
extern BCStrPtrLen kEREJECTED;
extern BCStrPtrLen kEMAXCONN;
extern BCStrPtrLen kEHANDSHAKE;
extern BCStrPtrLen kENOTFOUND;
extern BCStrPtrLen kENOQUOTA;
extern BCStrPtrLen kEEXISTS;
extern BCStrPtrLen kClosed;
extern BCStrPtrLen kPaused;
extern BCStrPtrLen kQuery;
extern BCStrPtrLen kQScheme;
extern BCStrPtrLen kHttps;
extern BCStrPtrLen kHost;
extern BCStrPtrLen kQPath;
extern BCStrPtrLen kProps;

BCRESULT 
TextToSockaddr(
    int type, const char *addr_text,
    unsigned int port,
    struct sockaddr *saddr,
    socklen_t *saddr_len);

bool 
ParseJsonFromBuffer(BCBuffer& buffer, Json::Value& outValue);

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////

void						
SplitString(const ::std::string& str,
			char delimiter,
			::std::vector< ::std::string>* dest);

void						
FormatLogTime(char* buf, size_t buf_len);

uint32_t LogLevelToBCLogLevel(uint32_t level);

Json::Value StatFixedAlloc(uint32_t nFilter);

std::string StatFixedAllocToString(uint32_t nFilter);

int32_t XQCLogLevelToBCLogLevel(int32_t level);

int32_t BCLogLevelToLogLevel(int32_t level);

int32_t LogLevelToBCLogLevel(int32_t level);

void
LogQ(const void *logger_ctx, int32_t nLevel, const char* szFmtStr, ...);

std::string EncodeURI(const std::string& uri_string);

std::string DecodeURI(const std::string& encoded_string);

const char* GetSDKVersion();

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

#endif // UTILS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : Utils.h
///////////////////////////////////////////////////////////////////////////////
