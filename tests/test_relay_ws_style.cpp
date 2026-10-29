// SPDX-License-Identifier: MIT
// The relay client core with a WebSocket-style transport (no token in frames,
// unpadded and un-retried handshake, asynchronous connect). The real
// WebSocket transport only exists in browser builds, so this drives the same
// core through a scripted in-memory transport; the browser test covers the
// real thing.
#include "relay_test_util.h"

#include <deque>

namespace {

struct ScriptTp {
    int state = RELAY_TP_CONNECTING;
    std::deque<Bytes> in;
    std::vector<Bytes> out;
    bool destroyed = false;
    int send_rc = 0;
};

int TpState(void *p) { return static_cast<ScriptTp *>(p)->state; }
int TpSend(void *p, const uint8_t *f, size_t n) {
    auto *t = static_cast<ScriptTp *>(p);
    if (t->send_rc < 0) return t->send_rc;
    t->out.emplace_back(f, f + n);
    return 0;
}
int TpRecv(void *p, uint8_t *buf, size_t cap) {
    auto *t = static_cast<ScriptTp *>(p);
    if (t->in.empty()) return VNET_EAGAIN;
    Bytes b = t->in.front();
    t->in.pop_front();
    if (b.size() > cap) return VNET_EMSGSIZE;
    memcpy(buf, b.data(), b.size());
    return (int)b.size();
}
void TpDestroy(void *p) { static_cast<ScriptTp *>(p)->destroyed = true; }

const relay_transport_ops kOps = {"relay-ws", 0, TpState, TpSend, TpRecv, TpDestroy, nullptr, nullptr};

class RelayWsStyle : public VnetTest {
protected:
    void TearDown() override { vnet_destroy(v); }
    vnet *Make(const char *room = nullptr, int timeout_ms = 0, int *err = nullptr) {
        vnet_config cfg;
        vnet_config_init(&cfg);
        cfg.backend = VNET_BACKEND_RELAY_WS;
        cfg.room = room;
        cfg.ignore_env = 1;
        cfg.clock = TestClockFn;
        cfg.clock_user = &clk;
        cfg.relay_timeout_ms = timeout_ms;
        return vnet__create_with_relay_transport(&cfg, &kOps, &tp, err);
    }
    void Open() {
        tp.state = RELAY_TP_OPEN;
        vnet_poll(v);
    }
    void Enter(int octet = 1) {
        tp.in.push_back(RoomFrame(tp.out.at(0), octet, "WXYZ23"));
        vnet_poll(v);
        ASSERT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
    }
    ScriptTp tp;
    TestClockState clk;
    vnet *v = nullptr;
};

TEST_F(RelayWsStyle, NothingIsSentUntilTheTransportIsOpen) {
    v = Make();
    ASSERT_NE(v, nullptr);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_CONNECTING);
    EXPECT_STREQ(st.transport, "relay-ws");
    for (int i = 0; i < 5; i++) {
        clk.now += 500;
        vnet_poll(v);
    }
    EXPECT_TRUE(tp.out.empty());
    Open();
    ASSERT_EQ(tp.out.size(), 1u);
}

TEST_F(RelayWsStyle, HandshakeIsUnpaddedAndSentOnce) {
    v = Make();
    Open();
    ASSERT_EQ(tp.out.size(), 1u);
    EXPECT_EQ(tp.out[0].size(), (size_t)(1 + 1 + RELAY_NONCE_LEN));  // WebSocket is reliable: no padding
    EXPECT_EQ(tp.out[0][0], RELAY_T_CREATE);
    for (int i = 0; i < 20; i++) {
        clk.now += 400;
        vnet_poll(v);
    }
    EXPECT_EQ(tp.out.size(), 1u) << "no retransmission over a reliable transport";
}

TEST_F(RelayWsStyle, NeverUsesTheStatelessRetry) {
    // A WebSocket proved its address with the TCP handshake: no cookie field,
    // and a RETRY (which a WebSocket relay never sends) changes nothing.
    v = Make();
    Open();
    ASSERT_EQ(tp.out.size(), 1u);
    Bytes retry{RELAY_T_RETRY, RELAY_PROTO_VERSION};
    retry.insert(retry.end(), tp.out[0].begin() + 2, tp.out[0].begin() + 10);
    retry.insert(retry.end(), 16, 0x33);
    tp.in.push_back(retry);
    vnet_poll(v);
    EXPECT_EQ(tp.out.size(), 1u);
}

TEST_F(RelayWsStyle, JoinFrame) {
    v = Make("k7m2qx");
    Open();
    ASSERT_EQ(tp.out.size(), 1u);
    ASSERT_EQ(tp.out[0].size(), (size_t)(1 + 1 + RELAY_NONCE_LEN + RELAY_CODE_LEN));
    EXPECT_EQ(tp.out[0][0], RELAY_T_JOIN);
    EXPECT_EQ(std::string(tp.out[0].begin() + 10, tp.out[0].end()), "K7M2QX");
}

TEST_F(RelayWsStyle, FramesCarryNoToken) {
    v = Make();
    Open();
    Enter(1);
    int s = vnet_open(v, 4000);
    ASSERT_EQ(Send(v, s, "abc", A("10.0.0.2:5000")), 3);
    ASSERT_EQ(tp.out.size(), 2u);
    Bytes want = B({RELAY_T_DATA, 10, 0, 0, 2, 0x13, 0x88, 0x0f, 0xa0, 'a', 'b', 'c'});
    EXPECT_EQ(tp.out[1], want);
    clk.now += 5000;
    vnet_poll(v);
    ASSERT_EQ(tp.out.size(), 3u);
    EXPECT_EQ(tp.out[2], B({RELAY_T_KEEPALIVE}));
    vnet_destroy(v);
    v = nullptr;
    ASSERT_EQ(tp.out.size(), 4u);
    EXPECT_EQ(tp.out[3], B({RELAY_T_LEAVE}));
    EXPECT_TRUE(tp.destroyed);
}

TEST_F(RelayWsStyle, ReceivesDataLikeTheUdpStyle) {
    v = Make();
    Open();
    Enter(1);
    int s = vnet_open(v, 4000);
    tp.in.push_back(DataToClient(2, 4000, 7, Str2Bytes("hi")));
    vnet_poll(v);
    std::string got;
    vnet_addr from{};
    ASSERT_EQ(Recv(v, s, &got, &from), 2);
    EXPECT_EQ(got, "hi");
    EXPECT_EQ(Str(from), "10.0.0.2:7");
}

TEST_F(RelayWsStyle, ConnectTimeout) {
    v = Make(nullptr, 4000);
    clk.now += 3999;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_CONNECTING);
    clk.now += 1;
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ETIMEDOUT);
    EXPECT_TRUE(tp.out.empty());
}

TEST_F(RelayWsStyle, ASuspendedPageWhileConnectingStillConnects) {
    // A tab hidden for 60 s while the WebSocket was still opening.
    v = Make(nullptr, 10000);
    clk.now += 60000;
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_CONNECTING);
    Open();
    Enter(1);
}

TEST_F(RelayWsStyle, ASuspendedPageInARoomStaysInItAndAsksTheRelayAtOnce) {
    v = Make();
    Open();
    Enter(1);
    ASSERT_EQ(tp.out.size(), 1u);
    clk.now += 60000;  // no polls for a minute; the relay's pings were answered by the browser
    vnet_poll(v);
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
    ASSERT_EQ(tp.out.size(), 2u);
    EXPECT_EQ(tp.out[1], B({RELAY_T_KEEPALIVE}));
    // The answer is a sample of the round trip.
    clk.now += 44;
    tp.in.push_back(B({RELAY_T_KEEPALIVE}));
    vnet_poll(v);
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_EQ(st.relay_rtt_ms, 44);
    EXPECT_EQ(tp.out.size(), 2u);
}

TEST_F(RelayWsStyle, ConnectFailureMeansTheRelayCannotBeReached) {
    v = Make();
    tp.state = RELAY_TP_FAILED;
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECONNREFUSED);
}

TEST_F(RelayWsStyle, LosingTheConnectionInARoomEndsTheSession) {
    v = Make();
    Open();
    Enter(1);
    tp.state = RELAY_TP_FAILED;
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECLOSED);
    int s = vnet_open(v, 1);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.2:1")), VNET_ENOTCONN);
}

TEST_F(RelayWsStyle, BackpressureDropsADatagramInsteadOfFailing) {
    v = Make();
    Open();
    Enter(1);
    int s = vnet_open(v, 4000);
    tp.send_rc = VNET_EAGAIN;
    vnet_stats before, after;
    vnet_get_stats(v, &before);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.2:1")), VNET_EAGAIN);
    vnet_get_stats(v, &after);
    EXPECT_EQ(after.wire_datagrams, before.wire_datagrams);  // not counted as sent
    EXPECT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);   // and not a failure
    tp.send_rc = 0;
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.2:1")), 1);
}

TEST_F(RelayWsStyle, TransportIsOwnedByTheContext) {
    int err = 0;
    v = Make("bad!!!", 0, &err);  // rejected: the transport must still be released
    EXPECT_EQ(v, nullptr);
    EXPECT_EQ(err, VNET_EINVAL);
    EXPECT_TRUE(tp.destroyed);

    ScriptTp t2;
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_WS;
    cfg.ignore_env = 1;
    cfg.impair.loss_pct = 150;  // invalid impairment: rejected before the backend is built
    EXPECT_EQ(vnet__create_with_relay_transport(&cfg, &kOps, &t2, &err), nullptr);
    EXPECT_EQ(err, VNET_EINVAL);
    EXPECT_TRUE(t2.destroyed);

    v = Make();
    ASSERT_NE(v, nullptr);
    tp.destroyed = false;
    vnet_destroy(v);
    v = nullptr;
    EXPECT_TRUE(tp.destroyed);
}

}  // namespace
