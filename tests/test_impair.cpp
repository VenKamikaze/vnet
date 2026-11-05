// SPDX-License-Identifier: MIT
#include "test_util.h"

#include <cmath>
#include <map>

// ---- spec parsing --------------------------------------------------------

TEST(ImpairParse, FullSpec) {
    vnet_impair im;
    ASSERT_EQ(vnet_impair_parse("lat=80,jit=20,loss=5,dup=1.5,reorder=3,rdelay=40,seed=7", &im), 0);
    EXPECT_EQ(im.latency_ms, 80u);
    EXPECT_EQ(im.jitter_ms, 20u);
    EXPECT_FLOAT_EQ(im.loss_pct, 5.0f);
    EXPECT_FLOAT_EQ(im.duplicate_pct, 1.5f);
    EXPECT_FLOAT_EQ(im.reorder_pct, 3.0f);
    EXPECT_EQ(im.reorder_delay_ms, 40u);
    EXPECT_EQ(im.seed, 7u);
}

TEST(ImpairParse, EmptySpecIsNoImpairment) {
    vnet_impair im;
    std::memset(&im, 0xAB, sizeof im);
    ASSERT_EQ(vnet_impair_parse("", &im), 0);
    EXPECT_EQ(im.latency_ms, 0u);
    EXPECT_EQ(im.loss_pct, 0.0f);
    EXPECT_EQ(im.seed, 0u);
}

TEST(ImpairParse, ToleratesWhitespaceHexSeedAndFractions) {
    vnet_impair im;
    ASSERT_EQ(vnet_impair_parse("  loss=.5 , seed=0xFF lat=3 ", &im), 0);
    EXPECT_FLOAT_EQ(im.loss_pct, 0.5f);
    EXPECT_EQ(im.seed, 255u);
    EXPECT_EQ(im.latency_ms, 3u);
}

TEST(ImpairParse, RejectsGarbage) {
    const char *bad[] = {"lat", "lat=", "lat=x", "lat=-1", "bogus=1", "loss=101", "loss=1e2",
                         "loss=1.2.3", "loss=", "loss=.", "dup=-5", "reorder=abc", "jit=70000",
                         "seed=0x", "seed=99999999999999999999", "lat=80;loss=5", "lat==80"};
    for (const char *s : bad) {
        vnet_impair im;
        std::memset(&im, 0x11, sizeof im);
        EXPECT_EQ(vnet_impair_parse(s, &im), VNET_EINVAL) << s;
    }
    EXPECT_EQ(vnet_impair_parse(nullptr, nullptr), VNET_EINVAL);
}

// ---- environment ---------------------------------------------------------

class ImpairEnv : public VnetTest {};

static vnet *EnvLoopback(vnet_loopback_network *net, const vnet_impair *imp, bool ignore_env, int *err) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.ignore_env = ignore_env;
    cfg.rx_queue_len = 4096;
    if (imp) cfg.impair = *imp;
    return vnet_create(&cfg, err);
}

// Sends n datagrams a->b, steps time, returns how many arrived.
static int Deliver(vnet_loopback_network *net, vnet *a, vnet *b, int n, int step_ms = 1000) {
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    vnet_addr to = A("127.1.0.2:2");
    for (int i = 0; i < n; i++) Send(a, sa, "x", to);
    vnet_loopback_network_step(net, (uint64_t)step_ms);
    int got = 0;
    std::string s;
    while (Recv(b, sb, &s) >= 0) got++;
    return got;
}

TEST_F(ImpairEnv, EnvironmentVariableEnablesImpairmentWithoutCodeChanges) {
    env.set("loss=100");
    vnet_loopback_network *net = vnet_loopback_network_create();
    int err = 0;
    vnet *a = EnvLoopback(net, nullptr, false, &err);
    vnet *b = EnvLoopback(net, nullptr, false, &err);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(Deliver(net, a, b, 50), 0);
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.impair_lost, 50u);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

TEST_F(ImpairEnv, IgnoreEnvFlagIsHonoured) {
    env.set("loss=100");
    vnet_loopback_network *net = vnet_loopback_network_create();
    int err = 0;
    vnet *a = EnvLoopback(net, nullptr, true, &err);
    vnet *b = EnvLoopback(net, nullptr, true, &err);
    EXPECT_EQ(Deliver(net, a, b, 50), 50);
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

TEST_F(ImpairEnv, EnvironmentReplacesConfiguredImpairment) {
    env.set("loss=0,lat=10");  // explicitly harmless
    vnet_impair cfgimp = {};
    cfgimp.loss_pct = 100;
    vnet_loopback_network *net = vnet_loopback_network_create();
    int err = 0;
    vnet *a = EnvLoopback(net, &cfgimp, false, &err);
    vnet *b = EnvLoopback(net, &cfgimp, false, &err);
    EXPECT_EQ(Deliver(net, a, b, 20), 20) << "env replaced the 100% loss";
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

TEST_F(ImpairEnv, ConfiguredSeedSurvivesWhenEnvHasNone) {
    // With the same env, different configured seeds must give different loss patterns.
    auto pattern = [&](uint64_t seed) {
        env.set("loss=50");
        vnet_impair cfgimp = {};
        cfgimp.seed = seed;
        vnet_loopback_network *net = vnet_loopback_network_create();
        int err = 0;
        vnet *a = EnvLoopback(net, &cfgimp, false, &err);
        vnet *b = EnvLoopback(net, &cfgimp, false, &err);
        int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
        std::string p;
        for (int i = 0; i < 64; i++) {
            Send(a, sa, "x", A("127.1.0.2:2"));
            vnet_loopback_network_step(net, 1);
            std::string s;
            p += Recv(b, sb, &s) >= 0 ? '1' : '0';
        }
        vnet_destroy(a);
        vnet_destroy(b);
        vnet_loopback_network_destroy(net);
        return p;
    };
    EXPECT_EQ(pattern(5), pattern(5));
    EXPECT_NE(pattern(5), pattern(6));
}

TEST_F(ImpairEnv, MalformedEnvironmentFailsCreateLoudly) {
    env.set("loss=banana");
    std::vector<std::string> logs;
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.log = [](void *u, int lvl, const char *m) {
        ASSERT_EQ(lvl, VNET_LOG_ERROR);
        static_cast<std::vector<std::string> *>(u)->push_back(m);
    };
    cfg.log_user = &logs;
    int err = 0;
    EXPECT_EQ(vnet_create(&cfg, &err), nullptr);
    EXPECT_EQ(err, VNET_EINVAL);
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_NE(logs[0].find("VNET_IMPAIR"), std::string::npos);
}

TEST_F(ImpairEnv, InvalidConfiguredPercentagesAreRejected) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    for (float bad : {-1.0f, 100.5f, NAN}) {
        vnet_impair im = {};
        im.loss_pct = bad;
        int err = 0;
        EXPECT_EQ(EnvLoopback(net, &im, true, &err), nullptr) << bad;
        EXPECT_EQ(err, VNET_EINVAL);
    }
    vnet_loopback_network_destroy(net);
}

// ---- behaviour over the loopback with virtual time -----------------------

class Impair : public VnetTest {
protected:
    struct Peers {
        vnet_loopback_network *net;
        vnet *a, *b;
        int sa, sb;
        vnet_addr to;
    };
    Peers Make(const vnet_impair &im, int rx = 100000) {
        Peers p;
        p.net = vnet_loopback_network_create();
        p.a = LoopbackHost(p.net, &im);
        p.b = LoopbackHost(p.net, nullptr, rx);
        p.sa = vnet_open(p.a, 1);
        p.sb = vnet_open(p.b, 2);
        p.to = A("127.1.0.2:2");
        return p;
    }
    void Free(Peers &p) {
        vnet_destroy(p.a);
        vnet_destroy(p.b);
        vnet_loopback_network_destroy(p.net);
    }
    // Sends packet i (payload = decimal i) at virtual time i*gap, drains b
    // each step, runs until `tail` ms after the last send. Returns arrival
    // records: (payload id, send time, arrival time) in arrival order.
    struct Rec { int id; uint64_t sent, arrived; };
    std::vector<Rec> Run(Peers &p, int n, int gap = 1, int tail = 2000) {
        std::vector<Rec> out;
        std::map<int, uint64_t> sent;
        auto drain = [&]() {
            std::string s;
            while (Recv(p.b, p.sb, &s) >= 0)
                out.push_back({std::stoi(s), sent[std::stoi(s)], vnet_loopback_network_time(p.net)});
        };
        for (int i = 0; i < n; i++) {
            sent[i] = vnet_loopback_network_time(p.net);
            Send(p.a, p.sa, std::to_string(i), p.to);
            for (int g = 0; g < gap; g++) {
                vnet_loopback_network_step(p.net, 1);
                drain();
            }
        }
        for (int t = 0; t < tail; t++) {
            vnet_loopback_network_step(p.net, 1);
            drain();
        }
        return out;
    }
};

TEST_F(Impair, OffByDefaultEverythingArrivesInOrderImmediately) {
    vnet_impair none = {};
    Peers p = Make(none);
    auto r = Run(p, 200, 1, 5);
    ASSERT_EQ(r.size(), 200u);
    for (size_t i = 0; i < r.size(); i++) {
        EXPECT_EQ(r[i].id, (int)i);
        EXPECT_EQ(r[i].arrived, r[i].sent + 1);  // visible at the next step
    }
    Free(p);
}

TEST_F(Impair, FixedLatencyDelaysByExactlyThatLong) {
    vnet_impair im = {};
    im.latency_ms = 80;
    Peers p = Make(im);
    Send(p.a, p.sa, "late", p.to);
    std::string s;
    vnet_loopback_network_step(p.net, 79);
    EXPECT_EQ(Recv(p.b, p.sb, &s), VNET_EAGAIN) << "must not arrive at 79 ms";
    vnet_loopback_network_step(p.net, 1);
    EXPECT_EQ(Recv(p.b, p.sb, &s), 4) << "must arrive at 80 ms";
    EXPECT_EQ(s, "late");
    Free(p);
}

TEST_F(Impair, LatencyAlsoNeedsSenderPollNotJustReceiver) {
    vnet_impair im = {};
    im.latency_ms = 10;
    Peers p = Make(im);
    Send(p.a, p.sa, "x", p.to);
    vnet_loopback_network_advance(p.net, 10);
    std::string s;
    vnet_poll(p.b);  // receiver polls, sender has not released yet
    EXPECT_EQ(Recv(p.b, p.sb, &s), VNET_EAGAIN);
    vnet_poll(p.a);  // release into b's staging
    vnet_poll(p.b);
    EXPECT_EQ(Recv(p.b, p.sb, &s), 1);
    Free(p);
}

// vnet_loopback_network_step() promises that once it returns, everything that
// is due in either direction is receivable, whatever the creation order.
TEST_F(Impair, NetworkStepMakesBothDirectionsDueTrafficReceivable) {
    vnet_impair im = {};
    im.latency_ms = 10;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net, &im), *b = LoopbackHost(net, &im);  // a is polled first
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    Send(a, sa, "a2b", A("127.1.0.2:2"));
    Send(b, sb, "b2a", A("127.1.0.1:1"));
    vnet_loopback_network_step(net, 10);
    std::string s;
    EXPECT_EQ(Recv(b, sb, &s), 3);
    EXPECT_EQ(s, "a2b");
    EXPECT_EQ(Recv(a, sa, &s), 3);
    EXPECT_EQ(s, "b2a");
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}

// Frames are much longer than the jitter, so many datagrams become due inside
// one step. They must still reach the receiver in due-time order (as a 1 ms
// step would deliver them), not in the order they were sent.
TEST_F(Impair, CoarseFramesStillReleaseInDueOrder) {
    auto arrival_order = [&](int step_ms) {
        vnet_impair im = {};
        im.latency_ms = 50;
        im.jitter_ms = 40;
        im.seed = 21;
        Peers p = Make(im);
        std::vector<int> order;
        auto drain = [&]() {
            std::string s;
            while (Recv(p.b, p.sb, &s) >= 0) order.push_back(std::stoi(s));
        };
        const int kFrame = 33, kFrames = 60, kPerFrame = 10;
        for (int f = 0; f < kFrames; f++) {
            for (int k = 0; k < kPerFrame; k++) Send(p.a, p.sa, std::to_string(f * kPerFrame + k), p.to);
            for (int t = 0; t < kFrame; t += step_ms) {
                vnet_loopback_network_step(p.net, (uint64_t)step_ms);
                drain();
            }
        }
        for (int t = 0; t < 330; t += step_ms) {
            vnet_loopback_network_step(p.net, (uint64_t)step_ms);
            drain();
        }
        Free(p);
        return order;
    };
    std::vector<int> fine = arrival_order(1);    // each tick releases only what is due then
    std::vector<int> coarse = arrival_order(33); // many due at once
    ASSERT_EQ(fine.size(), 600u);
    int inversions = 0;
    for (size_t i = 1; i < fine.size(); i++) inversions += fine[i] < fine[i - 1];
    EXPECT_GT(inversions, 50) << "the scenario must actually reorder";
    EXPECT_EQ(coarse, fine);
}

TEST_F(Impair, JitterStaysWithinBoundsAndActuallyVaries) {
    vnet_impair im = {};
    im.latency_ms = 50;
    im.jitter_ms = 20;
    im.seed = 99;
    Peers p = Make(im);
    auto r = Run(p, 3000, 1, 200);
    ASSERT_EQ(r.size(), 3000u);
    uint64_t lo = ~0ull, hi = 0;
    for (auto &x : r) {
        lo = std::min(lo, x.arrived - x.sent);
        hi = std::max(hi, x.arrived - x.sent);
    }
    // Arrival is observed at the next whole step at/after due time.
    EXPECT_GE(lo, 30u);
    EXPECT_LE(hi, 70u);
    EXPECT_LE(lo, 32u) << "jitter should reach the lower edge over 3000 samples";
    EXPECT_GE(hi, 68u) << "jitter should reach the upper edge over 3000 samples";
    Free(p);
}

TEST_F(Impair, JitterLargerThanLatencyNeverGoesNegative) {
    vnet_impair im = {};
    im.latency_ms = 5;
    im.jitter_ms = 50;
    im.seed = 3;
    Peers p = Make(im);
    auto r = Run(p, 500, 1, 200);
    ASSERT_EQ(r.size(), 500u);
    for (auto &x : r) EXPECT_GE(x.arrived, x.sent);
    Free(p);
}

TEST_F(Impair, JitterCanReorderPackets) {
    vnet_impair im = {};
    im.latency_ms = 50;
    im.jitter_ms = 40;
    im.seed = 1;
    Peers p = Make(im);
    auto r = Run(p, 500, 1, 200);
    int inversions = 0;
    for (size_t i = 1; i < r.size(); i++) inversions += r[i].id < r[i - 1].id;
    EXPECT_GT(inversions, 20);
    Free(p);
}

TEST_F(Impair, LossRateWithinToleranceAndCountsAgree) {
    for (float pct : {1.0f, 10.0f, 50.0f}) {
        vnet_impair im = {};
        im.loss_pct = pct;
        im.seed = 2024;
        Peers p = Make(im);
        const int N = 20000;
        auto r = Run(p, N, 1, 5);
        vnet_stats st;
        vnet_get_stats(p.a, &st);
        double q = pct / 100.0;
        double sigma = std::sqrt(N * q * (1 - q));
        EXPECT_NEAR((double)st.impair_lost, N * q, 5 * sigma) << pct;
        EXPECT_EQ(r.size() + st.impair_lost, (size_t)N) << "every packet is delivered or counted lost";
        EXPECT_EQ(st.tx_datagrams, (uint64_t)N);
        EXPECT_EQ(st.wire_datagrams, r.size());
        Free(p);
    }
}

TEST_F(Impair, ZeroLossLosesNothingAndHundredLosesEverything) {
    vnet_impair im = {};
    im.latency_ms = 1;  // impairment on, but no loss
    Peers p = Make(im);
    EXPECT_EQ(Run(p, 1000, 1, 5).size(), 1000u);
    Free(p);
    im = {};
    im.loss_pct = 100;
    p = Make(im);
    EXPECT_EQ(Run(p, 1000, 1, 5).size(), 0u);
    Free(p);
}

TEST_F(Impair, DuplicationRateWithinTolerance) {
    vnet_impair im = {};
    im.duplicate_pct = 20;
    im.seed = 77;
    Peers p = Make(im);
    const int N = 10000;
    auto r = Run(p, N, 1, 5);
    vnet_stats st;
    vnet_get_stats(p.a, &st);
    double sigma = std::sqrt(N * 0.2 * 0.8);
    EXPECT_NEAR((double)st.impair_duplicated, N * 0.2, 5 * sigma);
    EXPECT_EQ(r.size(), (size_t)N + st.impair_duplicated);
    std::map<int, int> copies;
    for (auto &x : r) copies[x.id]++;
    EXPECT_EQ(copies.size(), (size_t)N) << "nothing lost";
    for (auto &kv : copies) EXPECT_LE(kv.second, 2);
    Free(p);
}

TEST_F(Impair, FullDuplicationDeliversEverythingTwice) {
    vnet_impair im = {};
    im.duplicate_pct = 100;
    Peers p = Make(im);
    auto r = Run(p, 100, 1, 5);
    EXPECT_EQ(r.size(), 200u);
    Free(p);
}

TEST_F(Impair, NoReorderingWithoutJitterOrReorderSetting) {
    vnet_impair im = {};
    im.latency_ms = 80;
    im.loss_pct = 10;
    im.duplicate_pct = 10;
    im.seed = 5;
    Peers p = Make(im);
    auto r = Run(p, 2000, 1, 200);
    // Duplicates share latency so order of first/second copies is preserved.
    for (size_t i = 1; i < r.size(); i++) EXPECT_GE(r[i].id, r[i - 1].id) << i;
    Free(p);
}

TEST_F(Impair, ReorderingRateAndLateness) {
    vnet_impair im = {};
    im.reorder_pct = 20;
    im.reorder_delay_ms = 30;
    im.seed = 11;
    Peers p = Make(im);
    const int N = 5000;
    auto r = Run(p, N, 1, 100);
    vnet_stats st;
    vnet_get_stats(p.a, &st);
    double sigma = std::sqrt(N * 0.2 * 0.8);
    EXPECT_NEAR((double)st.impair_reordered, N * 0.2, 5 * sigma);
    ASSERT_EQ(r.size(), (size_t)N);
    int late = 0, overtaken = 0;
    for (size_t i = 0; i < r.size(); i++) {
        uint64_t d = r[i].arrived - r[i].sent;
        EXPECT_TRUE(d == 1 || d == 30) << "delay " << d;
        late += d == 30;
        if (i && r[i].id < r[i - 1].id) overtaken++;
    }
    EXPECT_EQ((uint64_t)late, st.impair_reordered);
    EXPECT_GT(overtaken, 100);
    Free(p);
}

TEST_F(Impair, DefaultReorderDelayIs50ms) {
    vnet_impair im = {};
    im.reorder_pct = 100;
    Peers p = Make(im);
    Send(p.a, p.sa, "x", p.to);
    std::string s;
    vnet_loopback_network_step(p.net, 49);
    EXPECT_EQ(Recv(p.b, p.sb, &s), VNET_EAGAIN);
    vnet_loopback_network_step(p.net, 1);
    EXPECT_EQ(Recv(p.b, p.sb, &s), 1);
    Free(p);
}

TEST_F(Impair, DelayQueueOverflowDropsAndCounts) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.ignore_env = 1;
    cfg.impair.latency_ms = 1000;
    cfg.impair_queue_len = 8;
    vnet *a = vnet_create(&cfg, nullptr);
    int s = vnet_open(a, 1);
    for (int i = 0; i < 20; i++) EXPECT_EQ(Send(a, s, "x", A("127.0.0.1:1")), 1);
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.impair_overflow, 12u);
    vnet_destroy(a);
}

TEST_F(Impair, ClosingSocketDropsItsDelayedDatagrams) {
    vnet_impair im = {};
    im.latency_ms = 100;
    Peers p = Make(im);
    Send(p.a, p.sa, "x", p.to);
    vnet_close(p.a, p.sa);
    int sa2 = vnet_open(p.a, 1);  // reuses the handle
    ASSERT_EQ(sa2, p.sa);
    vnet_loopback_network_step(p.net, 200);
    std::string s;
    EXPECT_EQ(Recv(p.b, p.sb, &s), VNET_EAGAIN);
    vnet_stats st;
    vnet_get_stats(p.a, &st);
    EXPECT_EQ(st.wire_datagrams, 0u);
    Free(p);
}

TEST_F(Impair, ImpairedSendStillReportsErrorsSynchronously) {
    vnet_impair im = {};
    im.latency_ms = 10;
    Peers p = Make(im);
    EXPECT_EQ(Send(p.a, 55, "x", p.to), VNET_EBADF);
    EXPECT_EQ(Send(p.a, p.sa, "x", A("[::1]:2")), VNET_EAFNOSUPPORT);
    EXPECT_EQ(Send(p.a, p.sa, std::string(VNET_MAX_DATAGRAM + 1, 'x'), p.to), VNET_EMSGSIZE);
    Free(p);
}

// ---- determinism -----------------------------------------------------------

static std::string Trace(uint64_t seed, const vnet_impair &base) {
    vnet_impair im = base;
    im.seed = seed;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net, &im), *b = LoopbackHost(net, nullptr, 100000);
    int sa = vnet_open(a, 1), sb = vnet_open(b, 2);
    vnet_addr to = A("127.1.0.2:2");
    std::string trace;
    for (int i = 0; i < 3000; i++) {
        Send(a, sa, std::to_string(i), to);
        vnet_loopback_network_step(net, 1);
        std::string s;
        while (Recv(b, sb, &s) >= 0) trace += std::to_string(vnet_loopback_network_time(net)) + ":" + s + ",";
    }
    for (int t = 0; t < 300; t++) {
        vnet_loopback_network_step(net, 1);
        std::string s;
        while (Recv(b, sb, &s) >= 0) trace += std::to_string(vnet_loopback_network_time(net)) + ":" + s + ",";
    }
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
    return trace;
}

TEST_F(Impair, SameSeedGivesBitIdenticalTraceDifferentSeedDiffers) {
    vnet_impair im = {};
    im.latency_ms = 60;
    im.jitter_ms = 30;
    im.loss_pct = 8;
    im.duplicate_pct = 5;
    im.reorder_pct = 7;
    std::string t1 = Trace(42, im), t2 = Trace(42, im), t3 = Trace(43, im);
    EXPECT_GT(t1.size(), 10000u);
    EXPECT_EQ(t1, t2);
    EXPECT_NE(t1, t3);
}

TEST_F(Impair, SeedZeroIsAValidDeterministicSeed) {
    vnet_impair im = {};
    im.loss_pct = 30;
    im.latency_ms = 5;
    EXPECT_EQ(Trace(0, im), Trace(0, im));
}

// Changing one percentage must not shift the random draws of the others: the
// packets that are duplicated stay the same when loss is turned on.
TEST_F(Impair, DrawsAreIndependentOfOtherSettings) {
    auto dup_ids = [&](float loss) {
        vnet_impair im = {};
        im.duplicate_pct = 30;
        im.loss_pct = loss;
        im.seed = 8;
        Peers p = Make(im);
        auto r = Run(p, 500, 1, 5);
        std::map<int, int> c;
        for (auto &x : r) c[x.id]++;
        std::string dups;
        for (auto &kv : c)
            if (kv.second == 2) dups += std::to_string(kv.first) + ",";
        Free(p);
        return dups;
    };
    std::string with_loss = dup_ids(20.0f), without = dup_ids(0.0f);
    // Every duplicated packet that survived loss is duplicated without loss too.
    std::map<int, int> base;
    size_t pos = 0;
    while (pos < without.size()) {
        size_t c = without.find(',', pos);
        base[std::stoi(without.substr(pos, c - pos))] = 1;
        pos = c + 1;
    }
    pos = 0;
    int checked = 0;
    while (pos < with_loss.size()) {
        size_t c = with_loss.find(',', pos);
        EXPECT_TRUE(base.count(std::stoi(with_loss.substr(pos, c - pos))));
        checked++;
        pos = c + 1;
    }
    EXPECT_GT(checked, 30);
}
