///////////////////////////////////////////////////////////////////////////////
// file : LinuxPhysicalIface_test.cpp
//
// Standalone unit test for the Linux physical-interface detector. The code
// under test is plain POSIX, so this compiles and runs on macOS too:
//
//   c++ -std=c++17 -I src/cpp src/cpp/linux/LinuxPhysicalIface.cpp \
//       src/cpp/tests/LinuxPhysicalIface_test.cpp -o /tmp/linuxphy_test \
//       && /tmp/linuxphy_test
//
///////////////////////////////////////////////////////////////////////////////
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

// 造一棵假的 /sys/class/net 目录树：
//   eth0      有 device/  -> 物理
//   wlan0     有 device/  -> 物理
//   tun0      无 device/  -> 虚拟
//   docker0   无 device/  -> 虚拟
//   myvpn     无 device/  -> 虚拟（名字不在黑名单里，验证 sysfs 判定确实生效）
//   eth-tun   有 device/  -> 物理（名字含 tun，验证 sysfs 优先于名字黑名单）
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
    // sysfs 是主依据：名字里带 tun 但有真实硬件，判为物理
    CHECK(tt_linux_iface_is_physical_at("eth-tun", root.c_str()) == 1);

    CHECK(tt_linux_iface_is_physical_at("tun0",    root.c_str()) == 0);
    CHECK(tt_linux_iface_is_physical_at("docker0", root.c_str()) == 0);
    // 名字不在任何黑名单里，但没有 device/，仍判为虚拟
    CHECK(tt_linux_iface_is_physical_at("myvpn",   root.c_str()) == 0);
}

// sysfs 整体不可读（容器 / 挂载受限）时回落到名字黑名单
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
    // 路径穿越 / 分隔符不该被当成合法网卡名
    CHECK(tt_linux_iface_is_physical_at("../etc", "/tmp") == 0);
    CHECK(tt_linux_iface_is_physical_at("a/b",    "/tmp") == 0);
    // root 传 NULL 时用默认 /sys/class/net，不能崩
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
