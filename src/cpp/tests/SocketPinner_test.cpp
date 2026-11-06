


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


static void test_zero_ifindex_is_not_needed()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;              
    req.ifIndex = 0;
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    char method[32] = "unset";
    int  err        = -1;
    CHECK(tt_socket_pin(&req, method, sizeof(method), &err) == TT_PIN_NOT_NEEDED);
    CHECK(strcmp(method, "") == 0);   
    CHECK(err == 0);
}


static void test_null_request_is_rejected()
{
    CHECK(tt_socket_pin(NULL, NULL, 0, NULL) == TT_PIN_FAILED);
}


static void test_bad_fd_fails()
{
    TTPinRequest req;
    memset(&req, 0, sizeof(req));
    req.fd      = -1;
    req.ifIndex = 1;               
    req.policy  = TT_VPN_POLICY_PREFER_PHYSICAL;

    CHECK(tt_socket_pin(&req, NULL, 0, NULL) == TT_PIN_FAILED);
}


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

    
    
    TTPinResult r = tt_socket_pin(&req, NULL, 0, NULL);
    CHECK(r == TT_PIN_OK || r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);

    close(fd);
}


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


static void test_unpin_bad_fd()
{
    TTPinResult r = tt_socket_unpin(-1, 0, NULL, NULL, 0, NULL);
    CHECK(r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);
}



static void test_unpin_out_params_are_optional()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;

    char method[32] = "";
    int  err        = -1;
    TTPinResult r = tt_socket_unpin(fd, 0, NULL, method, sizeof(method), &err);
    CHECK(r == TT_PIN_OK || r == TT_PIN_FAILED || r == TT_PIN_UNSUPPORTED);

    
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
