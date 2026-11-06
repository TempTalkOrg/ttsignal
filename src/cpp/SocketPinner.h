
















#ifndef TT_SOCKET_PINNER_H
#define TT_SOCKET_PINNER_H

#include <stdint.h>
#include <stddef.h>

#include "VpnPolicy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TT_PIN_OK          = 0,  
    TT_PIN_NOT_NEEDED  = 1,  
    TT_PIN_UNSUPPORTED = 2,  
    TT_PIN_FAILED      = 3,  
} TTPinResult;

typedef struct TTPinRequest {
    int         fd;                
    uint32_t    ifIndex;           
    uint64_t    androidNetHandle;  
    int         ipv6;              
    int         isTcp;             
    TTVpnPolicy policy;            
    void*       loggerCtx;         
} TTPinRequest;













TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno);













TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx,
                            char* outMethod, size_t outMethodLen,
                            int* outErrno);

#ifdef __cplusplus
}
#endif

#endif 




