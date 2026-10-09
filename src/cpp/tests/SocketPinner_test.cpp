///////////////////////////////////////////////////////////////////////////////
// file : SocketPinner_test.cpp
//
// Standalone unit test for the socket-pinning helpers. Not part of any build
// target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/jquic/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/Utils.cpp \
//       src/cpp/tests/SocketPinner_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       -o /tmp/socketpinner_test && /tmp/socketpinner_test
//
// SocketPinner.cpp 通过 StdAfx.h/Utils.h 调用真实的 LogQ（不是桩实现），
// 所以除了 VpnPolicy.cpp 之外还要把 Utils.cpp 一起编译，并链接 env 库
// （LogQ 内部依赖 BCPString::Format / LogCustomV 等符号，只编译 Utils.cpp
// 不链接 libenv.a 会在链接期报符号缺失）。-I deps/jquic/include 是因为
// Utils.h 传递性 include 了 <xquic/xquic.h>。非 Darwin/arm64 宿主换成对应
// 平台下的 deps/env/lib/<平台>/<架构>/Debug/libenv.a 即可。
///////////////////////////////////////////////////////////////////////////////
#include "SocketPinner.h"

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// ifIndex 为 0 表示调用方没有要求绑定，必须原样返回 NOT_NEEDED 且不碰 fd。
static void test_zero_ifindex_is_not_needed()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;              // 故意给非法 fd：不该被使用
    req.ifIndex = 0;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    char method[32] = "unset";
    int  err        = -1;
    CHECK(tt_socket_pin(&req, method, sizeof(method), &err) == TT_PIN_NOT_NEEDED);
    CHECK(strcmp(method, "") == 0);   // 未绑定时 method 置空串
    CHECK(err == 0);
}

// 传 NULL 必须安全返回，不能崩。
static void test_null_request_is_rejected()
{
    CHECK(tt_socket_pin(NULL, NULL, 0, NULL) == TT_PIN_FAILED);
}

// 非法 fd 且 ifIndex 非 0 时必须返回 FAILED，不能崩，也不能报成功。
static void test_bad_fd_fails()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;
    req.ifIndex = 1;               // loopback，任何平台都存在
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    CHECK(tt_socket_pin(&req, NULL, 0, NULL) == TT_PIN_FAILED);
}

// outMethod / outErrno 允许传 NULL，不能因此崩溃。
static void test_null_out_params_are_optional()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = fd;
    req.ifIndex = 1;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    // 只要不崩、返回值属于合法枚举即可。绑 loopback 在不同平台结果不同，
    // 所以这里不断言具体成败。
    TTPinResult r = tt_socket_pin(&req, NULL, 0, NULL);
    CHECK(r == TT_PIN_OK || r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);

    close(fd);
}

// 成功绑定时必须填出实际生效的手段名，供日志与诊断使用。
static void test_method_name_is_reported_on_success()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = fd;
    req.ifIndex = 1;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    char method[32] = "";
    TTPinResult r = tt_socket_pin(&req, method, sizeof(method), NULL);
    if (r == TT_PIN_OK) {
        CHECK(strlen(method) > 0);
    }

    close(fd);
}

// unpin 对非法 fd 必须安全返回，outMethod/outErrno 允许传 NULL。
static void test_unpin_bad_fd()
{
    TTPinResult r = tt_socket_unpin(-1, 0, NULL, NULL, 0, NULL);
    CHECK(r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);
}

// unpin 成功时（合法 fd）必须能安全接收 outMethod/outErrno，不崩溃；
// 具体是否清除因平台而异，这里只断言接口契约本身不出错。
static void test_unpin_out_params_are_optional()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    char method[32] = "";
    int  err        = -1;
    TTPinResult r = tt_socket_unpin(fd, 0, NULL, method, sizeof(method), &err);
    CHECK(r == TT_PIN_OK || r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);

    // NULL 出参同样不能崩。
    TTPinResult r2 = tt_socket_unpin(fd, 0, NULL, NULL, 0, NULL);
    CHECK(r2 == TT_PIN_OK || r2 == TT_PIN_FAILED || r2 == TT_PIN_UNSUPPORTED);

    close(fd);
}

int main()
{
    test_zero_ifindex_is_not_needed();
    test_null_request_is_rejected();
    test_bad_fd_fails();
    test_null_out_params_are_optional();
    test_method_name_is_reported_on_success();
    test_unpin_bad_fd();
    test_unpin_out_params_are_optional();

    if (g_failures == 0) {
        printf("SocketPinner_test: ALL PASS\n");
    } else {
        printf("SocketPinner_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
