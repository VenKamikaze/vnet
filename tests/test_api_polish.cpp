// SPDX-License-Identifier: MIT
// The 0.4.0 additions: vnet_version, vnet_set_impair, burst loss, vnet_wait,
// vnet_resolve_host, the UDP receive buffer, and the promise that native
// backends never call the wake callback.
#include "test_util.h"

#include <chrono>
#include <cmath>
#include <set>
#include <vector>

#if defined(VNET_TEST_FAKE_IPV6)  // the --wrap seam (ipv6_shim.cpp), Linux only
extern "C" int g_last_rcvbuf;
extern "C" int g_fail_rcvbuf;
#endif

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---------------------------------------------------------------- version

TEST(Version, MatchesTheMacrosAndTheCMakeProject) {
    std::string expect = std::to_string(VNET_VERSION_MAJOR) + "." + std::to_string(VNET_VERSION_MINOR) + "." +
                         std::to_string(VNET_VERSION_PATCH);
    EXPECT_EQ(std::string(vnet_version()), expect);
    EXPECT_EQ(expect, VNET_PROJECT_VERSION) << "CMakeLists.txt and vnet.h disagree";
}

// ------------------------------------------------------------ set_impair

class SetImpair : public VnetTest {
protected:
    void SetUp() override {
        net = vnet_loopback_network_create();
        a = LoopbackHost(net);
        b = LoopbackHost(net);
        sa = vnet_open(a, 1);
        sb = vnet_open(b, 2);
        to_b = A("127.1.0.2:2");
    }
    void TearDown() override {
        vnet_destroy(a);
        vnet_destroy(b);
        vnet_loopback_network_destroy(net);
    }
    // Send n numbered datagrams a -> b, step 0 ms, return which arrived.
    std::set<int> Burst(int first, int n) {
        for (int i = first; i < first + n; i++) Send(a, sa, std::to_string(i), to_b);
        vnet_loopback_network_step(net, 0);
        std::set<int> got;
        std::string d;
        while (Recv(b, sb, &d) >= 0) got.insert(std::stoi(d));
        return got;
    }
    vnet_loopback_network *net;
    vnet *a, *b;
    int sa, sb;
    vnet_addr to_b;
};

TEST_F(SetImpair, TotalLossThenRecoveryOnALiveContext) {
    EXPECT_EQ(Burst(0, 50).size(), 50u);
    vnet_impair out = {};
    out.loss_pct = 100;
    ASSERT_EQ(vnet_set_impair(a, &out), 0);
    EXPECT_EQ(Burst(100, 50).size(), 0u) << "an outage";
    vnet_stats st;
    vnet_get_stats(a, &st);
    EXPECT_EQ(st.impair_lost, 50u);
    ASSERT_EQ(vnet_set_impair(a, nullptr), 0);
    EXPECT_EQ(Burst(200, 50).size(), 50u) << "recovered; the sockets were never recreated";
}

TEST_F(SetImpair, AllZeroAndSeedOnlyTurnItOffToo) {
    vnet_impair out = {};
    out.loss_pct = 100;
    ASSERT_EQ(vnet_set_impair(a, &out), 0);
    EXPECT_EQ(Burst(0, 10).size(), 0u);
    vnet_impair zero = {};
    ASSERT_EQ(vnet_set_impair(a, &zero), 0);
    EXPECT_EQ(Burst(10, 10).size(), 10u);
    ASSERT_EQ(vnet_set_impair(a, &out), 0);
    vnet_impair seed_only = {};
    seed_only.seed = 99;
    ASSERT_EQ(vnet_set_impair(a, &seed_only), 0);
    EXPECT_EQ(Burst(20, 10).size(), 10u);
}

TEST_F(SetImpair, LatencySpikeOnAContextCreatedWithoutImpairment) {
    vnet_impair spike = {};
    spike.latency_ms = 300;
    ASSERT_EQ(vnet_set_impair(a, &spike), 0);
    std::string d;
    Send(a, sa, "late", to_b);
    vnet_loopback_network_step(net, 299);
    EXPECT_EQ(Recv(b, sb, &d), VNET_EAGAIN);
    vnet_loopback_network_step(net, 1);
    EXPECT_EQ(Recv(b, sb, &d), 4);
}

TEST_F(SetImpair, DatagramsAlreadyDelayedStayQueuedAndAreReleasedOnSchedule) {
    vnet_impair slow = {};
    slow.latency_ms = 100;
    ASSERT_EQ(vnet_set_impair(a, &slow), 0);
    Send(a, sa, "one", to_b);
    vnet_loopback_network_step(net, 40);
    Send(a, sa, "two", to_b);  // due at t = 140
    // Turn impairment off, and then change it: the two held datagrams are unaffected.
    ASSERT_EQ(vnet_set_impair(a, nullptr), 0);
    Send(a, sa, "now", to_b);
    vnet_loopback_network_step(net, 0);
    std::string d;
    ASSERT_EQ(Recv(b, sb, &d), 3);
    EXPECT_EQ(d, "now") << "sent after the change: not delayed";
    EXPECT_EQ(Recv(b, sb, &d), VNET_EAGAIN);
    vnet_impair lossy = {};
    lossy.loss_pct = 100;
    ASSERT_EQ(vnet_set_impair(a, &lossy), 0);  // does not drop what is already held
    vnet_loopback_network_step(net, 59);       // t = 99
    EXPECT_EQ(Recv(b, sb, &d), VNET_EAGAIN);
    vnet_loopback_network_step(net, 1);        // t = 100: "one" is due
    ASSERT_EQ(Recv(b, sb, &d), 3);
    EXPECT_EQ(d, "one");
    vnet_loopback_network_step(net, 39);       // t = 139
    EXPECT_EQ(Recv(b, sb, &d), VNET_EAGAIN);
    vnet_loopback_network_step(net, 1);        // t = 140: "two"
    ASSERT_EQ(Recv(b, sb, &d), 3);
    EXPECT_EQ(d, "two");
}

TEST_F(SetImpair, ReseedingMakesARunReproducible) {
    vnet_impair im = {};
    im.loss_pct = 40;
    im.seed = 1234;
    ASSERT_EQ(vnet_set_impair(a, &im), 0);
    std::set<int> first = Burst(0, 300);
    EXPECT_GT(first.size(), 100u);
    EXPECT_LT(first.size(), 260u);
    // The same call again restarts the PRNG: the same fate for the same sends.
    ASSERT_EQ(vnet_set_impair(a, &im), 0);
    std::set<int> again = Burst(0, 300);
    EXPECT_EQ(first, again);
    im.seed = 1235;
    ASSERT_EQ(vnet_set_impair(a, &im), 0);
    EXPECT_NE(first, Burst(0, 300)) << "another seed, another pattern";
}

TEST_F(SetImpair, BadValuesChangeNothing) {
    vnet_impair out = {};
    out.loss_pct = 100;
    ASSERT_EQ(vnet_set_impair(a, &out), 0);
    for (float bad : {101.0f, -1.0f, NAN}) {
        vnet_impair im = {};
        im.loss_pct = bad;
        EXPECT_EQ(vnet_set_impair(a, &im), VNET_EINVAL);
    }
    vnet_impair im = {};
    im.latency_ms = 70000;
    EXPECT_EQ(vnet_set_impair(a, &im), VNET_EINVAL);
    im = {};
    im.burst_pct = 5;  // burst_len missing
    EXPECT_EQ(vnet_set_impair(a, &im), VNET_EINVAL);
    EXPECT_EQ(Burst(0, 20).size(), 0u) << "total loss is still in force";
    EXPECT_EQ(vnet_set_impair(nullptr, &out), VNET_EINVAL);
}

TEST_F(SetImpair, WinsOverTheEnvironmentVariableWhichOnlyCreateReads) {
    env.set("loss=100");
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    vnet *c = vnet_create(&cfg, nullptr);  // reads VNET_IMPAIR: everything it sends is lost
    ASSERT_NE(c, nullptr);
    int sc = vnet_open(c, 3);
    vnet_addr to = A("127.1.0.2:2");
    Send(c, sc, "x", to);
    vnet_loopback_network_step(net, 0);
    std::string d;
    EXPECT_EQ(Recv(b, sb, &d), VNET_EAGAIN);
    ASSERT_EQ(vnet_set_impair(c, nullptr), 0);
    Send(c, sc, "y", to);
    vnet_loopback_network_step(net, 0);
    EXPECT_EQ(Recv(b, sb, &d), 1);
    vnet_destroy(c);
}

TEST_F(SetImpair, HonoursTheQueueLengthGivenAtCreate) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    cfg.ignore_env = 1;
    cfg.impair_queue_len = 3;
    vnet *c = vnet_create(&cfg, nullptr);
    int sc = vnet_open(c, 3);
    vnet_impair im = {};
    im.latency_ms = 1000;
    ASSERT_EQ(vnet_set_impair(c, &im), 0);
    for (int i = 0; i < 10; i++) Send(c, sc, "x", to_b);
    vnet_stats st;
    vnet_get_stats(c, &st);
    EXPECT_EQ(st.impair_overflow, 7u);
    vnet_destroy(c);
}

TEST_F(SetImpair, WorksOnUdp) {
    vnet *u = UdpHost();
    vnet *w = UdpHost();
    int su = vnet_open(u, 0), sw = vnet_open(w, 0);
    vnet_addr lw;
    ASSERT_EQ(vnet_local_addr(w, sw, &lw), 0);
    vnet_addr to = A("127.0.0.1:1");
    to.port = lw.port;
    vnet_impair out = {};
    out.loss_pct = 100;
    ASSERT_EQ(vnet_set_impair(u, &out), 0);
    for (int i = 0; i < 5; i++) Send(u, su, "x", to);
    std::string d;
    EXPECT_EQ(RecvWait(w, sw, &d, nullptr, 100), VNET_EAGAIN);
    ASSERT_EQ(vnet_set_impair(u, nullptr), 0);
    Send(u, su, "y", to);
    EXPECT_EQ(RecvWait(w, sw, &d), 1);
    vnet_destroy(u);
    vnet_destroy(w);
}

// ---------------------------------------------------------- burst loss

static const vnet_impair kBurst = [] {
    vnet_impair im = {};
    im.burst_pct = 2.0f;
    im.burst_len = 5.0f;
    im.seed = 42;
    return im;
}();

// Which of n sent packets were lost (read off the stats: no receiver needed).
static std::vector<char> LossPattern(const vnet_impair &im, int n) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *a = LoopbackHost(net, &im);
    int s = vnet_open(a, 1);
    vnet_addr to = A("127.1.0.9:1");
    std::vector<char> lost((size_t)n);
    uint64_t prev = 0;
    for (int i = 0; i < n; i++) {
        vnet_sendto(a, s, "x", 1, &to);
        vnet_stats st;
        vnet_get_stats(a, &st);
        lost[(size_t)i] = st.impair_lost != prev;
        prev = st.impair_lost;
    }
    vnet_destroy(a);
    vnet_loopback_network_destroy(net);
    return lost;
}

TEST(BurstLoss, ParserAndValidation) {
    vnet_impair im;
    ASSERT_EQ(vnet_impair_parse("burst=2.5,blen=4,seed=3", &im), 0);
    EXPECT_FLOAT_EQ(im.burst_pct, 2.5f);
    EXPECT_FLOAT_EQ(im.burst_len, 4.0f);
    EXPECT_EQ(vnet_impair_parse("blen=100001", &im), VNET_EINVAL);
    EXPECT_EQ(vnet_impair_parse("burst=101", &im), VNET_EINVAL);
    EXPECT_EQ(vnet_impair_parse("burst=", &im), VNET_EINVAL);
    EXPECT_EQ(vnet_impair_parse("blen=x", &im), VNET_EINVAL);
    // Creation rejects a burst without a usable length, and accepts "blen" alone (off).
    for (auto [pct, len, want] : std::vector<std::tuple<float, float, bool>>{
             {2, 0, false}, {2, 0.5f, false}, {2, 100001, false}, {101, 5, false}, {2, 1, true}, {0, 0, true}, {0, 7, true}}) {
        vnet_config cfg;
        vnet_config_init(&cfg);
        cfg.backend = VNET_BACKEND_LOOPBACK;
        cfg.ignore_env = 1;
        cfg.impair.burst_pct = pct;
        cfg.impair.burst_len = len;
        int err = 0;
        vnet *v = vnet_create(&cfg, &err);
        EXPECT_EQ(v != nullptr, want) << pct << " " << len;
        if (!want) {
            EXPECT_EQ(err, VNET_EINVAL);
        }
        vnet_destroy(v);
    }
}

TEST(BurstLoss, TheSameSeedGivesTheSamePatternAnotherSeedAnother) {
    EnvGuard env;
    auto p1 = LossPattern(kBurst, 20000), p2 = LossPattern(kBurst, 20000);
    EXPECT_EQ(p1, p2);
    vnet_impair other = kBurst;
    other.seed = 43;
    EXPECT_NE(p1, LossPattern(other, 20000));
}

TEST(BurstLoss, BurstLengthsAndLossRateMatchTheConfiguration) {
    EnvGuard env;
    // burst=2 %, blen=5 packets: enter a burst with p = 0.02 per packet, leave
    // it with r = 1/5 per packet. Over N = 400000 packets there are about
    // N * p / (1 + p/r) ~ 7300 bursts, so the mean burst length has a standard
    // error of ~ 4.5 / sqrt(7300) = 0.05 (1%), and the loss rate p / (p + r) =
    // 9.09% one of ~ 0.05 points. Tolerances below are 5% / 8% / 10%, several
    // sigma, so a fixed seed cannot flake and a wrong model still fails.
    const int N = 400000;
    auto lost = LossPattern(kBurst, N);
    std::vector<int> runs;
    int run = 0;
    long lost_total = 0;
    for (char l : lost) {
        if (l) {
            run++;
            lost_total++;
        } else if (run) {
            runs.push_back(run);
            run = 0;
        }
    }
    ASSERT_GT(runs.size(), 5000u);
    double mean = 0;
    int ones = 0, longer_than_10 = 0;
    for (int r : runs) {
        mean += r;
        ones += r == 1;
        longer_than_10 += r > 10;
    }
    mean /= (double)runs.size();
    EXPECT_NEAR(mean, 5.0, 5.0 * 0.05) << "mean burst length";
    // Geometric: P(length = 1) = r = 0.2, P(length > 10) = (1 - r)^10 = 0.107.
    EXPECT_NEAR((double)ones / (double)runs.size(), 0.2, 0.2 * 0.08);
    EXPECT_NEAR((double)longer_than_10 / (double)runs.size(), 0.107, 0.107 * 0.10);
    EXPECT_NEAR((double)lost_total / N, 0.02 / (0.02 + 0.2), 0.0909 * 0.08) << "loss rate";
}

TEST(BurstLoss, UnitLengthBurstsAreIsolatedLosses) {
    EnvGuard env;
    vnet_impair im = {};
    im.burst_pct = 10;
    im.burst_len = 1;
    im.seed = 5;
    auto lost = LossPattern(im, 50000);
    long n = 0, adjacent = 0;
    for (size_t i = 0; i < lost.size(); i++) {
        n += lost[i];
        adjacent += i > 0 && lost[i] && lost[i - 1];
    }
    EXPECT_NEAR((double)n / 50000, 0.10, 0.01);
    // After a burst of one the next packet is in the good state, which starts a new burst with p = 10%.
    EXPECT_EQ(adjacent, 0) << "a burst of length 1 is a single packet; the next draw leaves the burst first";
}

TEST(BurstLoss, SwitchingItOnNeverMovesTheOtherDraws) {
    EnvGuard env;
    vnet_impair base = {};
    base.loss_pct = 10;
    base.latency_ms = 20;
    base.jitter_ms = 15;
    base.duplicate_pct = 5;
    base.seed = 77;
    auto without = LossPattern(base, 20000);
    vnet_impair with = base;
    with.burst_pct = 1;
    with.burst_len = 8;
    auto withb = LossPattern(with, 20000);
    // Everything the uniform loss dropped is still dropped; the bursts add to it.
    long extra = 0;
    for (size_t i = 0; i < without.size(); i++) {
        if (without[i]) {
            EXPECT_TRUE(withb[i]) << i;
        }
        extra += withb[i] && !without[i];
    }
    EXPECT_GT(extra, 500);
}

TEST(BurstLoss, BurstsAreBlackoutsInTime) {
    EnvGuard env;
    // 1 packet per ms, bursts of ~50 packets: the receiver sees silences of
    // about 50 ms, not scattered holes.
    vnet_impair im = {};
    im.burst_pct = 0.2f;
    im.burst_len = 50;
    im.seed = 9;
    auto lost = LossPattern(im, 100000);
    int longest = 0, run = 0;
    for (char l : lost) {
        run = l ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    EXPECT_GE(longest, 100) << "with a mean of 50 some bursts are long";
}

// -------------------------------------------------------------- vnet_wait

class UdpWait : public VnetTest {
protected:
    void SetUp() override {
        a = UdpHost();
        b = UdpHost();
        sa = vnet_open(a, 0);
        sb = vnet_open(b, 0);
        vnet_addr lb;
        vnet_local_addr(b, sb, &lb);
        to_b = A("127.0.0.1:1");
        to_b.port = lb.port;
    }
    void TearDown() override {
        vnet_destroy(a);
        vnet_destroy(b);
    }
    vnet *a, *b;
    int sa, sb;
    vnet_addr to_b;
};

TEST_F(UdpWait, TimesOutWhenNothingArrives) {
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_wait(b, 60), 0);
    double ms = MsSince(t0);
    EXPECT_GE(ms, 50);
    EXPECT_LT(ms, 1500);
    t0 = Clock::now();
    EXPECT_EQ(vnet_wait(b, 0), 0) << "0 just looks";
    EXPECT_LT(MsSince(t0), 100);
}

TEST_F(UdpWait, ReturnsAtOnceForADatagramThatIsAlreadyThere) {
    ASSERT_EQ(Send(a, sa, "hi", to_b), 2);
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_wait(b, 5000), 1);
    EXPECT_LT(MsSince(t0), 1000);
    std::string d;
    EXPECT_EQ(RecvWait(b, sb, &d), 2);
}

TEST_F(UdpWait, WakesWhenADatagramArrivesWhileSleeping) {
    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        Send(a, sa, "late", to_b);
    });
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_wait(b, 10000), 1);
    double ms = MsSince(t0);
    sender.join();
    EXPECT_GE(ms, 50);
    EXPECT_LT(ms, 5000) << "woken by the datagram, not by the timeout";
    std::string d;
    EXPECT_EQ(RecvWait(b, sb, &d), 4);
}

TEST_F(UdpWait, BadArgumentsAndNoSockets) {
    EXPECT_EQ(vnet_wait(nullptr, 1), VNET_EINVAL);
    EXPECT_EQ(vnet_wait(b, -1), VNET_EINVAL);
    vnet_close(b, sb);
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_wait(b, 40), 0) << "no socket is open: nothing can arrive; it just sleeps";
    EXPECT_GE(MsSince(t0), 30);
}

TEST_F(UdpWait, ReturnsWhenADelayedDatagramIsDueSoThePollCanReleaseIt) {
    vnet_impair slow = {};
    slow.latency_ms = 120;
    vnet *u = UdpHost(VNET_FAMILY_IPV4, &slow);
    int su = vnet_open(u, 0);
    ASSERT_EQ(Send(u, su, "x", to_b), 1);
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_wait(u, 10000), 0) << "nothing receivable; the release is due, though";
    double ms = MsSince(t0);
    EXPECT_GE(ms, 90);
    EXPECT_LT(ms, 3000) << "not the whole 10 s";
    vnet_poll(u);
    std::string d;
    EXPECT_EQ(RecvWait(b, sb, &d), 1);
    vnet_destroy(u);
}

TEST(Wait, LoopbackCannotWait) {
    EnvGuard env;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet *l = LoopbackHost(net);
    EXPECT_EQ(vnet_wait(l, 0), VNET_ENOTSUP);
    EXPECT_EQ(vnet_wait(l, 10), VNET_ENOTSUP);
    vnet_destroy(l);
    vnet_loopback_network_destroy(net);
}

// ------------------------------------------------------ vnet_resolve_host

TEST(ResolveHost, NumericAddressesAndLocalhostNeedNoLookup) {
    vnet_addr a;
    ASSERT_EQ(vnet_resolve_host("1.2.3.4:5000", VNET_FAMILY_ANY, &a), 0);
    EXPECT_EQ(Str(a), "1.2.3.4:5000");
    ASSERT_EQ(vnet_resolve_host("[::1]:80", VNET_FAMILY_ANY, &a), 0);
    EXPECT_EQ(Str(a), "[::1]:80");
    ASSERT_EQ(vnet_resolve_host("localhost:9", VNET_FAMILY_IPV4, &a), 0);
    EXPECT_EQ(Str(a), "127.0.0.1:9");
}

TEST(ResolveHost, ANameGoesThroughTheSystemResolver) {
    vnet_addr a;
    // "LocalHost": not the literal vnet_resolve() knows, but /etc/hosts has it.
    ASSERT_EQ(vnet_resolve_host("LocalHost:4242", VNET_FAMILY_IPV4, &a), 0);
    EXPECT_EQ(Str(a), "127.0.0.1:4242");
    ASSERT_EQ(vnet_resolve_host("LocalHost:4242", VNET_FAMILY_ANY, &a), 0);
    EXPECT_EQ(a.port, 4242);
    EXPECT_TRUE(a.family == VNET_AF_IPV4 || a.family == VNET_AF_IPV6);
}

TEST(ResolveHost, TheResultIsUsableAsAnAddressForCreate) {
    // The recommended pattern: resolve early (or on a worker), pass a numeric address.
    vnet_addr a;
    ASSERT_EQ(vnet_resolve_host("LocalHost:7777", VNET_FAMILY_IPV4, &a), 0);
    char s[64];
    ASSERT_GT(vnet_addr_to_string(&a, s, sizeof s), 0);
    vnet_addr b;
    EXPECT_EQ(vnet_resolve(nullptr, s, &b), 0) << s << " is numeric: it never blocks";
    EXPECT_TRUE(vnet_addr_equal(&a, &b));
}

TEST(ResolveHost, FamilyFilter) {
    vnet_addr a;
    EXPECT_EQ(vnet_resolve_host("[::1]:80", VNET_FAMILY_IPV4, &a), VNET_EAFNOSUPPORT);
    EXPECT_EQ(vnet_resolve_host("1.2.3.4:80", VNET_FAMILY_IPV6, &a), VNET_EAFNOSUPPORT);
    EXPECT_EQ(vnet_resolve_host("1.2.3.4:80", 99, &a), VNET_EINVAL);
}

TEST(ResolveHost, MalformedInputIsInvalid) {
    vnet_addr a;
    for (const char *bad : {"", "nohostport", "host:", ":80", "host.invalid:99999", "host.invalid:12x", "1.2.3:5",
                            "host:5:6", "1.2.3.4:99999"}) {
        EXPECT_EQ(vnet_resolve_host(bad, VNET_FAMILY_ANY, &a), VNET_EINVAL) << "\"" << bad << "\"";
    }
    EXPECT_EQ(vnet_resolve_host(nullptr, VNET_FAMILY_ANY, &a), VNET_EINVAL);
    EXPECT_EQ(vnet_resolve_host("1.2.3.4:5", VNET_FAMILY_ANY, nullptr), VNET_EINVAL);
}

TEST(ResolveHost, AnUnknownNameIsAnIoErrorNotAHang) {
    vnet_addr a;
    auto t0 = Clock::now();
    EXPECT_EQ(vnet_resolve_host("no-such-host.invalid:80", VNET_FAMILY_ANY, &a), VNET_EIO);
    EXPECT_LT(MsSince(t0), 20000);
}

// --------------------------------------------------------- SO_RCVBUF

#if defined(VNET_TEST_FAKE_IPV6)
struct RcvbufReset {
    ~RcvbufReset() {
        g_last_rcvbuf = 0;
        g_fail_rcvbuf = 0;
    }
};

TEST(UdpReceiveBuffer, IsAskedToHold256KiB) {
    EnvGuard env;
    RcvbufReset reset;
    vnet *u = UdpHost();
    ASSERT_GE(vnet_open(u, 0), 0);
    EXPECT_EQ(g_last_rcvbuf, 256 * 1024);
    vnet_destroy(u);
}

TEST(UdpReceiveBuffer, AKernelThatRefusesIsIgnored) {
    EnvGuard env;
    RcvbufReset reset;
    g_fail_rcvbuf = 1;
    std::vector<std::string> log;
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.family = VNET_FAMILY_IPV4;
    cfg.ignore_env = 1;
    cfg.log = [](void *u, int level, const char *m) {
        if (level == VNET_LOG_DEBUG) static_cast<std::vector<std::string> *>(u)->push_back(m);
    };
    cfg.log_user = &log;
    vnet *u = vnet_create(&cfg, nullptr);
    ASSERT_NE(u, nullptr);
    int s = vnet_open(u, 0);
    EXPECT_GE(s, 0) << "a refused SO_RCVBUF is not an error";
    ASSERT_FALSE(log.empty()) << "but it is logged at debug level";
    EXPECT_NE(log[0].find("SO_RCVBUF"), std::string::npos);
    vnet_destroy(u);
}

#endif  // VNET_TEST_FAKE_IPV6

TEST(UdpReceiveBuffer, ABacklogBeyondTheKernelDefaultIsKept) {
    EnvGuard env;
    // The default buffer (212992 bytes) holds ~270 small datagrams; 450 of them
    // arriving before the game polls (one frame hitch at a server's packet rate)
    // fit once the buffer is 256 KiB (if the system allows it, as Linux does).
    vnet *a = UdpHost(), *b = UdpHost();
    int sa = vnet_open(a, 0), sb = vnet_open(b, 0);
    vnet_addr lb;
    vnet_local_addr(b, sb, &lb);
    vnet_addr to = A("127.0.0.1:1");
    to.port = lb.port;
    for (int i = 0; i < 450; i++) ASSERT_EQ(Send(a, sa, "0123456789", to), 10);
    // (macOS delivers loopback datagrams a moment late: wait for the stragglers.)
    int got = 0;
    std::string d;
    while (RecvWait(b, sb, &d, nullptr, 300) >= 0) got++;
    EXPECT_EQ(got, 450);
    vnet_destroy(a);
    vnet_destroy(b);
}

// ------------------------------------------- native backends never wake

struct WakeCount {
    int n = 0;
};
static void CountWake(void *u) { static_cast<WakeCount *>(u)->n++; }

TEST(WakeCallback, NativeBackendsNeverCallIt) {
    EnvGuard env;
    WakeCount wc;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.ignore_env = 1;
    cfg.wake = CountWake;
    cfg.wake_user = &wc;
    cfg.backend = VNET_BACKEND_LOOPBACK;
    cfg.loopback_network = net;
    vnet *l1 = vnet_create(&cfg, nullptr), *l2 = vnet_create(&cfg, nullptr);
    int s1 = vnet_open(l1, 1), s2 = vnet_open(l2, 2);
    vnet_addr to = A("127.1.0.2:2");
    Send(l1, s1, "x", to);
    vnet_loopback_network_step(net, 10);
    std::string d;
    EXPECT_EQ(Recv(l2, s2, &d), 1);
    cfg.backend = VNET_BACKEND_UDP;
    cfg.family = VNET_FAMILY_IPV4;
    vnet *u1 = vnet_create(&cfg, nullptr), *u2 = vnet_create(&cfg, nullptr);
    ASSERT_NE(u2, nullptr);
    int su1 = vnet_open(u1, 0), su2 = vnet_open(u2, 0);
    vnet_addr lu;
    vnet_local_addr(u2, su2, &lu);
    vnet_addr tou = A("127.0.0.1:1");
    tou.port = lu.port;
    Send(u1, su1, "y", tou);
    EXPECT_EQ(RecvWait(u2, su2, &d), 1);
    EXPECT_EQ(wc.n, 0);
    for (vnet *v : {l1, l2, u1, u2}) vnet_destroy(v);
    EXPECT_EQ(wc.n, 0);
    vnet_loopback_network_destroy(net);
}
