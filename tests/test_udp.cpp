// SPDX-License-Identifier: MIT
#include "test_util.h"

#include <atomic>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// A test-controlled clock for the UDP backend (so impairment timing on real
// sockets does not depend on sleeping).
static uint64_t g_now = 0;
static uint64_t TestClock(void *) { return g_now; }

class Udp : public VnetTest {
protected:
    void SetUp() override {
        a = UdpHost();
        b = UdpHost();
        ASSERT_TRUE(a && b);
        sa = vnet_open(a, 0);
        sb = vnet_open(b, 0);
        ASSERT_GE(sa, 0);
        ASSERT_GE(sb, 0);
        vnet_addr l{};
        ASSERT_EQ(vnet_local_addr(b, sb, &l), 0);
        to_b = A("127.0.0.1:1");
        to_b.port = l.port;
        ASSERT_EQ(vnet_local_addr(a, sa, &l), 0);
        port_a = l.port;
    }
    void TearDown() override {
        vnet_destroy(a);
        vnet_destroy(b);
    }
    vnet *a = nullptr, *b = nullptr;
    int sa = -1, sb = -1;
    vnet_addr to_b{};
    uint16_t port_a = 0;
};

TEST_F(Udp, EphemeralPortIsAssigned) {
    EXPECT_NE(to_b.port, 0);
    EXPECT_NE(port_a, 0);
    EXPECT_NE(port_a, to_b.port);
}

TEST_F(Udp, SendAndReceiveOverLocalhost) {
    ASSERT_EQ(Send(a, sa, "hello udp", to_b), 9);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(RecvWait(b, sb, &got, &from), 9);
    EXPECT_EQ(got, "hello udp");
    EXPECT_EQ(from.family, VNET_AF_IPV4);
    EXPECT_EQ(from.port, port_a);
    EXPECT_EQ(Str(from), "127.0.0.1:" + std::to_string(port_a));
}

TEST_F(Udp, EagainWhenNothingToReceive) {
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(b);
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
}

TEST_F(Udp, RecvNeverBlocks) {
    auto t0 = std::chrono::steady_clock::now();
    std::string got;
    for (int i = 0; i < 1000; i++) Recv(b, sb, &got);
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count(), 1000);
}

TEST_F(Udp, ReplyToSenderAddress) {
    Send(a, sa, "ping", to_b);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(RecvWait(b, sb, &got, &from), 4);
    ASSERT_EQ(Send(b, sb, "pong", from), 4);
    ASSERT_EQ(RecvWait(a, sa, &got), 4);
    EXPECT_EQ(got, "pong");
}

TEST_F(Udp, ZeroLengthDatagram) {
    ASSERT_EQ(vnet_sendto(a, sa, nullptr, 0, &to_b), 0);
    char c;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int n;
    while ((n = vnet_recvfrom(b, sb, &c, 1, nullptr)) == VNET_EAGAIN &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(n, 0);
}

TEST_F(Udp, MaxSizeDatagramRoundTripsIntact) {
    std::string big(VNET_MAX_DATAGRAM, '\0');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 13 + 5);
    ASSERT_EQ(Send(a, sa, big, to_b), VNET_MAX_DATAGRAM);
    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), VNET_MAX_DATAGRAM);
    EXPECT_EQ(got, big);
}

// If oversize sends were truncated rather than rejected, a 1100-byte prefix
// would arrive ahead of the marker.
TEST_F(Udp, OversizeSendIsRejectedNotTruncatedOrSent) {
    EXPECT_EQ(Send(a, sa, std::string(VNET_MAX_DATAGRAM + 1, 'o'), to_b), VNET_EMSGSIZE);
    EXPECT_EQ(Send(a, sa, std::string(5000, 'o'), to_b), VNET_EMSGSIZE);
    ASSERT_EQ(Send(a, sa, "marker", to_b), 6);
    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), 6);
    EXPECT_EQ(got, "marker");
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.tx_datagrams, 1u);
    EXPECT_EQ(st.wire_datagrams, 1u);
}

TEST_F(Udp, SmallReceiveBufferGivesEmsgsizeAndConsumesTheDatagram) {
    Send(a, sa, "0123456789", to_b);
    Send(a, sa, "next", to_b);
    char small[4];
    int n;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((n = vnet_recvfrom(b, sb, small, sizeof small, nullptr)) == VNET_EAGAIN &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(n, VNET_EMSGSIZE);
    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), 4);
    EXPECT_EQ(got, "next");
}

#if !defined(_WIN32)
// A foreign (non-vnet) sender can put arbitrarily large datagrams on the
// wire. They must be dropped, never delivered truncated to 1100 bytes.
TEST_F(Udp, OversizeInboundFromForeignSenderIsDroppedNotTruncated) {
    int raw = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(raw, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(to_b.port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string over(VNET_MAX_DATAGRAM + 1, 'q'), huge(4000, 'q'), ok(VNET_MAX_DATAGRAM, 'k');
    for (const std::string *s : {&over, &huge, &ok})
        ASSERT_EQ(sendto(raw, s->data(), s->size(), 0, (sockaddr *)&dst, sizeof dst), (ssize_t)s->size());
    ASSERT_EQ(sendto(raw, "end", 3, 0, (sockaddr *)&dst, sizeof dst), 3);
    close(raw);

    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), VNET_MAX_DATAGRAM);
    EXPECT_EQ(got, ok) << "the exactly-max datagram survives, the oversize ones do not";
    ASSERT_EQ(RecvWait(b, sb, &got), 3);
    EXPECT_EQ(got, "end");
    vnet_stats st;
    vnet_get_stats(b, &st);
    EXPECT_EQ(st.rx_oversize, 2u);
}
#endif

TEST_F(Udp, BindingTheSamePortTwiceFails) {
    EXPECT_EQ(vnet_open(b, to_b.port), VNET_EADDRINUSE);
    // A different context cannot take it either.
    EXPECT_EQ(vnet_open(a, to_b.port), VNET_EADDRINUSE);
}

TEST_F(Udp, ClosingReleasesThePort) {
    vnet_close(b, sb);
    int s = vnet_open(a, to_b.port);
    EXPECT_GE(s, 0);
}

TEST_F(Udp, BadHandlesAndArguments) {
    std::string got;
    EXPECT_EQ(Send(a, 99, "x", to_b), VNET_EBADF);
    EXPECT_EQ(Recv(a, -1, &got), VNET_EBADF);
    vnet_addr la;
    EXPECT_EQ(vnet_local_addr(a, 5, &la), VNET_EBADF);
    vnet_addr port0 = to_b;
    port0.port = 0;
    EXPECT_EQ(vnet_sendto(a, sa, "x", 1, &port0), VNET_EINVAL);
    int s = vnet_open(a, 0);
    vnet_close(a, s);
    EXPECT_EQ(Send(a, s, "x", to_b), VNET_EBADF);
    vnet_close(a, s);
}

TEST_F(Udp, Ipv6DestinationOnIpv4OnlyContextIsRejected) {
    EXPECT_EQ(Send(a, sa, "x", A("[::1]:9")), VNET_EAFNOSUPPORT);
}

TEST_F(Udp, BurstOfDatagramsAllArrive) {
    const int N = 200;
    for (int i = 0; i < N; i++) ASSERT_EQ(Send(a, sa, std::to_string(i), to_b), (int)std::to_string(i).size());
    std::vector<bool> seen(N, false);
    int count = 0;
    std::string got;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count < N && std::chrono::steady_clock::now() < deadline) {
        if (Recv(b, sb, &got) >= 0) {
            seen[std::stoi(got)] = true;
            count++;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_EQ(count, N);
    for (int i = 0; i < N; i++) EXPECT_TRUE(seen[i]) << i;
}

TEST_F(Udp, PingPongManyRounds) {
    for (int i = 0; i < 100; i++) {
        std::string msg = "p" + std::to_string(i), got;
        vnet_addr from{};
        ASSERT_EQ(Send(a, sa, msg, to_b), (int)msg.size());
        ASSERT_EQ(RecvWait(b, sb, &got, &from), (int)msg.size());
        ASSERT_EQ(got, msg);
        ASSERT_EQ(Send(b, sb, got + "!", from), (int)msg.size() + 1);
        ASSERT_EQ(RecvWait(a, sa, &got), (int)msg.size() + 1);
        ASSERT_EQ(got, msg + "!");
    }
}

TEST_F(Udp, SendToUnboundPortIsSilent) {
    // Nobody is listening: UDP gives the sender no error and no echo.
    vnet_addr nowhere = A("127.0.0.1:9");
    EXPECT_EQ(Send(a, sa, "void", nowhere), 4);
    std::string got;
    EXPECT_EQ(Recv(a, sa, &got), VNET_EAGAIN);
}

TEST(UdpFamily, DualStackOrFallbackReachesIpv4PeersAndReportsIpv4Senders) {
    EnvGuard env;
    vnet *x = UdpHost(VNET_FAMILY_ANY), *y = UdpHost(VNET_FAMILY_ANY);
    ASSERT_TRUE(x && y);
    int sx = vnet_open(x, 0), sy = vnet_open(y, 0);
    ASSERT_GE(sx, 0);
    ASSERT_GE(sy, 0);
    vnet_addr ly{}, lx{};
    vnet_local_addr(y, sy, &ly);
    vnet_local_addr(x, sx, &lx);
    vnet_addr to = A("127.0.0.1:1");
    to.port = ly.port;
    ASSERT_EQ(Send(x, sx, "dual", to), 4);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(RecvWait(y, sy, &got, &from), 4);
    EXPECT_EQ(got, "dual");
    // IPv4-mapped sources must come back as plain IPv4 so they compare equal
    // to what resolve() gives for the same peer.
    EXPECT_EQ(from.family, VNET_AF_IPV4);
    vnet_addr expect = A("127.0.0.1:1");
    expect.port = lx.port;
    EXPECT_TRUE(vnet_addr_equal(&from, &expect)) << Str(from);
    vnet_destroy(x);
    vnet_destroy(y);
}

#if !defined(_WIN32)
static bool Ipv6LoopbackAvailable() {
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s < 0) return false;
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_addr = in6addr_loopback;
    bool ok = bind(s, (sockaddr *)&sa, sizeof sa) == 0;
    close(s);
    return ok;
}
#else
static bool Ipv6LoopbackAvailable() { return false; }
#endif

// Environment-dependent: needs an IPv6 loopback interface. Hosts without one
// (some containers) cannot run it; the explicit skip message says so.
TEST(UdpFamily, Ipv6LoopbackRoundTrip) {
    if (!Ipv6LoopbackAvailable()) GTEST_SKIP() << "no IPv6 loopback (::1) on this host";
    EnvGuard env;
    vnet *x = UdpHost(VNET_FAMILY_IPV6), *y = UdpHost(VNET_FAMILY_IPV6);
    ASSERT_TRUE(x && y);
    int sx = vnet_open(x, 0), sy = vnet_open(y, 0);
    ASSERT_GE(sx, 0);
    ASSERT_GE(sy, 0);
    vnet_addr ly{}, lx{};
    vnet_local_addr(y, sy, &ly);
    vnet_local_addr(x, sx, &lx);
    EXPECT_EQ(ly.family, VNET_AF_IPV6);
    vnet_addr to = A("[::1]:1");
    to.port = ly.port;
    ASSERT_EQ(Send(x, sx, "six", to), 3);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(RecvWait(y, sy, &got, &from), 3);
    EXPECT_EQ(got, "six");
    EXPECT_EQ(from.family, VNET_AF_IPV6);
    EXPECT_EQ(Str(from), "[::1]:" + std::to_string(lx.port));
    // IPv6-only sockets can't send IPv4.
    EXPECT_EQ(Send(x, sx, "v4", A("127.0.0.1:9")), VNET_EAFNOSUPPORT);
    vnet_destroy(x);
    vnet_destroy(y);
}

TEST(UdpFamily, InvalidFamilyIsRejected) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.family = 5;
    cfg.ignore_env = 1;
    int err = 0;
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);
    EXPECT_EQ(err, VNET_EINVAL);
}

// ---- impairment over real sockets -----------------------------------------

class UdpImpair : public VnetTest {
protected:
    void SetUp() override { g_now = 1000; }
};

TEST_F(UdpImpair, LatencyHoldsDatagramsBackUntilTheClockAdvances) {
    vnet_impair im = {};
    im.latency_ms = 50;
    vnet *a = UdpHost(VNET_FAMILY_IPV4, &im, TestClock), *b = UdpHost();
    int sa = vnet_open(a, 0), sb = vnet_open(b, 0);
    vnet_addr l{};
    vnet_local_addr(b, sb, &l);
    vnet_addr to = A("127.0.0.1:1");
    to.port = l.port;
    ASSERT_EQ(Send(a, sa, "slow", to), 4);
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.tx_datagrams, 1u);
    EXPECT_EQ(st.wire_datagrams, 0u) << "nothing may reach the socket yet";
    g_now += 49;
    vnet_poll(a);
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.wire_datagrams, 0u);
    g_now += 1;
    vnet_poll(a);
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.wire_datagrams, 1u);
    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), 4);
    EXPECT_EQ(got, "slow");
    vnet_destroy(a);
    vnet_destroy(b);
}

TEST_F(UdpImpair, LossAndDuplicationApplyToRealSockets) {
    vnet_impair im = {};
    im.loss_pct = 100;
    vnet *a = UdpHost(VNET_FAMILY_IPV4, &im), *b = UdpHost();
    int sa = vnet_open(a, 0), sb = vnet_open(b, 0);
    vnet_addr l{};
    vnet_local_addr(b, sb, &l);
    vnet_addr to = A("127.0.0.1:1");
    to.port = l.port;
    for (int i = 0; i < 20; i++) Send(a, sa, "x", to);
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.impair_lost, 20u);
    EXPECT_EQ(st.wire_datagrams, 0u);
    // Prove the socket really is empty: a plain (unimpaired) marker is first.
    vnet *c = UdpHost();
    int sc = vnet_open(c, 0);
    Send(c, sc, "marker", to);
    std::string got;
    ASSERT_EQ(RecvWait(b, sb, &got), 6);
    EXPECT_EQ(got, "marker");
    vnet_destroy(a);
    vnet_destroy(c);

    im = {};
    im.duplicate_pct = 100;
    a = UdpHost(VNET_FAMILY_IPV4, &im);
    sa = vnet_open(a, 0);
    for (int i = 0; i < 10; i++) Send(a, sa, "d", to);
    int n = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (n < 20 && std::chrono::steady_clock::now() < deadline) {
        if (Recv(b, sb, &got) >= 0) n++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(n, 20);
    vnet_destroy(a);
    vnet_destroy(b);
}
