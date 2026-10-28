// SPDX-License-Identifier: MIT
#include "test_util.h"

#include <dirent.h>

class Lifecycle : public VnetTest {};

static int CountOpenFds() {
    int n = 0;
#if defined(__APPLE__)
    const char *fd_dir = "/dev/fd";
#else
    const char *fd_dir = "/proc/self/fd";
#endif
    if (DIR *d = opendir(fd_dir)) {
        while (readdir(d)) n++;
        closedir(d);
    }
    return n;
}

TEST_F(Lifecycle, RepeatedCreateAndDestroyLoopback) {
    for (int i = 0; i < 500; i++) {
        vnet_loopback_network *net = vnet_loopback_network_create();
        vnet *a = LoopbackHost(net), *b = LoopbackHost(net);
        ASSERT_TRUE(a && b) << i;
        int sa = vnet_open(a, 1000), sb = vnet_open(b, 2000);
        ASSERT_EQ(Send(a, sa, "x", A("127.1.0.2:2000")), 1);
        vnet_poll(b);
        std::string got;
        ASSERT_EQ(Recv(b, sb, &got), 1);
        vnet_destroy(a);
        vnet_destroy(b);
        vnet_loopback_network_destroy(net);
    }
}

TEST_F(Lifecycle, RepeatedCreateAndDestroyUdpReusesTheSamePortAndLeaksNoFds) {
    int fds_before = CountOpenFds();
    uint16_t port = 0;
    for (int i = 0; i < 300; i++) {
        vnet *v = UdpHost();
        ASSERT_NE(v, nullptr) << i;
        int s = vnet_open(v, port);
        ASSERT_GE(s, 0) << "iteration " << i << " port " << port;
        if (i == 0) {
            vnet_addr l{};
            vnet_local_addr(v, s, &l);
            port = l.port;
        }
        // Sockets left open are closed by destroy (even half of the time).
        if (i % 2) vnet_close(v, s);
        vnet_destroy(v);
    }
    EXPECT_GT(fds_before, 3) << "fd listing must work on this platform, or the check is vacuous";
    EXPECT_EQ(CountOpenFds(), fds_before);
}

TEST_F(Lifecycle, ManyContextsInOneProcessAreIndependent) {
    std::vector<vnet *> v;
    std::vector<int> s;
    std::vector<uint16_t> ports;
    for (int i = 0; i < 8; i++) {
        v.push_back(UdpHost());
        s.push_back(vnet_open(v.back(), 0));
        vnet_addr l{};
        vnet_local_addr(v.back(), s.back(), &l);
        ports.push_back(l.port);
    }
    for (int i = 0; i < 8; i++) {
        vnet_addr to = A("127.0.0.1:1");
        to.port = ports[(i + 1) % 8];
        Send(v[i], s[i], "from" + std::to_string(i), to);
    }
    for (int i = 0; i < 8; i++) {
        std::string got;
        ASSERT_GT(RecvWait(v[i], s[i], &got), 0);
        EXPECT_EQ(got, "from" + std::to_string((i + 7) % 8));
    }
    // Destroying one leaves the others working.
    vnet_destroy(v[3]);
    vnet_addr to = A("127.0.0.1:1");
    to.port = ports[5];
    Send(v[4], s[4], "still", to);
    std::string got;
    ASSERT_EQ(RecvWait(v[5], s[5], &got), 5);
    for (int i = 0; i < 8; i++)
        if (i != 3) vnet_destroy(v[i]);
}

TEST_F(Lifecycle, TimeIsTheContextClockOnEveryBackend) {
    EXPECT_EQ(vnet_time(nullptr), 0u);

    // Loopback: the network's virtual clock, shared by all its hosts.
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net), *b = LoopbackHost(net);
    EXPECT_EQ(vnet_time(a), 0u);
    vnet_loopback_network_advance(net, 7);
    EXPECT_EQ(vnet_time(a), 7u);
    EXPECT_EQ(vnet_time(b), 7u);
    vnet_loopback_network_step(net, 33);
    EXPECT_EQ(vnet_time(a), 40u);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);

    // An injected clock wins over the backend default (loopback and UDP).
    static uint64_t t;
    t = 1234;
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.ignore_env = 1;
    cfg.clock = [](void *) { return t; };
    vnet *u = vnet_create(&cfg, nullptr);
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(vnet_time(u), 1234u);
    t = 2000;
    EXPECT_EQ(vnet_time(u), 2000u);
    vnet_destroy(u);

    // UDP default: real monotonic time that actually advances.
    vnet *r = UdpHost();
    uint64_t t0 = vnet_time(r), prev = t0, now = t0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (now < t0 + 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now = vnet_time(r);
        EXPECT_GE(now, prev) << "monotonic";
        prev = now;
    }
    EXPECT_GE(now, t0 + 5);
    vnet_destroy(r);
}

TEST_F(Lifecycle, NullTolerance) {
    vnet_destroy(nullptr);
    vnet_poll(nullptr);
    vnet_close(nullptr, 0);
    vnet_loopback_network_destroy(nullptr);
    vnet_loopback_network_advance(nullptr, 5);
    vnet_loopback_network_step(nullptr, 5);
    EXPECT_EQ(vnet_loopback_network_time(nullptr), 0u);
    EXPECT_EQ(vnet_open(nullptr, 0), VNET_EINVAL);
    vnet_stats st;
    st.tx_datagrams = 5;
    vnet_get_stats(nullptr, &st);
    EXPECT_EQ(st.tx_datagrams, 0u);
}

TEST_F(Lifecycle, NullConfigMeansUdpDefaults) {
    int err = -1;
    vnet *v = vnet_create(nullptr, &err);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(err, VNET_OK);
    EXPECT_GE(vnet_open(v, 0), 0);
    vnet_destroy(v);
    v = vnet_create(nullptr, nullptr);  // err pointer is optional
    ASSERT_NE(v, nullptr);
    vnet_destroy(v);
}

TEST_F(Lifecycle, UnknownBackendFailsCleanly) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = 77;
    int err = 0;
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);
    EXPECT_EQ(err, VNET_ENOTSUP);
    EXPECT_EQ(vnet_create(&cfg, nullptr), nullptr);
}

TEST_F(Lifecycle, ConfigInitZeroesEverything) {
    vnet_config cfg;
    std::memset(&cfg, 0xCD, sizeof cfg);
    vnet_config_init(&cfg);
    vnet_config zero;
    std::memset(&zero, 0, sizeof zero);
    EXPECT_EQ(std::memcmp(&cfg, &zero, sizeof cfg), 0);
}

TEST_F(Lifecycle, LoggingIsSilentByDefaultAndRoutedWhenSet) {
    // Default: nothing to observe, but creation failure must not crash without a sink.
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_host = "garbage";
    cfg.ignore_env = 1;
    EXPECT_EQ(vnet_create(&cfg, nullptr), nullptr);

    struct Sink { int n = 0; int last_level = -1; std::string last; } sink;
    cfg.log = [](void *u, int level, const char *m) {
        auto *s = static_cast<Sink *>(u);
        s->n++;
        s->last_level = level;
        s->last = m;
    };
    cfg.log_user = &sink;
    EXPECT_EQ(vnet_create(&cfg, nullptr), nullptr);
    EXPECT_EQ(sink.n, 1);
    EXPECT_EQ(sink.last_level, VNET_LOG_ERROR);
    EXPECT_NE(sink.last.find("garbage"), std::string::npos);
}

TEST_F(Lifecycle, CustomClockOverridesBackendDefault) {
    static uint64_t t;
    t = 0;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.ignore_env = 1;
    cfg.impair.latency_ms = 100;
    cfg.clock = [](void *) { return t; };
    vnet *a = vnet_create(&cfg, nullptr);
    cfg.clock = nullptr;
    cfg.impair = {};
    vnet *b = vnet_create(&cfg, nullptr);
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    Send(a, sa, "x", A("127.1.0.2:2"));
    vnet_loopback_network_advance(net, 1000);  // network time irrelevant to a's clock
    vnet_poll(a);
    vnet_poll(b);
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    t = 100;
    vnet_poll(a);
    vnet_poll(b);
    EXPECT_EQ(Recv(b, sb, &got), 1);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

// A context must not dangle if its network is destroyed first (documented
// as wrong, but it must not crash; run under ASan in CI).
TEST_F(Lifecycle, NetworkDestroyedBeforeContextDegradesSafely) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net);
    int s = vnet_open(a, 1);
    vnet_loopback_network_destroy(net);
    EXPECT_EQ(Send(a, s, "x", A("127.1.0.1:1")), 1);
    vnet_poll(a);
    std::string got;
    EXPECT_EQ(Recv(a, s, &got), VNET_EAGAIN);
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.unroutable, 1u);
    vnet_destroy(a);
}

#if defined(VNET_TEST_COUNT_ALLOCS)
extern "C" {
extern unsigned long g_alloc_calls;
}
// Steady state must not touch the heap (ring buffers are allocated up front).
TEST_F(Lifecycle, SteadyStateSendRecvPollDoesNotAllocate) {
    vnet_impair im = {};
    im.latency_ms = 5;
    im.jitter_ms = 3;
    im.loss_pct = 5;
    im.duplicate_pct = 5;
    im.reorder_pct = 5;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net, &im), *b = LoopbackHost(net);
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    vnet *u = UdpHost(VNET_FAMILY_IPV4, &im);
    vnet *w = UdpHost();
    int su = vnet_open(u, 0), sw = vnet_open(w, 0);
    vnet_addr l{};
    vnet_local_addr(w, sw, &l);
    vnet_addr wto = A("127.0.0.1:1");
    wto.port = l.port;
    vnet_addr to = A("127.1.0.2:2");
    char buf[VNET_MAX_DATAGRAM] = {};
    vnet_addr from;
    unsigned long before = g_alloc_calls;
    for (int i = 0; i < 20000; i++) {
        vnet_sendto(a, sa, buf, 100, &to);
        vnet_sendto(u, su, buf, 100, &wto);
        vnet_loopback_network_step(net, 1);
        vnet_poll(u);
        while (vnet_recvfrom(b, sb, buf, sizeof buf, &from) >= 0) {}
        while (vnet_recvfrom(w, sw, buf, sizeof buf, &from) >= 0) {}
        vnet_stats st;
        vnet_get_stats(a, &st);
    }
    unsigned long after = g_alloc_calls;
    EXPECT_EQ(after - before, 0ul);
    vnet_destroy(u);
    vnet_destroy(w);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}
#endif
