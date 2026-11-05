// SPDX-License-Identifier: MIT
// The receive queue of the WebSocket transport (src/relay_inbox.c): separate
// rings for DATA and control frames, so that a burst of datagrams cannot push
// out a KEEPALIVE answer or an ERROR. The browser-only relay_ws.c feeds frames
// into this module from its message callback; here the same module is fed by a
// scripted transport and run through the relay client core.
#include "relay_test_util.h"

extern "C" {
#include "relay_inbox.h"
}

namespace {

Bytes Data(int tag) {
    Bytes f = DataToClient(2, 4000, 1, Bytes{(uint8_t)tag});
    return f;
}

struct InboxTp {
    relay_inbox *q = relay_inbox_create(256, 64);
    std::vector<Bytes> out;
    int state = RELAY_TP_OPEN;
    int overflow = 0;
    // What relay_ws.c's on_message does.
    void Message(const Bytes &f) {
        if (relay_inbox_push(q, f.data(), f.size()) == RELAY_INBOX_OVERFLOW) state = RELAY_TP_FAILED, overflow++;
    }
    ~InboxTp() { relay_inbox_destroy(q); }
};
int TpState(void *p) { return static_cast<InboxTp *>(p)->state; }
int TpSend(void *p, const uint8_t *f, size_t n) {
    static_cast<InboxTp *>(p)->out.emplace_back(f, f + n);
    return 0;
}
int TpRecv(void *p, uint8_t *buf, size_t cap) { return relay_inbox_pop(static_cast<InboxTp *>(p)->q, buf, cap); }
void TpDestroy(void *) {}
const relay_transport_ops kOps = {"relay-ws", 0, TpState, TpSend, TpRecv, TpDestroy, nullptr, nullptr};

}  // namespace

TEST(RelayInbox, DatagramsOverflowAndAreDroppedInOrder) {
    relay_inbox *q = relay_inbox_create(4, 2);
    for (int i = 0; i < 6; i++) {
        Bytes f = Data(i);
        EXPECT_EQ(relay_inbox_push(q, f.data(), f.size()), i < 4 ? RELAY_INBOX_DATA : RELAY_INBOX_DROPPED) << i;
    }
    uint8_t buf[RELAY_MAX_FRAME];
    for (int i = 0; i < 4; i++) {
        int n = relay_inbox_pop(q, buf, sizeof buf);
        ASSERT_EQ(n, (int)Data(i).size());
        EXPECT_EQ(buf[n - 1], i);
    }
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), VNET_EAGAIN);
    relay_inbox_destroy(q);
}

TEST(RelayInbox, ABurstOfDatagramsNeverPushesOutAControlFrame) {
    relay_inbox *q = relay_inbox_create(8, 4);
    Bytes ka{RELAY_T_KEEPALIVE};
    Bytes err = ErrorFrame(Bytes(), RELAY_E_CLOSED);
    EXPECT_EQ(relay_inbox_push(q, ka.data(), ka.size()), RELAY_INBOX_CONTROL);
    for (int i = 0; i < 100; i++) {
        Bytes f = Data(i);
        relay_inbox_push(q, f.data(), f.size());
    }
    EXPECT_EQ(relay_inbox_data_count(q), 8);
    // The ring is full of datagrams, and a control frame still gets in.
    EXPECT_EQ(relay_inbox_push(q, err.data(), err.size()), RELAY_INBOX_CONTROL);
    EXPECT_EQ(relay_inbox_control_count(q), 2);
    // Control frames come out first, in order, then the datagrams.
    uint8_t buf[RELAY_MAX_FRAME];
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), 1);
    EXPECT_EQ(buf[0], RELAY_T_KEEPALIVE);
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), (int)err.size());
    EXPECT_EQ(buf[0], RELAY_T_ERROR);
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), (int)Data(0).size());
    EXPECT_EQ(buf[0], RELAY_T_DATA);
    relay_inbox_destroy(q);
}

TEST(RelayInbox, TooManyControlFramesIsAnOverflowAndBadFramesAreDropped) {
    relay_inbox *q = relay_inbox_create(4, 2);
    Bytes ka{RELAY_T_KEEPALIVE};
    EXPECT_EQ(relay_inbox_push(q, ka.data(), 1), RELAY_INBOX_CONTROL);
    EXPECT_EQ(relay_inbox_push(q, ka.data(), 1), RELAY_INBOX_CONTROL);
    EXPECT_EQ(relay_inbox_push(q, ka.data(), 1), RELAY_INBOX_OVERFLOW);
    Bytes long_ctl(RELAY_WT_CTL_MAX + 1, 0);
    long_ctl[0] = RELAY_T_ROOM;
    relay_inbox *q2 = relay_inbox_create(4, 2);
    EXPECT_EQ(relay_inbox_push(q2, long_ctl.data(), long_ctl.size()), RELAY_INBOX_DROPPED) << "no control frame is that long";
    Bytes huge(RELAY_MAX_FRAME + 1, 0);
    huge[0] = RELAY_T_DATA;
    EXPECT_EQ(relay_inbox_push(q2, huge.data(), huge.size()), RELAY_INBOX_DROPPED);
    EXPECT_EQ(relay_inbox_push(q2, ka.data(), 0), RELAY_INBOX_DROPPED);
    relay_inbox_destroy(q2);
    relay_inbox_destroy(q);
}

TEST(RelayInbox, ASmallBufferConsumesTheFrame) {
    relay_inbox *q = relay_inbox_create(4, 2);
    Bytes f = Data(1);
    relay_inbox_push(q, f.data(), f.size());
    uint8_t buf[4];
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), VNET_EMSGSIZE);
    EXPECT_EQ(relay_inbox_pop(q, buf, sizeof buf), VNET_EAGAIN);
    relay_inbox_destroy(q);
}

// The relay client core on top of it: an unpolled page that took a burst of
// 1000 datagrams still hears that the relay ended its session.
class InboxCore : public VnetTest {
protected:
    void TearDown() override { vnet_destroy(v); }
    void SetUp() override {
        vnet_config cfg;
        vnet_config_init(&cfg);
        cfg.backend = VNET_BACKEND_RELAY_WS;
        cfg.ignore_env = 1;
        cfg.clock = TestClockFn;
        cfg.clock_user = &clk;
        v = vnet__create_with_relay_transport(&cfg, &kOps, &tp, nullptr);
        ASSERT_NE(v, nullptr);
        vnet_poll(v);
        ASSERT_FALSE(tp.out.empty());
        tp.Message(RoomFrame(tp.out[0], 1, "WXYZ23"));
        vnet_poll(v);
        ASSERT_EQ(vnet_status(v, nullptr), VNET_STATUS_READY);
        sock = vnet_open(v, 4000);
    }
    InboxTp tp;
    TestClockState clk;
    vnet *v = nullptr;
    int sock = -1;
};

TEST_F(InboxCore, ABurstBeforeTheErrorStillLetsTheErrorThrough) {
    for (int i = 0; i < 1000; i++) tp.Message(Data(i % 200));
    tp.Message(ErrorFrame(Bytes(), RELAY_E_CLOSED));
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECLOSED);
}

TEST_F(InboxCore, ABurstBeforeTheKeepaliveAnswerStillCountsAsLifeAndGivesTheRtt) {
    clk.now += 5000;
    vnet_poll(v);  // the client's KEEPALIVE
    ASSERT_EQ(tp.out.back(), B({RELAY_T_KEEPALIVE}));
    clk.now += 25;
    for (int i = 0; i < 1000; i++) tp.Message(Data(i % 200));
    tp.Message(Bytes{RELAY_T_KEEPALIVE});
    vnet_poll(v);
    vnet_status_info st;
    vnet_status(v, &st);
    EXPECT_EQ(st.relay_rtt_ms, 25);
    int got = 0;
    char buf[VNET_MAX_DATAGRAM];
    while (vnet_recvfrom(v, sock, buf, sizeof buf, nullptr) >= 0) got++;
    EXPECT_EQ(got, 128) << "the rest of the burst is a lossy network: the socket queue (128) took what fit";
}

TEST_F(InboxCore, ControlFramesPilingUpMeansTheTransportFails) {
    // 64 control frames nobody has polled: the page has been away far too long.
    for (int i = 0; i < 70; i++) tp.Message(Bytes{RELAY_T_KEEPALIVE});
    EXPECT_GT(tp.overflow, 0);
    vnet_poll(v);
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECLOSED);
}
