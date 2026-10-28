// SPDX-License-Identifier: MIT
// The relay client backend (VNET_BACKEND_RELAY_UDP) against a scripted fake
// relay, with a hand-moved clock: handshake, idempotent retries, timeouts,
// keepalives, framing and failure handling, without sleeping.
#include "relay_test_util.h"

extern unsigned long g_alloc_calls;

class RelayClient : public VnetTest {
protected:
    void TearDown() override { vnet_destroy(v); }

    // Create a context; the handshake request is sent from inside vnet_create().
    vnet *Create(const char *room = nullptr, int keepalive_ms = 0, int timeout_ms = 0, int *err = nullptr,
                 const vnet_impair *imp = nullptr) {
        addr = relay.addr();
        vnet_config cfg = RelayUdpConfig(relay, &clk, room);
        cfg.relay = addr.c_str();
        cfg.keepalive_ms = keepalive_ms;
        cfg.relay_timeout_ms = timeout_ms;
        if (imp) cfg.impair = *imp;
        int e = 0;
        vnet *c = vnet_create(&cfg, &e);
        if (err) *err = e;
        return c;
    }

    void Start(const char *room = nullptr, int keepalive_ms = 0, int timeout_ms = 0) {
        v = Create(room, keepalive_ms, timeout_ms);
        ASSERT_NE(v, nullptr);
        ASSERT_TRUE(relay.RecvType(room ? RELAY_T_JOIN : RELAY_T_CREATE, &req));
    }

    // Poll (real time bounded) until the state is `want`.
    bool PollUntilState(int want) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            vnet_poll(v);
            if (vnet_status(v, nullptr) == want) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void Enter(int octet = 1, const char *code = "ABCDEF", int max_members = 8, int keepalive_s = 5) {
        relay.Send(RoomFrame(req, octet, code, max_members, keepalive_s));
        ASSERT_TRUE(PollUntilState(VNET_STATUS_READY));
    }

    // Poll until the client has received `n` more datagrams on socket s.
    bool RecvOne(int s, Bytes *out, vnet_addr *from = nullptr) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            vnet_poll(v);
            char buf[VNET_MAX_DATAGRAM];
            vnet_addr f{};
            int n = vnet_recvfrom(v, s, buf, sizeof buf, &f);
            if (n >= 0) {
                out->assign(buf, buf + n);
                if (from) *from = f;
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    FakeRelay relay;
    TestClockState clk;
    std::string addr;
    vnet *v = nullptr;
    Bytes req;
};

// ------------------------------------------------------------- handshake

TEST_F(RelayClient, CreateRequestIsPaddedAndStartsConnecting) {
    Start();
    ASSERT_EQ(req.size(), (size_t)RELAY_MIN_UDP_HANDSHAKE);  // replies must be smaller than the request
    EXPECT_EQ(req[0], RELAY_T_CREATE);
    EXPECT_EQ(req[1], RELAY_PROTO_VERSION);
    Bytes nonce(req.begin() + 2, req.begin() + 10);
    EXPECT_NE(nonce, Bytes(8, 0));
    for (size_t i = 10; i < req.size(); i++) EXPECT_EQ(req[i], 0) << i;  // zero padding

    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_CONNECTING);
    EXPECT_EQ(st.state, VNET_STATUS_CONNECTING);
    EXPECT_EQ(st.error, 0);
    EXPECT_STREQ(st.transport, "relay-udp");
    EXPECT_STREQ(st.room, "");
    EXPECT_EQ(st.self.family, VNET_AF_NONE);
}

TEST_F(RelayClient, JoinRequestCarriesTheUpperCasedCode) {
    Start("abc234");
    ASSERT_EQ(req.size(), (size_t)RELAY_MIN_UDP_HANDSHAKE);
    EXPECT_EQ(req[0], RELAY_T_JOIN);
    EXPECT_EQ(std::string(req.begin() + 10, req.begin() + 16), "ABC234");
}

TEST_F(RelayClient, BadRoomCodesAreRejectedUpFront) {
    for (const char *bad : {"ABC", "ABCDEFG", "ABCDE0", "ABCDEO", "ABCDE1", "ABCDEI", "ABC-EF"}) {
        int err = 0;
        vnet *c = Create(bad, 0, 0, &err);
        EXPECT_EQ(c, nullptr) << bad;
        EXPECT_EQ(err, VNET_EINVAL) << bad;
        vnet_destroy(c);
    }
    // Nothing was sent for any of them.
    Bytes b;
    EXPECT_FALSE(relay.TryRecv(&b));
}

TEST_F(RelayClient, EnteringARoomGivesAnAddressAndStatus) {
    Start();
    Enter(1, "K7M2QX");
    vnet_status_info st;
    ASSERT_EQ(vnet_status(v, &st), VNET_STATUS_READY);
    EXPECT_STREQ(st.room, "K7M2QX");
    EXPECT_EQ(Str(st.self), "10.0.0.1:0");
    EXPECT_EQ(st.error, 0);
    int s = vnet_open(v, 4000);
    vnet_addr la{};
    ASSERT_EQ(vnet_local_addr(v, s, &la), 0);
    EXPECT_EQ(Str(la), "10.0.0.1:4000");
}

TEST_F(RelayClient, NoAddressUntilInARoom) {
    Start();
    int s = vnet_open(v, 4000);  // opening is local and works at once
    ASSERT_GE(s, 0);
    vnet_addr la{};
    EXPECT_EQ(vnet_local_addr(v, s, &la), VNET_ENOTCONN);
    vnet_addr to = A("10.0.0.2:5");
    EXPECT_EQ(Send(v, s, "x", to), VNET_ENOTCONN);
    std::string got;
    EXPECT_EQ(Recv(v, s, &got), VNET_EAGAIN);
    vnet_stats stt;
    vnet_get_stats(v, &stt);
    EXPECT_EQ(stt.tx_datagrams, 0u);
    EXPECT_EQ(stt.wire_datagrams, 0u);
}

TEST_F(RelayClient, SendIsRefusedBeforeTheRoomEvenWithImpairment) {
    vnet_impair imp{};
    imp.latency_ms = 50;
    v = Create(nullptr, 0, 0, nullptr, &imp);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(relay.RecvType(RELAY_T_CREATE, &req));
    int s = vnet_open(v, 4000);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.2:5")), VNET_ENOTCONN);  // not silently queued
    vnet_stats stt;
    vnet_get_stats(v, &stt);
    EXPECT_EQ(stt.tx_datagrams, 0u);
}

TEST_F(RelayClient, IgnoresRoomRepliesToOtherRequests) {
    Start();
    Bytes other = req;
    other[5] ^= 0x55;  // a different nonce
    relay.Send(RoomFrame(other, 3, "ZZZZZZ"));
    relay.Send(ErrorFrame(other, RELAY_E_ROOM_FULL));
    // Then the real answer: it must still be accepted, and as ours.
    relay.Send(RoomFrame(req, 1, "ABCDEF"));
    ASSERT_TRUE(PollUntilState(VNET_STATUS_READY));
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_STREQ(st.room, "ABCDEF");
    EXPECT_EQ(Str(st.self), "10.0.0.1:0");
}

TEST_F(RelayClient, RetriesWithTheSameNonceOnABackoffSchedule) {
    Start();
    std::vector<Bytes> seen{req};
    // Due at +250, then +500 later (750), then +1000 (1750), then every 1000.
    struct Step { uint64_t t; int expect_new; };
    for (Step s : {Step{249, 0}, Step{250, 1}, Step{749, 0}, Step{750, 1}, Step{1749, 0},
                   Step{1750, 1}, Step{2749, 0}, Step{2750, 1}}) {
        clk.now = 1000 + s.t;
        vnet_poll(v);
        int got = 0;
        Bytes b;
        while (relay.TryRecv(&b)) {
            seen.push_back(b);
            got++;
        }
        EXPECT_EQ(got, s.expect_new) << "at +" << s.t << " ms";
    }
    ASSERT_EQ(seen.size(), 5u);
    for (size_t i = 1; i < seen.size(); i++)
        EXPECT_EQ(seen[i], seen[0]) << "retry " << i << " must repeat the same request, nonce included";

    // The answer to a retry (the first reply was "lost") completes the handshake.
    relay.Send(RoomFrame(seen.back(), 2, "QQQQQQ"));
    ASSERT_TRUE(PollUntilState(VNET_STATUS_READY));
    // No more retries once in a room.
    clk.now += 5000;
    vnet_poll(v);
    Bytes b;
    while (relay.TryRecv(&b)) EXPECT_NE(b[0], RELAY_T_CREATE);
}

TEST_F(RelayClient, DuplicateRoomRepliesAreHarmless) {
    Start();
    relay.Send(RoomFrame(req, 1, "ABCDEF", 8, 5, 0x10));
    ASSERT_TRUE(PollUntilState(VNET_STATUS_READY));
    // A late reply to a retry with a different token must not replace the session.
    relay.Send(RoomFrame(req, 4, "ABCDEF", 8, 5, 0x77));
    for (int i = 0; i < 3; i++) vnet_poll(v);
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_EQ(Str(st.self), "10.0.0.1:0");
    int s = vnet_open(v, 9);
    ASSERT_EQ(Send(v, s, "x", A("10.0.0.2:5")), 1);
    Bytes f;
    ASSERT_TRUE(relay.RecvType(RELAY_T_DATA, &f));
    EXPECT_EQ(f[1], 0x10);  // the original token
}

TEST_F(RelayClient, HandshakeTimesOut) {
    Start(nullptr, 0, 3000);
    clk.now += 2999;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_CONNECTING);
    clk.now += 1;
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ETIMEDOUT);
    // Failure is final, and sending says why not.
    int s = vnet_open(v, 1);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.1:1")), VNET_ENOTCONN);
}

TEST_F(RelayClient, DefaultHandshakeTimeoutIsTenSeconds) {
    Start();
    clk.now += 9999;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_CONNECTING);
    clk.now += 1;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_FAILED);
}

TEST_F(RelayClient, RelayErrorsBecomeFailedStatusWithTheRightReason) {
    struct Case { int reason; int expect; };
    for (Case c : {Case{RELAY_E_ROOM_NOT_FOUND, VNET_ENOROOM}, Case{RELAY_E_ROOM_FULL, VNET_EROOMFULL},
                   Case{RELAY_E_LIMIT, VNET_ELIMIT}, Case{RELAY_E_RATE, VNET_ELIMIT},
                   Case{RELAY_E_BAD_VERSION, VNET_EPROTO}, Case{RELAY_E_MALFORMED, VNET_EPROTO},
                   Case{RELAY_E_NONCE_REUSED, VNET_EPROTO}, Case{RELAY_E_CLOSED, VNET_ECLOSED},
                   Case{99, VNET_EPROTO}}) {
        vnet_destroy(v);
        Start("ABCDEF");
        relay.Send(ErrorFrame(req, c.reason));
        ASSERT_TRUE(PollUntilState(VNET_STATUS_FAILED)) << c.reason;
        vnet_status_info st;
        vnet_status(v, &st);
        EXPECT_EQ(st.error, c.expect) << "reason " << c.reason;
    }
}

TEST_F(RelayClient, MalformedRoomFramesAreIgnored) {
    Start();
    Bytes ok = RoomFrame(req, 1, "ABCDEF");
    Bytes short_room(ok.begin(), ok.end() - 1);
    Bytes long_room = ok;
    long_room.push_back(0);
    Bytes bad_ver = ok;
    bad_ver[1] = 9;
    Bytes foreign_net = ok;
    foreign_net[10] = 192;
    for (const Bytes &b : {short_room, long_room, bad_ver, Bytes{RELAY_T_ROOM}, Bytes{0x55, 1, 2}})
        relay.Send(b);
    for (int i = 0; i < 5; i++) vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_CONNECTING);
    // A ROOM that names an address outside 10.0.0.0/24 fails the session: the
    // relay is speaking something else.
    relay.Send(foreign_net);
    ASSERT_TRUE(PollUntilState(VNET_STATUS_FAILED));
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_EQ(st.error, VNET_EPROTO);
}

// -------------------------------------------------------------- data path

TEST_F(RelayClient, SendProducesADataFrameWithTokenAddressAndPorts) {
    Start();
    Enter(2, "ABCDEF");
    int s = vnet_open(v, 6000);
    ASSERT_GE(s, 0);
    vnet_addr to = A("10.0.0.1:7000");
    ASSERT_EQ(Send(v, s, "hello", to), 5);
    Bytes f;
    ASSERT_TRUE(relay.RecvType(RELAY_T_DATA, &f));
    ASSERT_EQ(f.size(), (size_t)(RELAY_DATA_HDR_UDP + 5));
    for (int i = 0; i < RELAY_TOKEN_LEN; i++) EXPECT_EQ(f[1 + i], 0xA0 + i) << "token byte " << i;
    size_t o = 1 + RELAY_TOKEN_LEN;
    EXPECT_EQ(Bytes(f.begin() + o, f.begin() + o + 4), B({10, 0, 0, 1}));
    EXPECT_EQ((f[o + 4] << 8) | f[o + 5], 7000);  // destination port
    EXPECT_EQ((f[o + 6] << 8) | f[o + 7], 6000);  // source port = the local socket
    EXPECT_EQ(std::string(f.begin() + o + 8, f.end()), "hello");
}

TEST_F(RelayClient, SendValidatesBeforeTheWire) {
    Start();
    Enter();
    int s = vnet_open(v, 6000);
    std::string big(VNET_MAX_DATAGRAM + 1, 'x');
    EXPECT_EQ(Send(v, s, big, A("10.0.0.2:1")), VNET_EMSGSIZE);
    EXPECT_EQ(Send(v, s, "x", A("[::1]:1")), VNET_EAFNOSUPPORT);
    EXPECT_EQ(Send(v, 77, "x", A("10.0.0.2:1")), VNET_EBADF);
    vnet_addr zero_port = A("10.0.0.2:0");
    EXPECT_EQ(Send(v, s, "x", zero_port), VNET_EINVAL);
    // The largest datagram and an empty one go through.
    EXPECT_EQ(Send(v, s, std::string(VNET_MAX_DATAGRAM, 'y'), A("10.0.0.2:1")), VNET_MAX_DATAGRAM);
    EXPECT_EQ(Send(v, s, "", A("10.0.0.2:1")), 0);
    Bytes f;
    ASSERT_TRUE(relay.RecvType(RELAY_T_DATA, &f));
    EXPECT_EQ(f.size(), (size_t)RELAY_MAX_FRAME);
    ASSERT_TRUE(relay.RecvType(RELAY_T_DATA, &f));
    EXPECT_EQ(f.size(), (size_t)RELAY_DATA_HDR_UDP);
    EXPECT_FALSE(relay.TryRecv(&f));  // nothing else (the rejected sends never reached the wire)
}

TEST_F(RelayClient, ReceivesDatagramsOnlyAfterPollAndByPort) {
    Start();
    Enter(1);
    int s1 = vnet_open(v, 4000), s2 = vnet_open(v, 4001);
    relay.Send(DataToClient(3, 4001, 9000, Str2Bytes("for-s2")));
    relay.Send(DataToClient(2, 4000, 9001, Str2Bytes("for-s1")));
    std::string got;
    EXPECT_EQ(Recv(v, s1, &got), VNET_EAGAIN);  // nothing is visible until a poll
    Bytes b;
    vnet_addr from{};
    ASSERT_TRUE(RecvOne(s2, &b, &from));
    EXPECT_EQ(std::string(b.begin(), b.end()), "for-s2");
    EXPECT_EQ(Str(from), "10.0.0.3:9000");
    ASSERT_TRUE(RecvOne(s1, &b, &from));
    EXPECT_EQ(std::string(b.begin(), b.end()), "for-s1");
    EXPECT_EQ(Str(from), "10.0.0.2:9001");
    EXPECT_EQ(Recv(v, s1, &got), VNET_EAGAIN);
}

TEST_F(RelayClient, DatagramsKeepTheirOrderAndAllBytes) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    for (int i = 0; i < 100; i++) {
        Bytes p(i % 50, (uint8_t)i);
        relay.Send(DataToClient(2, 4000, 1, p));
    }
    for (int i = 0; i < 100; i++) {
        Bytes b;
        ASSERT_TRUE(RecvOne(s, &b)) << i;
        EXPECT_EQ(b, Bytes(i % 50, (uint8_t)i)) << i;
    }
}

TEST_F(RelayClient, BadOrUnroutableDataIsDropped) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    relay.Send(DataToClient(2, 5555, 1, Str2Bytes("no-such-port")));
    relay.Send(DataToClient(2, 4000, 1, Bytes(VNET_MAX_DATAGRAM + 1, 1)));        // oversize payload
    relay.Send(Bytes{RELAY_T_DATA, 10, 0, 0, 2, 0x0f, 0xa0});                      // truncated header
    relay.Send(Bytes{RELAY_T_DATA, 192, 168, 0, 2, 0x0f, 0xa0, 0, 1, 'x'});        // not a room address
    relay.Send(DataToClient(2, 4000, 1, Str2Bytes("good")));
    Bytes b;
    ASSERT_TRUE(RecvOne(s, &b));
    EXPECT_EQ(std::string(b.begin(), b.end()), "good");  // the only one that got through
    vnet_stats st;
    vnet_get_stats(v, &st);
    EXPECT_EQ(st.unroutable, 1u);
    EXPECT_EQ(st.rx_oversize, 1u);
    std::string none;
    EXPECT_EQ(Recv(v, s, &none), VNET_EAGAIN);
}

TEST_F(RelayClient, OnlyTheRelaysAddressIsTrusted) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    // Frames carry no token on the way in, so the source address is the check.
    relay.SpoofFrom(DataToClient(2, 4000, 1, Str2Bytes("injected")));
    relay.SpoofFrom(ErrorFrame(Bytes(), RELAY_E_NO_SESSION));
    relay.Send(DataToClient(2, 4000, 1, Str2Bytes("real")));
    Bytes b;
    ASSERT_TRUE(RecvOne(s, &b));
    EXPECT_EQ(std::string(b.begin(), b.end()), "real");
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);  // the forged ERROR did nothing
    std::string none;
    EXPECT_EQ(Recv(v, s, &none), VNET_EAGAIN);
}

TEST_F(RelayClient, SmallReceiveBufferConsumesTheDatagram) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    relay.Send(DataToClient(2, 4000, 1, Str2Bytes("0123456789")));
    relay.Send(DataToClient(2, 4000, 1, Str2Bytes("next")));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    char small[4];
    int rc;
    do {
        vnet_poll(v);
        rc = vnet_recvfrom(v, s, small, sizeof small, nullptr);
    } while (rc == VNET_EAGAIN && std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(rc, VNET_EMSGSIZE);
    Bytes b;
    ASSERT_TRUE(RecvOne(s, &b));
    EXPECT_EQ(std::string(b.begin(), b.end()), "next");
}

TEST_F(RelayClient, ReceiveQueueOverflowDropsLikeUdp) {
    addr = relay.addr();
    vnet_config cfg = RelayUdpConfig(relay, &clk);
    cfg.relay = addr.c_str();
    cfg.rx_queue_len = 4;
    v = vnet_create(&cfg, nullptr);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(relay.RecvType(RELAY_T_CREATE, &req));
    Enter(1);
    int s = vnet_open(v, 4000);
    for (int i = 0; i < 10; i++) relay.Send(DataToClient(2, 4000, 1, Bytes{(uint8_t)i}));
    relay.Send(DataToClient(2, 4001, 1, Bytes{0}));  // marker: an unroutable one at the end
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    vnet_stats st;
    do {
        vnet_poll(v);
        vnet_get_stats(v, &st);
    } while (st.unroutable < 1 && std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(st.rx_overflow, 6u);
    for (int i = 0; i < 4; i++) {
        Bytes b;
        ASSERT_TRUE(RecvOne(s, &b));
        EXPECT_EQ(b, Bytes{(uint8_t)i});
    }
}

TEST_F(RelayClient, SocketPortsAreLocalAndUnique) {
    Start();
    int a = vnet_open(v, 0), b = vnet_open(v, 0);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    Enter(1);
    vnet_addr la{}, lb{};
    vnet_local_addr(v, a, &la);
    vnet_local_addr(v, b, &lb);
    EXPECT_NE(la.port, lb.port);
    EXPECT_GE(la.port, 49152);
    EXPECT_EQ(vnet_open(v, la.port), VNET_EADDRINUSE);
    vnet_close(v, a);
    EXPECT_EQ(vnet_open(v, la.port), a);  // reusable after close
}

// ---------------------------------------------------- keepalive and loss

TEST_F(RelayClient, KeepaliveFollowsTheRelaysSuggestionUnlessOverridden) {
    Start();
    Enter(1, "ABCDEF", 8, 2);  // the relay suggests 2 s
    Bytes b;
    clk.now += 1999;
    vnet_poll(v);
    EXPECT_FALSE(relay.TryRecv(&b));
    clk.now += 1;
    vnet_poll(v);
    ASSERT_TRUE(relay.RecvType(RELAY_T_KEEPALIVE, &b));
    ASSERT_EQ(b.size(), (size_t)(1 + RELAY_TOKEN_LEN));
    for (int i = 0; i < RELAY_TOKEN_LEN; i++) EXPECT_EQ(b[1 + i], 0xA0 + i);
    relay.Send(Bytes{RELAY_T_KEEPALIVE});  // the relay answers: it is alive
    vnet_poll(v);
    clk.now += 2000;
    vnet_poll(v);
    ASSERT_TRUE(relay.RecvType(RELAY_T_KEEPALIVE, &b));
}

TEST_F(RelayClient, ConfiguredKeepaliveWins) {
    Start(nullptr, 700);
    Enter(1, "ABCDEF", 8, 5);
    Bytes b;
    clk.now += 699;
    vnet_poll(v);
    EXPECT_FALSE(relay.TryRecv(&b));
    clk.now += 1;
    vnet_poll(v);
    EXPECT_TRUE(relay.RecvType(RELAY_T_KEEPALIVE, &b));
}

TEST_F(RelayClient, SilentRelayEndsTheSessionAfterThreeIntervals) {
    Start();
    Enter(1);  // keepalive 5 s: lost after 15 s of silence
    clk.now += 15000;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
    clk.now += 1;
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ETIMEDOUT);
    int s = vnet_open(v, 3);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.2:1")), VNET_ENOTCONN);
}

TEST_F(RelayClient, AnyFrameFromTheRelayCountsAsLife) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    Bytes b;
    // Data alone, with no keepalive answers, keeps the session up.
    for (int i = 0; i < 10; i++) {
        clk.now += 10000;
        relay.Send(DataToClient(2, 4000, 1, Bytes{1}));
        ASSERT_TRUE(RecvOne(s, &b));
    }
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
    // Keepalive answers too.
    for (int i = 0; i < 10; i++) {
        clk.now += 10000;
        relay.Send(Bytes{RELAY_T_KEEPALIVE});
        vnet_poll(v);
    }
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
}

TEST_F(RelayClient, PollingLateDoesNotKillASessionThatHasPendingFrames) {
    // A game that stops polling for 20 s (a long load, a hidden tab) and
    // resumes: frames waiting in the socket are read before the timers run.
    Start();
    Enter(1);
    clk.now += 20000;
    relay.Send(Bytes{RELAY_T_KEEPALIVE});
    // One poll must be enough: it reads first, then judges.
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
}

TEST_F(RelayClient, RelayForgettingTheSessionEndsIt) {
    Start();
    Enter(1);
    relay.Send(ErrorFrame(Bytes(), RELAY_E_NO_SESSION));  // e.g. the relay restarted
    ASSERT_TRUE(PollUntilState(VNET_STATUS_FAILED));
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_EQ(st.error, VNET_ECLOSED);
}

TEST_F(RelayClient, EvictionAndRoomClosureEndTheSession) {
    for (int reason : {RELAY_E_TIMEOUT, RELAY_E_CLOSED}) {
        vnet_destroy(v);
        Start();
        Enter(1);
        relay.Send(ErrorFrame(Bytes(), reason));
        ASSERT_TRUE(PollUntilState(VNET_STATUS_FAILED)) << reason;
        vnet_status_info st;
        vnet_status(v, &st);
        EXPECT_EQ(st.error, VNET_ECLOSED);
    }
}

TEST_F(RelayClient, UnrelatedErrorsInARoomAreIgnored) {
    Start();
    Enter(1);
    for (int reason : {RELAY_E_ROOM_FULL, RELAY_E_RATE, RELAY_E_MALFORMED, RELAY_E_BAD_VERSION})
        relay.Send(ErrorFrame(req, reason));
    for (int i = 0; i < 5; i++) vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
}

TEST_F(RelayClient, DestroyLeavesTheRoom) {
    Start();
    Enter(1);
    vnet_destroy(v);
    v = nullptr;
    Bytes b;
    ASSERT_TRUE(relay.RecvType(RELAY_T_LEAVE, &b));
    ASSERT_EQ(b.size(), (size_t)(1 + RELAY_TOKEN_LEN));
    for (int i = 0; i < RELAY_TOKEN_LEN; i++) EXPECT_EQ(b[1 + i], 0xA0 + i);
}

TEST_F(RelayClient, DestroyBeforeTheRoomSendsNoLeave) {
    Start();
    vnet_destroy(v);
    v = nullptr;
    Bytes b;
    EXPECT_FALSE(relay.TryRecv(&b));
}

// ---------------------------------------------------------------- the rest

TEST_F(RelayClient, ResolveRoomHostWorksBeforeTheRoomIsEntered) {
    Start();
    vnet_addr a{};
    ASSERT_EQ(vnet_resolve(v, "room-host:5000", &a), 0);
    EXPECT_EQ(Str(a), "10.0.0.1:5000");
    EXPECT_EQ(vnet_resolve(v, "room-host:0", &a), 0);
    EXPECT_EQ(a.port, 0);
    for (const char *bad : {"room-host:", "room-host:70000", "room-host:12x", "room-host", "room-host:-1"}) {
        EXPECT_EQ(vnet_resolve(v, bad, &a), VNET_EINVAL) << bad;
    }
    // Everything else still goes through the normal parser.
    ASSERT_EQ(vnet_resolve(v, "10.0.0.3:99", &a), 0);
    EXPECT_EQ(Str(a), "10.0.0.3:99");
    ASSERT_EQ(vnet_resolve(v, "[::1]:7", &a), 0);
    // Other backends do not know "room-host".
    EXPECT_EQ(vnet_resolve(nullptr, "room-host:5000", &a), VNET_EINVAL);
}

TEST_F(RelayClient, ImpairmentAppliesToGameDatagramsOnly) {
    vnet_impair imp{};
    imp.latency_ms = 100;
    v = Create(nullptr, 0, 0, nullptr, &imp);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(relay.RecvType(RELAY_T_CREATE, &req));
    Enter(1);  // the handshake was not delayed
    int s = vnet_open(v, 4000);
    ASSERT_EQ(Send(v, s, "late", A("10.0.0.2:1")), 4);
    vnet_poll(v);
    Bytes b;
    EXPECT_FALSE(relay.TryRecv(&b));  // held back by the 100 ms latency
    clk.now += 100;
    vnet_poll(v);
    ASSERT_TRUE(relay.RecvType(RELAY_T_DATA, &b));
}

TEST_F(RelayClient, AHostNameIsResolvedWithTheSystemResolver) {
    // Not "localhost" (vnet_resolve handles that itself): another spelling that
    // only getaddrinfo() understands, and that /etc/hosts always has.
    addr = "LocalHost:" + std::to_string(relay.port());
    vnet_config cfg = RelayUdpConfig(relay, &clk);
    cfg.relay = addr.c_str();
    cfg.family = VNET_FAMILY_IPV4;
    int err = 0;
    v = vnet_create(&cfg, &err);
    ASSERT_NE(v, nullptr) << vnet_strerror(err);
    EXPECT_TRUE(relay.RecvType(RELAY_T_CREATE, &req));
}

TEST_F(RelayClient, BadRelayAddressesFailCreate) {
    for (const char *bad : {"", "nohostport", "1.2.3.4", "1.2.3.4:0", "1.2.3.4:99999", ":5000"}) {
        vnet_config cfg;
        vnet_config_init(&cfg);
        cfg.backend = VNET_BACKEND_RELAY_UDP;
        cfg.relay = bad;
        cfg.ignore_env = 1;
        int err = 0;
        vnet *c = vnet_create(&cfg, &err);
        EXPECT_EQ(c, nullptr) << bad;
        EXPECT_EQ(err, VNET_EINVAL) << "\"" << bad << "\"";
    }
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_UDP;
    cfg.ignore_env = 1;
    int err = 0;
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);  // no relay at all
    EXPECT_EQ(err, VNET_EINVAL);
}

TEST_F(RelayClient, WebSocketBackendIsUnavailableOnNativeBuilds) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_WS;
    cfg.relay = "ws://127.0.0.1:1/vnet";
    cfg.ignore_env = 1;
    int err = 0;
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);
    EXPECT_EQ(err, VNET_ENOTSUP);
}

TEST_F(RelayClient, StatusIsGenericForEveryBackend) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *lb = LoopbackHost(net);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(lb, &st), VNET_STATUS_READY);
    EXPECT_STREQ(st.transport, "loopback");
    EXPECT_EQ(st.error, 0);
    EXPECT_STREQ(st.room, "");
    vnet_destroy(lb);
    vnet *u = UdpHost();
    EXPECT_EQ(vnet_status(u, &st), VNET_STATUS_READY);
    EXPECT_STREQ(st.transport, "udp");
    vnet_destroy(u);
    vnet_loopback_network_destroy(net);
    EXPECT_EQ(vnet_status(nullptr, &st), VNET_EINVAL);
}

#if defined(VNET_TEST_COUNT_ALLOCS)
TEST_F(RelayClient, SteadyStateDoesNotAllocate) {
    Start();
    Enter(1);
    int s = vnet_open(v, 4000);
    ASSERT_GE(s, 0);
    vnet_addr to = A("10.0.0.2:5");
    Bytes b;
    // Warm up, then count heap calls over many send/receive/poll rounds.
    for (int i = 0; i < 10; i++) {
        Send(v, s, "warm", to);
        relay.Send(DataToClient(2, 4000, 1, Str2Bytes("warm")));
        ASSERT_TRUE(RecvOne(s, &b));
    }
    relay.Drain();
    unsigned long before = g_alloc_calls;
    for (int i = 0; i < 500; i++) {
        Send(v, s, "payload", to);
        vnet_poll(v);
        char buf[VNET_MAX_DATAGRAM];
        vnet_recvfrom(v, s, buf, sizeof buf, nullptr);
    }
    EXPECT_EQ(g_alloc_calls, before);
}
#endif
