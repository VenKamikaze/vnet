// SPDX-License-Identifier: MIT
#ifndef VNET_TEST_UTIL_H
#define VNET_TEST_UTIL_H

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "vnet.h"

// Clears VNET_IMPAIR for the whole test (and restores it) so a developer's
// shell environment can never change a test result.
class EnvGuard {
public:
    EnvGuard() {
        const char *old = std::getenv("VNET_IMPAIR");
        had_ = old != nullptr;
        if (had_) saved_ = old;
        unsetenv("VNET_IMPAIR");
    }
    ~EnvGuard() {
        if (had_) setenv("VNET_IMPAIR", saved_.c_str(), 1);
        else unsetenv("VNET_IMPAIR");
    }
    void set(const char *v) { setenv("VNET_IMPAIR", v, 1); }

private:
    bool had_;
    std::string saved_;
};

class VnetTest : public ::testing::Test {
protected:
    EnvGuard env;
};

inline vnet_addr A(const char *s) {
    vnet_addr a = {};
    EXPECT_EQ(vnet_resolve(nullptr, s, &a), 0) << s;
    return a;
}

inline std::string Str(const vnet_addr &a) {
    char buf[96];
    int n = vnet_addr_to_string(&a, buf, sizeof buf);
    EXPECT_GE(n, 0);
    return n >= 0 ? std::string(buf, (size_t)n) : std::string();
}

// A loopback context on `net`.
inline vnet *LoopbackHost(vnet_loopback_network *net, const vnet_impair *imp = nullptr,
                          int rx_queue = 0, const char *host = nullptr) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.loopback_host = host;
    cfg.ignore_env = 1;
    cfg.rx_queue_len = rx_queue;
    if (imp) cfg.impair = *imp;
    int err = 0;
    vnet *v = vnet_create(&cfg, &err);
    EXPECT_NE(v, nullptr) << vnet_strerror(err);
    return v;
}

inline vnet *UdpHost(int family = VNET_FAMILY_IPV4, const vnet_impair *imp = nullptr,
                     vnet_clock_fn clk = nullptr, void *clk_user = nullptr) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_UDP;
    cfg.family = family;
    cfg.ignore_env = 1;
    cfg.clock = clk;
    cfg.clock_user = clk_user;
    if (imp) cfg.impair = *imp;
    int err = 0;
    vnet *v = vnet_create(&cfg, &err);
    EXPECT_NE(v, nullptr) << vnet_strerror(err);
    return v;
}

inline int Send(vnet *v, int s, const std::string &payload, const vnet_addr &to) {
    return vnet_sendto(v, s, payload.data(), payload.size(), &to);
}

// Receive one datagram or return a negative error.
inline int Recv(vnet *v, int s, std::string *out, vnet_addr *from = nullptr) {
    char buf[VNET_MAX_DATAGRAM];
    vnet_addr f = {};
    int n = vnet_recvfrom(v, s, buf, sizeof buf, &f);
    if (n >= 0 && out) out->assign(buf, (size_t)n);
    if (from) *from = f;
    return n;
}

// Poll `v` until a datagram arrives or `timeout_ms` of REAL time passes.
// Used only for real sockets; the deadline is a bound, not an expectation.
inline int RecvWait(vnet *v, int s, std::string *out, vnet_addr *from = nullptr,
                    int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        vnet_poll(v);
        int n = Recv(v, s, out, from);
        if (n != VNET_EAGAIN) return n;
        if (std::chrono::steady_clock::now() >= deadline) return VNET_EAGAIN;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

#endif
