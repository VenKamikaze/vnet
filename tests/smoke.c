/* SPDX-License-Identifier: MIT */
/*
 * Dependency-free smoke test (plain C, no GoogleTest). It exists so that
 * platforms without GoogleTest - notably the MinGW cross build, run under
 * Wine - can still exercise the real Winsock code paths: UDP send/receive on
 * 127.0.0.1, EAGAIN, oversize rejection and impairment. The full suite is in
 * the GoogleTest binary.
 */
#define _POSIX_C_SOURCE 200809L
#include "vnet.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(void) { Sleep(1); }
#else
#include <time.h>
static void sleep_ms(void) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
}
#endif

static int failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Wait for a datagram with vnet_wait(): up to 1000 rounds of at most 5 ms (5 s). */
static int recv_wait(vnet *v, int s, void *buf, size_t cap, vnet_addr *from) {
    for (int i = 0; i < 1000; i++) {
        vnet_poll(v);
        int n = vnet_recvfrom(v, s, buf, cap, from);
        if (n != VNET_EAGAIN) return n;
        if (vnet_wait(v, 5) == VNET_ENOTSUP) sleep_ms();
    }
    return VNET_EAGAIN;
}

static void test_udp(void) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.ignore_env = 1;
    int err = 0;
    vnet *a = vnet_create(&cfg, &err);
    vnet *b = vnet_create(&cfg, &err);
    CHECK(a && b);
    if (!a || !b) return;
    int sa = vnet_open(a, 0), sb = vnet_open(b, 0);
    CHECK(sa >= 0 && sb >= 0);

    char buf[VNET_MAX_DATAGRAM + 8];
    vnet_addr from, lb, to;
    CHECK(vnet_recvfrom(b, sb, buf, sizeof buf, &from) == VNET_EAGAIN);

    CHECK(vnet_local_addr(b, sb, &lb) == 0);
    CHECK(vnet_resolve(b, "127.0.0.1:1", &to) == 0);
    to.port = lb.port;

    CHECK(vnet_sendto(a, sa, "hello", 5, &to) == 5);
    int n = recv_wait(b, sb, buf, sizeof buf, &from);
    CHECK(n == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(from.family == VNET_AF_IPV4 && from.bytes[0] == 127);

    /* oversize: rejected, and not sent (the marker must be first) */
    static char big[VNET_MAX_DATAGRAM + 1];
    memset(big, 'x', sizeof big);
    CHECK(vnet_sendto(a, sa, big, sizeof big, &to) == VNET_EMSGSIZE);
    CHECK(vnet_sendto(a, sa, big, VNET_MAX_DATAGRAM, &to) == VNET_MAX_DATAGRAM);
    n = recv_wait(b, sb, buf, sizeof buf, &from);
    CHECK(n == VNET_MAX_DATAGRAM);

    CHECK(vnet_open(b, lb.port) == VNET_EADDRINUSE);

    /* vnet_wait: a timeout is 0, a waiting datagram is 1 */
    CHECK(vnet_wait(b, 0) == 0);
    CHECK(vnet_wait(b, 10) == 0);
    CHECK(vnet_wait(b, -1) == VNET_EINVAL);
    CHECK(vnet_sendto(a, sa, "w", 1, &to) == 1);
    CHECK(vnet_wait(b, 5000) == 1);
    CHECK(recv_wait(b, sb, buf, sizeof buf, &from) == 1);

    /* vnet_set_impair on a live UDP context: total loss, then recovery */
    {
        vnet_impair out;
        memset(&out, 0, sizeof out);
        out.loss_pct = 100;
        CHECK(vnet_set_impair(a, &out) == 0);
        CHECK(vnet_sendto(a, sa, "lost", 4, &to) == 4);
        CHECK(vnet_wait(b, 50) == 0);
        CHECK(vnet_set_impair(a, NULL) == 0);
        CHECK(vnet_sendto(a, sa, "back", 4, &to) == 4);
        CHECK(recv_wait(b, sb, buf, sizeof buf, &from) == 4 && memcmp(buf, "back", 4) == 0);
    }
    vnet_destroy(a);
    vnet_destroy(b);
}

static void test_loopback_impair(void) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.ignore_env = 1;
    cfg.impair.latency_ms = 50;
    vnet *a = vnet_create(&cfg, NULL);
    cfg.impair.latency_ms = 0;
    vnet *b = vnet_create(&cfg, NULL);
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    vnet_addr to;
    CHECK(vnet_resolve(a, "127.1.0.2:2", &to) == 0);
    char buf[16];
    CHECK(vnet_sendto(a, sa, "late", 4, &to) == 4);
    vnet_loopback_network_step(net, 49);
    CHECK(vnet_recvfrom(b, sb, buf, sizeof buf, NULL) == VNET_EAGAIN);
    vnet_loopback_network_step(net, 1);
    CHECK(vnet_recvfrom(b, sb, buf, sizeof buf, NULL) == 4);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

static void test_version_and_resolve(void) {
    char want[32];
    vnet_addr a;
    snprintf(want, sizeof want, "%d.%d.%d", VNET_VERSION_MAJOR, VNET_VERSION_MINOR, VNET_VERSION_PATCH);
    CHECK(strcmp(vnet_version(), want) == 0);
    CHECK(vnet_resolve_host("127.0.0.1:99", VNET_FAMILY_ANY, &a) == 0 && a.port == 99);
    CHECK(vnet_resolve_host("localhost:99", VNET_FAMILY_IPV4, &a) == 0 && a.bytes[0] == 127);
    CHECK(vnet_resolve_host("nonsense", VNET_FAMILY_ANY, &a) == VNET_EINVAL);
}

static void test_addresses(void) {
    vnet_addr a, b;
    char s[64];
    CHECK(vnet_resolve(NULL, "[2001:db8::1]:80", &a) == 0);
    CHECK(vnet_addr_to_string(&a, s, sizeof s) > 0);
    CHECK(strcmp(s, "[2001:db8::1]:80") == 0);
    CHECK(vnet_resolve(NULL, s, &b) == 0 && vnet_addr_equal(&a, &b));
    CHECK(vnet_resolve(NULL, "nonsense", &a) == VNET_EINVAL);
}

int main(void) {
    test_addresses();
    test_version_and_resolve();
    test_loopback_impair();
    test_udp();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("vnet smoke: ok\n");
    return 0;
}
