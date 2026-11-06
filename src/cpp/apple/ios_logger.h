











#ifndef TTSIGNAL_IOS_LOGGER_H_INCLUDED__
#define TTSIGNAL_IOS_LOGGER_H_INCLUDED__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif














typedef void (*TTLogCallback)(void* userdata, int32_t level, const char* msg);



void tt_logger_set_callback(TTLogCallback cb, void* userdata);




void ios_logger_dispatch(int32_t level, const char* msg);

#ifdef __cplusplus
} 
#endif

#endif 
