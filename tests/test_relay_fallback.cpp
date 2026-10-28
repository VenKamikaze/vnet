// SPDX-License-Identifier: MIT
// VNET_BACKEND_RELAY_AUTO and the WebTransport transport's datagram-size
// check, without a browser: the relay client core runs over two scripted
// in-memory transports (a "relay-wt" first, a "relay-ws" fallback). The real
// browser transports are covered by tests/browser/wt_browser.test.js.
#include "relay_test_util.h"

#include <deque>

namespace {

struct ScriptTp {
    explicit ScriptTp(const char *n) : name(n) {}
    const char *name;
    int state = RELAY_TP_CONNECTING;
    std::deque<Bytes> in;
    std::vector<Bytes> out;
    bool destroyed = false;
    size_t max_frame = 1200;  // what the transport promises to carry
};

int TpState(void *p) { return static_cast<ScriptTp *>(p)->state; }
int TpSend(void *p, const uint8_t *f, size_t n) {
    static_cast<ScriptTp *>(p)->out.emplace_back(f, f + n);
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
size_t TpMaxFrame(void *p) { return static_cast<ScriptTp *>(p)->max_frame; }

const relay_transport_ops kWtOps = {"relay-wt", 0, TpState, TpSend, TpRecv, TpDestroy, TpMaxFrame};
const relay_transport_ops kWsOps = {"relay-ws", 0, TpState, TpSend, TpRecv, TpDestroy, nullptr};

// What the core is told to fall back to: a way to open the second transport.
struct Fallback {
    ScriptTp *tp = nullptr;
    int rc = 0;     // what opening it returns
    int calls = 0;  // how often it was asked
};

int OpenFallback(vnet *, const void *arg, const relay_transport_ops **ops, void **tp) {
    Fallback *f = *static_cast<Fallback *const *>(arg);  // the core copied the pointer's bytes
    f->calls++;
    if (f->rc != 0) return f->rc;
    *ops = &kWsOps;
    *tp = f->tp;
    return 0;
}

constexpr unsigned kPrimaryTimeout = 3000;
constexpr size_t kFull = RELAY_WT_DATA_MAX;  // 1109

class RelayFallback : public VnetTest {
protected:
    void TearDown() override { vnet_destroy(v); }
    vnet *Make(bool with_fallback = true, const char *room = nullptr, int keepalive_ms = 0) {
        vnet_config cfg;
        vnet_config_init(&cfg);
        cfg.backend = VNET_BACKEND_RELAY_AUTO;
        cfg.room = room;
        cfg.ignore_env = 1;
        cfg.clock = TestClockFn;
        cfg.clock_user = &clk;
        cfg.keepalive_ms = keepalive_ms;
        Fallback *fp = &fb;
        int err = 0;
        if (with_fallback)
            v = vnet__create_with_relay_fallback(&cfg, &kWtOps, &wt, kPrimaryTimeout, OpenFallback, &fp, sizeof fp,
                                                 &err);
        else
            v = vnet__create_with_relay_transport(&cfg, &kWtOps, &wt, &err);
        EXPECT_EQ(err, 0);
        return v;
    }
    void Poll(uint64_t advance_ms = 0) {
        clk.now += advance_ms;
        vnet_poll(v);
    }
    vnet_status_info Status() {
        vnet_status_info st;
        vnet_status(v, &st);
        return st;
    }
    static Bytes Nonce(const ScriptTp &t, size_t frame = 0) {
        return Bytes(t.out.at(frame).begin() + 2, t.out.at(frame).begin() + 2 + RELAY_NONCE_LEN);
    }
    // The room, entered over `t`.
    void Enter(ScriptTp &t, int octet = 1, int keepalive_s = 5) {
        t.in.push_back(RoomFrame(t.out.at(0), octet, "WXYZ23", 8, keepalive_s));
        Poll();
    }
    // The echo of the probe a transport has sent: the relay delivers a member's datagram to itself.
    static Bytes Echo(const Bytes &probe, int octet) {
        Bytes e = probe;  // type, dst(4), dport, sport, payload  ->  type, src(4), ...
        e[4] = (uint8_t)octet;
        return e;
    }

    ScriptTp wt{"relay-wt"}, ws{"relay-ws"};
    Fallback fb{&ws, 0, 0};
    TestClockState clk;
    vnet *v = nullptr;
};

TEST_F(RelayFallback, TheFirstTransportIsUsedWhenItWorks) {
    Make();
    wt.state = RELAY_TP_OPEN;
    Poll();
    ASSERT_EQ(wt.out.size(), 1u);
    EXPECT_STREQ(Status().transport, "relay-wt");
    Enter(wt);
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_READY);
    EXPECT_STREQ(st.transport, "relay-wt");
    EXPECT_EQ(fb.calls, 0) << "the fallback must not even be opened";
    EXPECT_FALSE(wt.destroyed);
    // Long after the fallback window nothing changes.
    for (int i = 0; i < 10; i++) Poll(1000);
    EXPECT_EQ(Status().state, VNET_STATUS_READY);
    EXPECT_EQ(fb.calls, 0);
    vnet_destroy(v);
    v = nullptr;
    EXPECT_TRUE(wt.destroyed);
    EXPECT_EQ(wt.out.back(), B({RELAY_T_LEAVE}));
}

TEST_F(RelayFallback, AFailedTransportIsReplacedAndTheHandshakeStartsOverWithANewNonce) {
    Make();
    wt.state = RELAY_TP_OPEN;
    Poll();
    ASSERT_EQ(wt.out.size(), 1u);
    Bytes first = Nonce(wt);
    wt.state = RELAY_TP_FAILED;
    Poll();
    EXPECT_EQ(fb.calls, 1);
    EXPECT_TRUE(wt.destroyed) << "the first transport is closed (with it any room it made)";
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_CONNECTING);
    EXPECT_STREQ(st.transport, "relay-ws");
    ws.state = RELAY_TP_OPEN;
    Poll();
    ASSERT_EQ(ws.out.size(), 1u);
    EXPECT_NE(Nonce(ws), first) << "the relay would answer the old nonce with NONCE_REUSED";
    // An answer to the old request means nothing here.
    Bytes old = ws.out[0];
    std::copy(first.begin(), first.end(), old.begin() + 2);
    ws.in.push_back(RoomFrame(old, 1, "AAAAAA"));
    Poll();
    EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    Enter(ws, 2);
    st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_READY);
    EXPECT_STREQ(st.transport, "relay-ws");
    EXPECT_STREQ(st.room, "WXYZ23");
    EXPECT_EQ(st.self.bytes[3], 2);
    int s = vnet_open(v, 4000);
    EXPECT_EQ(Send(v, s, "hi", A("10.0.0.1:5000")), 2);
    ASSERT_EQ(ws.out.size(), 2u);
    EXPECT_EQ(ws.out[1][0], RELAY_T_DATA);
    EXPECT_EQ(wt.out.size(), 1u) << "nothing more goes to the abandoned transport";
}

TEST_F(RelayFallback, ATransportThatNeverComesUpIsGivenLimitedTime) {
    Make();
    Poll(kPrimaryTimeout - 1);
    EXPECT_EQ(fb.calls, 0);
    EXPECT_STREQ(Status().transport, "relay-wt");
    Poll(1);
    EXPECT_EQ(fb.calls, 1);
    EXPECT_STREQ(Status().transport, "relay-ws");
    EXPECT_TRUE(wt.destroyed);
}

TEST_F(RelayFallback, ARelayThatNeverAnswersIsGivenLimitedTimeToo) {
    Make();
    wt.state = RELAY_TP_OPEN;
    Poll();
    ASSERT_EQ(wt.out.size(), 1u);
    Poll(kPrimaryTimeout);
    EXPECT_EQ(fb.calls, 1);
    EXPECT_STREQ(Status().transport, "relay-ws");
}

TEST_F(RelayFallback, TheFallbackGetsTheWholeHandshakeTimeoutNotWhatIsLeft) {
    Make();
    Poll(kPrimaryTimeout);  // falls back at t = 3 s
    ASSERT_EQ(fb.calls, 1);
    ws.state = RELAY_TP_OPEN;
    // 10.5 s after creation but 7.5 s after the switch: the default timeout
    // of 10 s counts from the start of the WebSocket attempt.
    Poll(7500);
    EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    Poll(2500);  // 10 s after the switch
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ETIMEDOUT);
    EXPECT_EQ(fb.calls, 1) << "no third attempt";
}

TEST_F(RelayFallback, AnAnswerFromTheRelayIsFinalNotAReasonToFallBack) {
    struct {
        int reason, error;
    } cases[] = {{RELAY_E_ROOM_NOT_FOUND, VNET_ENOROOM}, {RELAY_E_ROOM_FULL, VNET_EROOMFULL},
                 {RELAY_E_LIMIT, VNET_ELIMIT}, {RELAY_E_BAD_VERSION, VNET_EPROTO}};
    for (auto &c : cases) {
        wt = ScriptTp("relay-wt");
        fb.calls = 0;
        Make(true, "WXYZ23");
        wt.state = RELAY_TP_OPEN;
        Poll();
        wt.in.push_back(ErrorFrame(wt.out.at(0), c.reason));
        Poll();
        vnet_status_info st = Status();
        EXPECT_EQ(st.state, VNET_STATUS_FAILED) << c.reason;
        EXPECT_EQ(st.error, c.error) << c.reason;
        EXPECT_EQ(fb.calls, 0) << "reason " << c.reason << " fell back";
        EXPECT_STREQ(st.transport, "relay-wt");
        vnet_destroy(v);
        v = nullptr;
    }
}

TEST_F(RelayFallback, LosingTheSessionAfterTheRoomWasEnteredIsNotAFallback) {
    Make();
    wt.state = RELAY_TP_OPEN;
    Poll();
    Enter(wt);
    ASSERT_EQ(Status().state, VNET_STATUS_READY);
    wt.state = RELAY_TP_FAILED;
    Poll();
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECLOSED);
    EXPECT_EQ(fb.calls, 0) << "the game has a room and an address; a silent switch would invalidate both";
}

TEST_F(RelayFallback, TheFallbackFailingToOpenIsReported) {
    struct {
        int rc, want;
    } cases[] = {{VNET_ENOTSUP, VNET_ECONNREFUSED}, {VNET_ENOMEM, VNET_ENOMEM}, {VNET_ECONNREFUSED, VNET_ECONNREFUSED}};
    for (auto &c : cases) {
        wt = ScriptTp("relay-wt");
        fb.calls = 0;
        fb.rc = c.rc;
        Make();
        wt.state = RELAY_TP_FAILED;
        Poll();
        vnet_status_info st = Status();
        EXPECT_EQ(st.state, VNET_STATUS_FAILED);
        EXPECT_EQ(st.error, c.want);
        EXPECT_EQ(fb.calls, 1);
        vnet_destroy(v);  // must not touch the already destroyed first transport again
        v = nullptr;
    }
}

TEST_F(RelayFallback, TheFallbackFailingToConnectEndsInFailureWithoutAThirdTry) {
    Make();
    wt.state = RELAY_TP_FAILED;
    Poll();
    ws.state = RELAY_TP_FAILED;
    Poll();
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ECONNREFUSED);
    EXPECT_EQ(fb.calls, 1);
}

TEST_F(RelayFallback, WithoutAFallbackAFailureIsFinal) {
    Make(false);
    wt.state = RELAY_TP_FAILED;
    Poll();
    EXPECT_EQ(Status().state, VNET_STATUS_FAILED);
    EXPECT_EQ(Status().error, VNET_ECONNREFUSED);
    EXPECT_EQ(fb.calls, 0);
}

TEST_F(RelayFallback, KeepaliveFollowsTheConfigurationNotTheFirstRelaysSuggestion) {
    // The first transport's ROOM suggested 2 s; the datagram check then failed.
    // On the fallback, with keepalive_ms left at 0, the relay's answer there counts.
    wt.max_frame = 1024;
    Make();
    wt.state = RELAY_TP_OPEN;
    Poll();
    Enter(wt, 1, 2);
    Poll(RELAY_PROBE_TIMEOUT_MS);
    ASSERT_EQ(fb.calls, 1);
    ws.state = RELAY_TP_OPEN;
    Poll();
    Enter(ws, 1, 5);
    ASSERT_EQ(Status().state, VNET_STATUS_READY);
    size_t n = ws.out.size();
    Poll(4999);
    EXPECT_EQ(ws.out.size(), n);
    Poll(1);
    ASSERT_EQ(ws.out.size(), n + 1);
    EXPECT_EQ(ws.out.back(), B({RELAY_T_KEEPALIVE}));
}

// ----------------------------------------------------- datagram-size probe

class RelayProbe : public RelayFallback {
protected:
    // In a room over a transport that reports a conservative size.
    void EnterSmall(bool with_fallback = true, size_t max_frame = 1024) {
        wt.max_frame = max_frame;
        Make(with_fallback);
        wt.state = RELAY_TP_OPEN;
        Poll();
        Enter(wt, 2);
    }
    Bytes Probe() {
        EXPECT_GE(wt.out.size(), 2u);
        return wt.out.size() >= 2 ? wt.out[1] : Bytes{};
    }
};

TEST_F(RelayProbe, AFullSizeDatagramToOneselfDecidesBeforeTheGameIsToldItCanSend) {
    EnterSmall();
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_CONNECTING) << "not READY until a full-size datagram has come back";
    int s = vnet_open(v, 4000);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.1:5000")), VNET_ENOTCONN);
    Bytes p = Probe();
    ASSERT_EQ(p.size(), (size_t)RELAY_WT_DATA_MAX);
    EXPECT_EQ(p[0], RELAY_T_DATA) << "so that a WebTransport transport sends it as a datagram";
    EXPECT_EQ(Bytes(p.begin() + 1, p.begin() + 5), B({10, 0, 0, 2})) << "to ourselves";
    EXPECT_EQ(Bytes(p.begin() + 5, p.begin() + 9), B({0, 0, 0, 0})) << "port 0: no socket has it";
    EXPECT_EQ(p.size() - RELAY_DATA_HDR_WS, (size_t)VNET_MAX_DATAGRAM);
    // Not yet: another member's datagram, a short echo, the wrong port, the wrong content.
    Bytes echo = Echo(p, 2);
    Bytes other = echo;
    other[4] = 3;
    Bytes shorter(echo.begin(), echo.end() - 1);
    Bytes longer = echo;  // a datagram with the right start but more bytes than we sent
    longer.push_back(0);
    Bytes port = echo;
    port[6] = 1;
    Bytes content = echo;
    content.back() ^= 1;
    Bytes subnet = echo;
    subnet[3] = 1;
    for (const Bytes &bad : {other, shorter, longer, port, content, subnet}) {
        wt.in.push_back(bad);
        Poll();
        EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    }
    wt.in.push_back(echo);
    Poll();
    st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_READY);
    EXPECT_STREQ(st.transport, "relay-wt");
    EXPECT_EQ(fb.calls, 0);
    EXPECT_EQ(Send(v, s, "x", A("10.0.0.1:5000")), 1);
    // The probe is not a game datagram: nothing reached the socket.
    char buf[8];
    vnet_addr from;
    EXPECT_EQ(vnet_recvfrom(v, s, buf, sizeof buf, &from), VNET_EAGAIN);
}

TEST_F(RelayProbe, TheProbeIsRepeatedUntilItIsAnsweredOrTheTimeIsUp) {
    EnterSmall();
    size_t n = wt.out.size();  // CREATE, probe
    Poll(RELAY_PROBE_RETRY_MS - 1);
    EXPECT_EQ(wt.out.size(), n);
    Poll(1);
    EXPECT_EQ(wt.out.size(), n + 1);
    EXPECT_EQ(wt.out.back().size(), (size_t)RELAY_WT_DATA_MAX);
    Poll(RELAY_PROBE_RETRY_MS);
    EXPECT_EQ(wt.out.size(), n + 2);
    Bytes late = Echo(wt.out.back(), 2);
    wt.in.push_back(late);
    Poll();
    EXPECT_EQ(Status().state, VNET_STATUS_READY) << "a repeat's echo is as good as the first's";
}

TEST_F(RelayProbe, NoAnswerMeansFallBackWhenThereIsAFallback) {
    EnterSmall();
    Bytes first = Nonce(wt);
    Poll(RELAY_PROBE_TIMEOUT_MS - 1);
    EXPECT_EQ(fb.calls, 0);
    Poll(1);
    EXPECT_EQ(fb.calls, 1);
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_CONNECTING);
    EXPECT_STREQ(st.transport, "relay-ws");
    EXPECT_TRUE(wt.destroyed);
    ws.state = RELAY_TP_OPEN;
    Poll();
    ASSERT_EQ(ws.out.size(), 1u);
    EXPECT_NE(Nonce(ws), first);
    Enter(ws, 1);
    EXPECT_EQ(Status().state, VNET_STATUS_READY);
    EXPECT_STREQ(Status().transport, "relay-ws");
}

TEST_F(RelayProbe, NoAnswerMeansFailureWhenThereIsNoFallback) {
    EnterSmall(false);
    Poll(RELAY_PROBE_TIMEOUT_MS);
    vnet_status_info st = Status();
    EXPECT_EQ(st.state, VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_EMSGSIZE);
    EXPECT_STREQ(vnet_strerror(st.error), "datagram too large");
}

TEST_F(RelayProbe, ATransportThatPromisesEnoughIsNotProbed) {
    // Exactly enough, and more than enough: no probe, READY at once. One byte
    // short is probed.
    for (size_t mf : {kFull, kFull + 1, (size_t)1500}) {
        wt = ScriptTp("relay-wt");
        EnterSmall(true, mf);
        EXPECT_EQ(Status().state, VNET_STATUS_READY) << mf;
        EXPECT_EQ(wt.out.size(), 1u) << "no probe frame for a transport promising " << mf;
        vnet_destroy(v);
        v = nullptr;
    }
    wt = ScriptTp("relay-wt");
    EnterSmall(true, kFull - 1);
    EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    EXPECT_EQ(wt.out.size(), 2u);
}

TEST_F(RelayProbe, AnUnreportedLimitIsProbedToo) {
    wt = ScriptTp("relay-wt");
    EnterSmall(true, 0);  // the browser did not say
    EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    EXPECT_EQ(wt.out.size(), 2u);
}

TEST_F(RelayProbe, TransportsWithoutALimitAreNeverProbed) {
    // The WebSocket transport has no max_data_frame hook.
    ScriptTp plain{"relay-ws"};
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_WS;
    cfg.ignore_env = 1;
    cfg.clock = TestClockFn;
    cfg.clock_user = &clk;
    int err = 0;
    v = vnet__create_with_relay_transport(&cfg, &kWsOps, &plain, &err);
    ASSERT_NE(v, nullptr);
    plain.state = RELAY_TP_OPEN;
    Poll();
    plain.in.push_back(RoomFrame(plain.out.at(0), 1, "WXYZ23"));
    Poll();
    EXPECT_EQ(Status().state, VNET_STATUS_READY);
    EXPECT_EQ(plain.out.size(), 1u);
    vnet_destroy(v);  // before `plain` goes out of scope
    v = nullptr;
}

TEST_F(RelayProbe, LeavingDuringTheProbeLeavesTheRoom) {
    EnterSmall();
    size_t n = wt.out.size();
    vnet_destroy(v);
    v = nullptr;
    ASSERT_EQ(wt.out.size(), n + 1);
    EXPECT_EQ(wt.out.back(), B({RELAY_T_LEAVE})) << "the relay already holds a member for us";
}

TEST_F(RelayProbe, ARepeatedRoomDuringTheProbeChangesNothing) {
    EnterSmall();
    size_t n = wt.out.size();
    wt.in.push_back(RoomFrame(wt.out.at(0), 5, "WXYZ23"));
    Poll();
    EXPECT_EQ(Status().state, VNET_STATUS_CONNECTING);
    EXPECT_EQ(wt.out.size(), n);
}

// -------------------------------------------------- arguments and AUTO setup

TEST(RelayAutoArguments, TheWebTransportUrlIsDerivedFromASecureWebSocketUrl) {
    char out[64];
    EXPECT_EQ(vnet__relay_auto_derive_wt_url("wss://relay.example.org/vnet", out, sizeof out), 0);
    EXPECT_STREQ(out, "https://relay.example.org/vnet");
    EXPECT_EQ(vnet__relay_auto_derive_wt_url("wss://relay.example.org:8443/a/b?c=d", out, sizeof out), 0);
    EXPECT_STREQ(out, "https://relay.example.org:8443/a/b?c=d");
    EXPECT_EQ(vnet__relay_auto_derive_wt_url("wss://[::1]:4433/vnet", out, sizeof out), 0);
    EXPECT_STREQ(out, "https://[::1]:4433/vnet");
    // Plain ws:// has no WebTransport form: WebTransport needs TLS.
    EXPECT_NE(vnet__relay_auto_derive_wt_url("ws://relay.example.org/vnet", out, sizeof out), 0);
    EXPECT_NE(vnet__relay_auto_derive_wt_url("https://relay.example.org/vnet", out, sizeof out), 0);
    EXPECT_NE(vnet__relay_auto_derive_wt_url("wss://", out, sizeof out), 0);
    EXPECT_NE(vnet__relay_auto_derive_wt_url("", out, sizeof out), 0);
    // Exactly fits / one too long.
    char tight[sizeof "https://h/v"];
    EXPECT_EQ(vnet__relay_auto_derive_wt_url("wss://h/v", tight, sizeof tight), 0);
    EXPECT_STREQ(tight, "https://h/v");
    EXPECT_NE(vnet__relay_auto_derive_wt_url("wss://h/vv", tight, sizeof tight), 0);
}

TEST(RelayAutoArguments, CertificateHashesAreSixtyFourHexDigits) {
    uint8_t h[32];
    int n = -1;
    std::string good(64, 'a');
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", good.c_str(), h, &n), 0);
    EXPECT_EQ(n, 32);
    EXPECT_EQ(h[0], 0xaa);
    std::string mixed = "0123456789abcdefABCDEF" + std::string(42, '0');
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", mixed.c_str(), h, &n), 0);
    EXPECT_EQ(h[0], 0x01);
    EXPECT_EQ(h[7], 0xef);
    EXPECT_EQ(h[8], 0xAB);
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", nullptr, h, &n), 0);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", "", h, &n), 0) << "empty means none";
    EXPECT_EQ(n, 0);
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", std::string(63, 'a').c_str(), h, &n), VNET_EINVAL);
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", std::string(65, 'a').c_str(), h, &n), VNET_EINVAL);
    EXPECT_EQ(vnet__relay_wt_check_args("https://h:1/vnet", (std::string(63, 'a') + "g").c_str(), h, &n), VNET_EINVAL);
    EXPECT_EQ(vnet__relay_wt_check_args("http://h:1/vnet", nullptr, h, &n), VNET_EINVAL);
    EXPECT_EQ(vnet__relay_wt_check_args("https://", nullptr, h, &n), VNET_EINVAL);
    EXPECT_EQ(vnet__relay_wt_check_args(nullptr, nullptr, h, &n), VNET_EINVAL);
}

TEST(RelayAutoArguments, BadArgumentsAreRejectedAndBrowserOnlyBackendsSayTheyAreNotAvailableNatively) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.ignore_env = 1;
    int err = 0;
    struct {
        int backend;
        const char *relay;
        const char *hash;
        int want;
    } cases[] = {
        {VNET_BACKEND_RELAY_AUTO, nullptr, nullptr, VNET_EINVAL},
        {VNET_BACKEND_RELAY_AUTO, "127.0.0.1:7777", nullptr, VNET_EINVAL},
        {VNET_BACKEND_RELAY_AUTO, "https://x/vnet", nullptr, VNET_EINVAL},
        {VNET_BACKEND_RELAY_AUTO, "wss://x/vnet", "nothex", VNET_EINVAL},
        {VNET_BACKEND_RELAY_AUTO, "wss://x/vnet", nullptr, VNET_ENOTSUP},
        {VNET_BACKEND_RELAY_AUTO, "ws://x/vnet", nullptr, VNET_ENOTSUP},
        {VNET_BACKEND_RELAY_WT, "ws://x/vnet", nullptr, VNET_EINVAL},
        {VNET_BACKEND_RELAY_WT, "https://x:1/vnet", "00", VNET_EINVAL},
        {VNET_BACKEND_RELAY_WT, "https://x:1/vnet", nullptr, VNET_ENOTSUP},
    };
    for (auto &c : cases) {
        cfg.backend = c.backend;
        cfg.relay = c.relay;
        cfg.relay_cert_sha256 = c.hash;
        vnet *v = vnet_create(&cfg, &err);
        EXPECT_EQ(v, nullptr) << c.backend << " " << (c.relay ? c.relay : "(null)");
        EXPECT_EQ(err, c.want) << c.backend << " " << (c.relay ? c.relay : "(null)");
        vnet_destroy(v);
    }
}

}  // namespace
