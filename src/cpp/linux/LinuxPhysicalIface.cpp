



#include "LinuxPhysicalIface.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char* const kDefaultSysClassNet = "/sys/class/net";



bool NameLooksVirtual(const char* name)
{
    static const char* const kBlacklist[] = {
        "lo",        
        "docker",    
        "br-",       
        "veth",      
        "virbr",     
        "vmnet",     
        "tailscale", 
        "tun",       
        "tap",       
        "zt",        
        "kube-",     
        "cni",       
        "flannel",   
        NULL,
    };
    for (int i = 0; kBlacklist[i]; i++) {
        if (strncmp(name, kBlacklist[i], strlen(kBlacklist[i])) == 0) {
            return true;
        }
    }
    return false;
}



bool NameIsSane(const char* name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '.') return false;
    if (strchr(name, '/') != NULL) return false;
    return true;
}

bool PathExists(const char* path)
{
    struct stat st;
    
    return stat(path, &st) == 0;
}

} 

extern "C" {

int tt_linux_iface_is_physical_at(const char* ifName,
                                  const char* sysClassNetRoot)
{
    if (!NameIsSane(ifName)) {
        return 0;
    }
    const char* root = (sysClassNetRoot && sysClassNetRoot[0])
                     ? sysClassNetRoot
                     : kDefaultSysClassNet;

    
    if (!PathExists(root)) {
        return NameLooksVirtual(ifName) ? 0 : 1;
    }

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/device", root, ifName);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        
        return 0;
    }
    if (PathExists(path)) {
        return 1;
    }

    
    n = snprintf(path, sizeof(path), "%s/%s", root, ifName);
    if (n > 0 && (size_t)n < sizeof(path) && PathExists(path)) {
        return 0;
    }

    
    
    return NameLooksVirtual(ifName) ? 0 : 1;
}

int tt_linux_iface_is_physical(const char* ifName)
{
    return tt_linux_iface_is_physical_at(ifName, NULL);
}

} 




