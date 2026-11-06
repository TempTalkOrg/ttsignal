














#ifndef TT_LINUX_PHYSICAL_IFACE_H
#define TT_LINUX_PHYSICAL_IFACE_H

#ifdef __cplusplus
extern "C" {
#endif



int tt_linux_iface_is_physical(const char* ifName);










int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot);

#ifdef __cplusplus
}
#endif

#endif 




