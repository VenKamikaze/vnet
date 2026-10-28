// SPDX-License-Identifier: MIT
#include "test_util.h"

class Loopback : public VnetTest {
protected:
    void SetUp() override {
        net = vnet_loopback_network_create();
        ASSERT_NE(net, nullptr);
        a = LoopbackHost(net);
        b = LoopbackHost(net);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        sa = vnet_open(a, 4000);
        sb = vnet_open(b, 5000);
        ASSERT_GE(sa, 0);
        ASSERT_GE(sb, 0);
        addr_a = A("127.1.0.1:4000");
        addr_b = A("127.1.0.2:5000");
    }
    void TearDown() override {
        vnet_destroy(a);
        vnet_destroy(b);
        vnet_loopback_network_destroy(net);
    }
    vnet_loopback_network *net = nullptr;
    vnet *a = nullptr, *b = nullptr;
    int sa = -1, sb = -1;
    vnet_addr addr_a{}, addr_b{};
};

TEST_F(Loopback, HostsGetDistinctSequentialAddresses) {
    vnet_addr la{}, lb{};
    ASSERT_EQ(vnet_local_addr(a, sa, &la), 0);
    ASSERT_EQ(vnet_local_addr(b, sb, &lb), 0);
    EXPECT_EQ(Str(la), "127.1.0.1:4000");
    EXPECT_EQ(Str(lb), "127.1.0.2:5000");
}

TEST_F(Loopback, DeliversPayloadAndSenderAddress) {
    ASSERT_EQ(Send(a, sa, "hello", addr_b), 5);
    vnet_poll(b);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(Recv(b, sb, &got, &from), 5);
    EXPECT_EQ(got, "hello");
    EXPECT_TRUE(vnet_addr_equal(&from, &addr_a)) << Str(from);
}

TEST_F(Loopback, ReplyUsingFromAddress) {
    Send(a, sa, "ping", addr_b);
    vnet_poll(b);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(Recv(b, sb, &got, &from), 4);
    ASSERT_EQ(Send(b, sb, "pong", from), 4);
    vnet_poll(a);
    ASSERT_EQ(Recv(a, sa, &got), 4);
    EXPECT_EQ(got, "pong");
}

TEST_F(Loopback, EagainWhenEmpty) {
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(b);
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
}

// Delivery happens on vnet_poll() of the receiver, so code that forgets to
// poll fails here rather than later on an async (browser) backend.
TEST_F(Loopback, DatagramIsNotReceivableUntilReceiverPolls) {
    Send(a, sa, "x", addr_b);
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(a);  // the sender polling does not deliver
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(b);
    EXPECT_EQ(Recv(b, sb, &got), 1);
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
}

TEST_F(Loopback, PollOnlyExposesWhatWasSentBeforeIt) {
    Send(a, sa, "1", addr_b);
    vnet_poll(b);
    Send(a, sa, "2", addr_b);
    std::string got;
    ASSERT_EQ(Recv(b, sb, &got), 1);
    EXPECT_EQ(got, "1");
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(b);
    ASSERT_EQ(Recv(b, sb, &got), 1);
    EXPECT_EQ(got, "2");
}

TEST_F(Loopback, ZeroLengthDatagram) {
    ASSERT_EQ(vnet_sendto(a, sa, nullptr, 0, &addr_b), 0);
    vnet_poll(b);
    char c;
    vnet_addr from{};
    EXPECT_EQ(vnet_recvfrom(b, sb, &c, 1, &from), 0);
    EXPECT_TRUE(vnet_addr_equal(&from, &addr_a));
    EXPECT_EQ(vnet_recvfrom(b, sb, &c, 1, nullptr), VNET_EAGAIN);
}

TEST_F(Loopback, MaxSizeDatagramIsDeliveredIntact) {
    std::string big(VNET_MAX_DATAGRAM, '\0');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 31 + 7);
    ASSERT_EQ(Send(a, sa, big, addr_b), VNET_MAX_DATAGRAM);
    vnet_poll(b);
    std::string got;
    ASSERT_EQ(Recv(b, sb, &got), VNET_MAX_DATAGRAM);
    EXPECT_EQ(got, big);
}

TEST_F(Loopback, OversizeSendIsRejectedAndNothingIsDelivered) {
    std::string big(VNET_MAX_DATAGRAM + 1, 'z');
    EXPECT_EQ(Send(a, sa, big, addr_b), VNET_EMSGSIZE);
    EXPECT_EQ(Send(a, sa, std::string(100000, 'z'), addr_b), VNET_EMSGSIZE);
    vnet_poll(b);
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN) << "no truncated copy may arrive";
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.tx_datagrams, 0u);
}

TEST_F(Loopback, SmallReceiveBufferGivesEmsgsizeAndConsumesTheDatagram) {
    Send(a, sa, "0123456789", addr_b);
    Send(a, sa, "ok", addr_b);
    vnet_poll(b);
    char small[4];
    EXPECT_EQ(vnet_recvfrom(b, sb, small, sizeof small, nullptr), VNET_EMSGSIZE);
    std::string got;
    ASSERT_EQ(Recv(b, sb, &got), 2);  // the next datagram, not the remainder of the first
    EXPECT_EQ(got, "ok");
}

TEST_F(Loopback, ExactFitReceiveBufferWorks) {
    Send(a, sa, "abcd", addr_b);
    vnet_poll(b);
    char buf[4];
    EXPECT_EQ(vnet_recvfrom(b, sb, buf, sizeof buf, nullptr), 4);
    EXPECT_EQ(std::string(buf, 4), "abcd");
}

TEST_F(Loopback, DeliveryOrderIsGlobalSendOrder) {
    vnet *c = LoopbackHost(net);
    int sc = vnet_open(c, 6000);
    vnet_addr addr_c = A("127.1.0.3:6000");
    // Interleave two senders towards b: arrival order must equal send order.
    Send(a, sa, "a1", addr_b);
    Send(c, sc, "c1", addr_b);
    Send(a, sa, "a2", addr_b);
    Send(c, sc, "c2", addr_b);
    Send(b, sb, "self", addr_c);
    vnet_poll(b);
    std::string got;
    const char *expect[] = {"a1", "c1", "a2", "c2"};
    for (const char *e : expect) {
        ASSERT_EQ(Recv(b, sb, &got), 2);
        EXPECT_EQ(got, e);
    }
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
    vnet_poll(c);
    EXPECT_EQ(Recv(c, sc, &got), 4);
    vnet_destroy(c);
}

TEST_F(Loopback, SendToSelfViaLoopbackAddress) {
    vnet_addr me = A("127.0.0.1:4000");
    ASSERT_EQ(Send(a, sa, "me", me), 2);
    vnet_poll(a);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(Recv(a, sa, &got, &from), 2);
    EXPECT_TRUE(vnet_addr_equal(&from, &addr_a));
}

TEST_F(Loopback, UnroutableDatagramsVanishLikeUdp) {
    EXPECT_EQ(Send(a, sa, "x", A("127.1.0.2:5001")), 1);   // no such port
    EXPECT_EQ(Send(a, sa, "x", A("127.1.9.9:5000")), 1);   // no such host
    EXPECT_EQ(Send(a, sa, "x", A("8.8.8.8:53")), 1);       // not on the network
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.unroutable, 3u);
    vnet_poll(b);
    std::string got;
    EXPECT_EQ(Recv(b, sb, &got), VNET_EAGAIN);
}

TEST_F(Loopback, Ipv6DestinationIsRejected) {
    EXPECT_EQ(Send(a, sa, "x", A("[::1]:5000")), VNET_EAFNOSUPPORT);
}

TEST_F(Loopback, ArgumentValidation) {
    vnet_addr none{};
    EXPECT_EQ(vnet_sendto(a, sa, "x", 1, nullptr), VNET_EINVAL);
    EXPECT_EQ(vnet_sendto(a, sa, "x", 1, &none), VNET_EINVAL);
    vnet_addr port0 = addr_b;
    port0.port = 0;
    EXPECT_EQ(vnet_sendto(a, sa, "x", 1, &port0), VNET_EINVAL);
    EXPECT_EQ(vnet_sendto(a, sa, nullptr, 5, &addr_b), VNET_EINVAL);
    EXPECT_EQ(vnet_sendto(nullptr, sa, "x", 1, &addr_b), VNET_EINVAL);
    EXPECT_EQ(vnet_recvfrom(nullptr, sb, nullptr, 0, nullptr), VNET_EINVAL);
    EXPECT_EQ(vnet_local_addr(a, sa, nullptr), VNET_EINVAL);
}

TEST_F(Loopback, BadSocketHandles) {
    std::string got;
    EXPECT_EQ(Send(a, 99, "x", addr_b), VNET_EBADF);
    EXPECT_EQ(Send(a, -1, "x", addr_b), VNET_EBADF);
    EXPECT_EQ(Send(a, 7, "x", addr_b), VNET_EBADF);  // in range but never opened
    EXPECT_EQ(Recv(a, 99, &got), VNET_EBADF);
    vnet_addr la;
    EXPECT_EQ(vnet_local_addr(a, 3, &la), VNET_EBADF);
    int s = vnet_open(a, 4100);
    ASSERT_GE(s, 0);
    vnet_close(a, s);
    EXPECT_EQ(Send(a, s, "x", addr_b), VNET_EBADF);
    EXPECT_EQ(Recv(a, s, &got), VNET_EBADF);
    vnet_close(a, s);       // double close is harmless
    vnet_close(a, 12345);   // so is closing garbage
}

TEST_F(Loopback, PortAllocation) {
    EXPECT_EQ(vnet_open(a, 4000), VNET_EADDRINUSE);
    int e1 = vnet_open(a, 0), e2 = vnet_open(a, 0);
    ASSERT_GE(e1, 0);
    ASSERT_GE(e2, 0);
    vnet_addr l1{}, l2{};
    vnet_local_addr(a, e1, &l1);
    vnet_local_addr(a, e2, &l2);
    EXPECT_NE(l1.port, 0);
    EXPECT_NE(l1.port, l2.port);
    EXPECT_GE(l1.port, 49152);
    // The same port is free on a different host.
    EXPECT_GE(vnet_open(b, 4000), 0);
    // And reusable after close.
    vnet_close(a, sa);
    EXPECT_GE(vnet_open(a, 4000), 0);
}

TEST_F(Loopback, EphemeralPortsAreDeterministic) {
    vnet_loopback_network *n2 = vnet_loopback_network_create();
    vnet *x = LoopbackHost(n2);
    int s1 = vnet_open(x, 0);
    vnet_addr l{};
    vnet_local_addr(x, s1, &l);
    EXPECT_EQ(l.port, 49152);
    vnet_destroy(x);
    vnet_loopback_network_destroy(n2);
}

TEST_F(Loopback, SocketSlotsAreLimitedByMaxSockets) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.max_sockets = 2;
    cfg.ignore_env = 1;
    vnet *x = vnet_create(&cfg, nullptr);
    ASSERT_NE(x, nullptr);
    int s1 = vnet_open(x, 0), s2 = vnet_open(x, 0);
    EXPECT_GE(s1, 0);
    EXPECT_GE(s2, 0);
    EXPECT_EQ(vnet_open(x, 0), VNET_ENOSOCK);
    vnet_close(x, s1);
    EXPECT_GE(vnet_open(x, 0), 0);  // slot is reusable
    vnet_destroy(x);
}

TEST_F(Loopback, ReceiveQueueOverflowDropsNewestAndCounts) {
    vnet *c = LoopbackHost(net, nullptr, /*rx_queue=*/4);
    int sc = vnet_open(c, 6000);
    vnet_addr addr_c = A("127.1.0.3:6000");
    for (int i = 0; i < 10; i++) Send(a, sa, std::string(1, (char)('0' + i)), addr_c);
    vnet_poll(c);
    std::string got;
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(Recv(c, sc, &got), 1);
        EXPECT_EQ(got, std::string(1, (char)('0' + i)));
    }
    EXPECT_EQ(Recv(c, sc, &got), VNET_EAGAIN);
    vnet_stats st;
    vnet_get_stats(c, &st);
    EXPECT_EQ(st.rx_overflow, 6u);
    // The ring keeps working after wrap-around.
    for (int round = 0; round < 20; round++) {
        Send(a, sa, "w", addr_c);
        vnet_poll(c);
        ASSERT_EQ(Recv(c, sc, &got), 1) << round;
    }
    vnet_destroy(c);
}

TEST_F(Loopback, ClosingASocketDiscardsItsQueuedDatagrams) {
    Send(a, sa, "stale", addr_b);
    vnet_poll(b);
    vnet_close(b, sb);
    int sb2 = vnet_open(b, 5000);
    ASSERT_GE(sb2, 0);
    vnet_poll(b);
    std::string got;
    EXPECT_EQ(Recv(b, sb2, &got), VNET_EAGAIN);
}

TEST_F(Loopback, HostAddressConfiguration) {
    vnet *x = LoopbackHost(net, nullptr, 0, "10.9.8.7");
    ASSERT_NE(x, nullptr);
    int sx = vnet_open(x, 7000);
    vnet_addr l{};
    vnet_local_addr(x, sx, &l);
    EXPECT_EQ(Str(l), "10.9.8.7:7000");
    Send(a, sa, "to-x", A("10.9.8.7:7000"));
    vnet_poll(x);
    std::string got;
    EXPECT_EQ(Recv(x, sx, &got), 4);
    // Next automatic address skips what is taken and fills in order.
    vnet *y = LoopbackHost(net);
    int sy = vnet_open(y, 1);
    vnet_local_addr(y, sy, &l);
    EXPECT_EQ(Str(l), "127.1.0.3:1");
    vnet_destroy(y);
    vnet_destroy(x);

    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.ignore_env = 1;
    int err = 0;
    cfg.loopback_host = "127.1.0.1";  // already taken by `a`
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);
    EXPECT_EQ(err, VNET_EADDRINUSE);
    const char *bad[] = {"nonsense", "1.2.3", "::1", "127.0.0.1", "0.0.0.0", "1.2.3.4:5"};
    for (const char *h : bad) {
        cfg.loopback_host = h;
        err = 0;
        EXPECT_EQ(vnet_create(&cfg, &err), nullptr) << h;
        EXPECT_EQ(err, VNET_EINVAL) << h;
    }
}

TEST_F(Loopback, SeparateNetworksAreIsolated) {
    vnet_loopback_network *n2 = vnet_loopback_network_create();
    vnet *x = LoopbackHost(n2);  // also 127.1.0.1
    int sx = vnet_open(x, 4000);
    Send(a, sa, "cross", A("127.1.0.1:4000"));  // to a's own address, in net 1
    vnet_poll(x);
    std::string got;
    EXPECT_EQ(Recv(x, sx, &got), VNET_EAGAIN);
    vnet_poll(a);
    EXPECT_EQ(Recv(a, sa, &got), 5);
    vnet_destroy(x);
    vnet_loopback_network_destroy(n2);
}

TEST_F(Loopback, ContextWithoutNetworkGetsAPrivateOne) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.ignore_env = 1;
    vnet *x = vnet_create(&cfg, nullptr);
    ASSERT_NE(x, nullptr);
    int s1 = vnet_open(x, 1000), s2 = vnet_open(x, 2000);
    Send(x, s1, "self", A("127.0.0.1:2000"));
    vnet_poll(x);
    std::string got;
    EXPECT_EQ(Recv(x, s2, &got), 4);
    vnet_destroy(x);
}

TEST_F(Loopback, StatsTrackTraffic) {
    Send(a, sa, "12345", addr_b);
    Send(a, sa, "678", addr_b);
    vnet_poll(b);
    std::string got;
    Recv(b, sb, &got);
    Recv(b, sb, &got);
    vnet_stats sa_, sb_;
    vnet_get_stats(a, &sa_);
    vnet_get_stats(b, &sb_);
    EXPECT_EQ(sa_.tx_datagrams, 2u);
    EXPECT_EQ(sa_.tx_bytes, 8u);
    EXPECT_EQ(sa_.wire_datagrams, 2u);
    EXPECT_EQ(sb_.rx_datagrams, 2u);
    EXPECT_EQ(sb_.rx_bytes, 8u);
}

// The headline use: several peers stepped in one thread, exactly repeatable.
TEST_F(Loopback, MultiPeerRunIsExactlyRepeatable) {
    auto run = [&]() {
        vnet_loopback_network *n = vnet_loopback_network_create();
        std::vector<vnet *> hosts;
        std::vector<int> socks;
        for (int i = 0; i < 4; i++) {
            hosts.push_back(LoopbackHost(n));
            socks.push_back(vnet_open(hosts.back(), 9000));
        }
        std::string trace;
        for (int round = 0; round < 50; round++) {
            for (int i = 0; i < 4; i++) {
                int j = (i + 1 + round) % 4;
                if (j == i) j = (j + 1) % 4;
                char ad[32];
                snprintf(ad, sizeof ad, "127.1.0.%d:9000", j + 1);
                Send(hosts[i], socks[i], "r" + std::to_string(round) + "h" + std::to_string(i), A(ad));
            }
            vnet_loopback_network_step(n, 1);
            for (int i = 0; i < 4; i++) {
                std::string got;
                vnet_addr from{};
                while (Recv(hosts[i], socks[i], &got, &from) >= 0)
                    trace += std::to_string(i) + "<" + Str(from) + ":" + got + ";";
            }
        }
        for (auto *h : hosts) vnet_destroy(h);
        vnet_loopback_network_destroy(n);
        return trace;
    };
    std::string t1 = run(), t2 = run();
    EXPECT_FALSE(t1.empty());
    EXPECT_EQ(t1, t2);
}
