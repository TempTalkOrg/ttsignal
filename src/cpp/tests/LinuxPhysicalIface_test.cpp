


#include "linux/LinuxPhysicalIface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)








static std::string MakeFakeSysfs()
{
    char tmpl[] = "/tmp/ttsig_sysfs_XXXXXX";
    const char* root = mkdtemp(tmpl);
    if (!root) {
        printf("FAIL: mkdtemp\n");
        exit(1);
    }
    std::string base(root);
    const char* withDevice[]    = {"eth0", "wlan0", "eth-tun", NULL};
    const char* withoutDevice[] = {"tun0", "docker0", "myvpn", NULL};

    for (int i = 0; withDevice[i]; i++) {
        std::string d = base + "/" + withDevice[i];
        mkdir(d.c_str(), 0755);
        mkdir((d + "/device").c_str(), 0755);
    }
    for (int i = 0; withoutDevice[i]; i++) {
        mkdir((base + "/" + withoutDevice[i]).c_str(), 0755);
    }
    return base;
}

static void test_sysfs_detection()
{
    std::string root = MakeFakeSysfs();

    CHECK(tt_linux_iface_is_physical_at("eth0",    root.c_str()) == 1);
    CHECK(tt_linux_iface_is_physical_at("wlan0",   root.c_str()) == 1);
    
    CHECK(tt_linux_iface_is_physical_at("eth-tun", root.c_str()) == 1);

    CHECK(tt_linux_iface_is_physical_at("tun0",    root.c_str()) == 0);
    CHECK(tt_linux_iface_is_physical_at("docker0", root.c_str()) == 0);
    
    CHECK(tt_linux_iface_is_physical_at("myvpn",   root.c_str()) == 0);
}


static void test_name_blacklist_fallback()
{
    const char* kMissingRoot = "/tmp/ttsig_sysfs_does_not_exist_12345";

    CHECK(tt_linux_iface_is_physical_at("eth0",      kMissingRoot) == 1);
    CHECK(tt_linux_iface_is_physical_at("wlan0",     kMissingRoot) == 1);
    CHECK(tt_linux_iface_is_physical_at("enp3s0",    kMissingRoot) == 1);

    CHECK(tt_linux_iface_is_physical_at("lo",        kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tun0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tap0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("tailscale0",kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("docker0",   kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("br-abc123", kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("veth1234",  kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("virbr0",    kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("vmnet1",    kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("zt0",       kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("kube-br0",  kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("cni0",      kMissingRoot) == 0);
    CHECK(tt_linux_iface_is_physical_at("flannel.1", kMissingRoot) == 0);
}

static void test_bad_input()
{
    CHECK(tt_linux_iface_is_physical_at(NULL, "/tmp") == 0);
    CHECK(tt_linux_iface_is_physical_at("",   "/tmp") == 0);
    
    CHECK(tt_linux_iface_is_physical_at("../etc", "/tmp") == 0);
    CHECK(tt_linux_iface_is_physical_at("a/b",    "/tmp") == 0);
    
    (void)tt_linux_iface_is_physical_at("eth0", NULL);
}

int main()
{
    test_sysfs_detection();
    test_name_blacklist_fallback();
    test_bad_input();
    if (g_failures == 0) {
        printf("LinuxPhysicalIface_test: ALL PASS\n");
        return 0;
    }
    printf("LinuxPhysicalIface_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
