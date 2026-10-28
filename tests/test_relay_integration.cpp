// SPDX-License-Identifier: MIT
// Native clients (VNET_BACKEND_RELAY_UDP) against the real Go relay, started
// as a child process on loopback ports. Needs the relay binary: it is built
// by the "vnet-relay" CMake target (so this file is only compiled when Go is
// available).
#include "relay_test_util.h"

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>

#ifndef VNET_RELAY_BIN
#error "VNET_RELAY_BIN must name the vnet-relay binary"
#endif

namespace {

using Clock = std::chrono::steady_clock;
Clock::time_point In(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }

// The relay as a child process. It prints "LISTEN udp=<addr> http=<addr>" once
// it is accepting traffic.
class RelayProc {
public:
    explicit RelayProc(std::vector<std::string> extra = {}) {
        int pipefd[2];
        EXPECT_EQ(pipe(pipefd), 0);
        pid_ = fork();
        if (pid_ == 0) {
            dup2(pipefd[1], 1);
            close(pipefd[0]);
            close(pipefd[1]);
            std::vector<std::string> args = {VNET_RELAY_BIN, "-udp", "127.0.0.1:0", "-http", "127.0.0.1:0"};
            for (auto &e : extra) args.push_back(e);
            std::vector<char *> argv;
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            // A clean environment: a developer's VNET_RELAY_* must not change a test.
            char *envp[] = {const_cast<char *>("PATH=/usr/bin:/bin"), nullptr};
            execve(VNET_RELAY_BIN, argv.data(), envp);
            _exit(127);
        }
        close(pipefd[1]);
        out_ = pipefd[0];
        std::string line;
        auto deadline = In(15000);
        while (Clock::now() < deadline) {
            pollfd p{out_, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            char c;
            if (read(out_, &c, 1) != 1) break;
            if (c == '\n') break;
            line += c;
        }
        // "LISTEN udp=127.0.0.1:1234 http=127.0.0.1:5678"
        auto field = [&](const std::string &key) {
            size_t i = line.find(key + "=");
            if (i == std::string::npos) return std::string();
            i += key.size() + 1;
            return line.substr(i, line.find(' ', i) - i);
        };
        udp_ = field("udp");
        http_ = field("http");
        EXPECT_FALSE(udp_.empty()) << "relay did not start; output: " << line;
    }
    ~RelayProc() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            auto deadline = In(5000);
            int status = 0;
            while (waitpid(pid_, &status, WNOHANG) == 0) {
                if (Clock::now() > deadline) {
                    kill(pid_, SIGKILL);
                    waitpid(pid_, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (out_ >= 0) close(out_);
    }
    const std::string &udp() const { return udp_; }
    uint16_t udp_port() const { return (uint16_t)std::stoi(udp_.substr(udp_.rfind(':') + 1)); }

    // A counter from /stats ("sessions", "rooms_created", ...), or -1.
    long Stat(const std::string &name) const {
        size_t colon = http_.rfind(':');
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)std::stoi(http_.substr(colon + 1)));
        inet_pton(AF_INET, http_.substr(0, colon).c_str(), &a.sin_addr);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, (sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return -1;
        }
        const char req[] = "GET /stats HTTP/1.0\r\n\r\n";
        if (write(fd, req, sizeof req - 1) < 0) {
            close(fd);
            return -1;
        }
        std::string resp;
        char buf[1024];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) resp.append(buf, (size_t)n);
        close(fd);
        size_t i = resp.find("\"" + name + "\":");
        if (i == std::string::npos) return -1;
        return std::atol(resp.c_str() + i + name.size() + 3);
    }
    bool WaitStat(const std::string &name, long want) const {
        auto deadline = In(5000);
        while (Stat(name) != want) {
            if (Clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    pid_t pid_ = -1;
    int out_ = -1;
    std::string udp_, http_;
};

vnet *RelayClientFor(const std::string &relay, const char *room, const vnet_impair *imp = nullptr,
                     vnet_clock_fn clk = nullptr, void *clk_user = nullptr, int timeout_ms = 0) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_UDP;
    cfg.relay = relay.c_str();
    cfg.room = room;
    cfg.ignore_env = 1;
    cfg.relay_timeout_ms = timeout_ms;
    cfg.rx_queue_len = 1024;  // the tests send bursts before they poll
    cfg.clock = clk;
    cfg.clock_user = clk_user;
    if (imp) cfg.impair = *imp;
    int err = 0;
    vnet *v = vnet_create(&cfg, &err);
    EXPECT_NE(v, nullptr) << vnet_strerror(err);
    return v;
}

// Poll v until its state is no longer CONNECTING (bounded).
int Settle(vnet *v, int timeout_ms = 8000) {
    auto deadline = In(timeout_ms);
    for (;;) {
        vnet_poll(v);
        int st = vnet_status(v, nullptr);
        if (st != VNET_STATUS_CONNECTING || Clock::now() > deadline) return st;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::string RoomOf(vnet *v) {
    vnet_status_info st;
    vnet_status(v, &st);
    return st.room;
}

// Pump every context until `got` has n datagrams or the deadline passes.
struct Rx {
    std::vector<std::string> data;
    std::vector<std::string> from;
};
bool Collect(const std::vector<vnet *> &pumps, vnet *v, int s, size_t n, Rx *rx, int timeout_ms = 8000) {
    auto deadline = In(timeout_ms);
    while (rx->data.size() < n) {
        for (vnet *p : pumps) vnet_poll(p);
        std::string d;
        vnet_addr f{};
        int r = Recv(v, s, &d, &f);
        if (r >= 0) {
            rx->data.push_back(d);
            rx->from.push_back(Str(f));
        } else if (Clock::now() > deadline) {
            return false;
        } else if (r == VNET_EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

class RelayIntegration : public VnetTest {};

TEST_F(RelayIntegration, TwoNativeClientsExchangeDatagramsInOrder) {
    RelayProc relay;
    vnet *host = RelayClientFor(relay.udp(), nullptr);
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(Settle(host), VNET_STATUS_READY);
    std::string code = RoomOf(host);
    ASSERT_EQ(code.size(), 6u);
    vnet *guest = RelayClientFor(relay.udp(), code.c_str());
    ASSERT_NE(guest, nullptr);
    ASSERT_EQ(Settle(guest), VNET_STATUS_READY);

    vnet_status_info hs, gs;
    vnet_status(host, &hs);
    vnet_status(guest, &gs);
    EXPECT_EQ(Str(hs.self), "10.0.0.1:0");  // the creator is "room-host"
    EXPECT_EQ(Str(gs.self), "10.0.0.2:0");
    EXPECT_STREQ(gs.room, code.c_str());

    int hsock = vnet_open(host, 5000), gsock = vnet_open(guest, 5001);
    ASSERT_GE(hsock, 0);
    ASSERT_GE(gsock, 0);
    vnet_addr to_host{}, to_guest{};
    ASSERT_EQ(vnet_resolve(guest, "room-host:5000", &to_host), 0);
    ASSERT_EQ(vnet_resolve(host, "10.0.0.2:5001", &to_guest), 0);

    const int N = 200;
    for (int i = 0; i < N; i++) {
        std::string a = "g2h-" + std::to_string(i) + std::string((size_t)(i % 97), 'x');
        std::string b = "h2g-" + std::to_string(i) + std::string((size_t)(i % 89), 'y');
        ASSERT_EQ(Send(guest, gsock, a, to_host), (int)a.size());
        ASSERT_EQ(Send(host, hsock, b, to_guest), (int)b.size());
    }
    Rx at_host, at_guest;
    ASSERT_TRUE(Collect({host, guest}, host, hsock, N, &at_host));
    ASSERT_TRUE(Collect({host, guest}, guest, gsock, N, &at_guest));
    for (int i = 0; i < N; i++) {
        EXPECT_EQ(at_host.data[i], "g2h-" + std::to_string(i) + std::string((size_t)(i % 97), 'x'));
        EXPECT_EQ(at_guest.data[i], "h2g-" + std::to_string(i) + std::string((size_t)(i % 89), 'y'));
        EXPECT_EQ(at_host.from[i], "10.0.0.2:5001");  // sender's virtual address and port
        EXPECT_EQ(at_guest.from[i], "10.0.0.1:5000");
    }
    // Replies go to the right place: answer from the received address.
    vnet_addr who{};
    ASSERT_TRUE(vnet_resolve(host, at_host.from[0].c_str(), &who) == 0);
    ASSERT_EQ(Send(host, hsock, "pong", who), 4);
    Rx pong;
    ASSERT_TRUE(Collect({host, guest}, guest, gsock, 1, &pong));
    EXPECT_EQ(pong.data[0], "pong");

    vnet_stats st;
    vnet_get_stats(host, &st);
    EXPECT_EQ(st.tx_datagrams, (uint64_t)N + 1);
    EXPECT_EQ(st.rx_datagrams, (uint64_t)N);
    vnet_destroy(guest);
    vnet_destroy(host);
}

TEST_F(RelayIntegration, ImpairmentInFrontOfTheRelayBackend) {
    RelayProc relay;
    vnet_impair lossy{};
    lossy.loss_pct = 25;
    lossy.seed = 5;
    vnet *host = RelayClientFor(relay.udp(), nullptr, &lossy);
    ASSERT_EQ(Settle(host), VNET_STATUS_READY);  // the handshake is not impaired
    vnet *guest = RelayClientFor(relay.udp(), RoomOf(host).c_str());
    ASSERT_EQ(Settle(guest), VNET_STATUS_READY);
    int hs = vnet_open(host, 1), gs = vnet_open(guest, 2);
    vnet_addr to_guest = A("10.0.0.2:2");
    const int N = 400;
    // Send in frames of 10 and pump between them, as a game loop would (a
    // single huge burst would overflow the guest's kernel socket buffer).
    Rx rx;
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(Send(host, hs, std::to_string(i), to_guest), (int)std::to_string(i).size());
        if (i % 10 == 9) {
            vnet_poll(host);
            vnet_poll(guest);
            std::string d;
            vnet_addr f{};
            while (Recv(guest, gs, &d, &f) >= 0) rx.data.push_back(d);
        }
    }
    vnet_stats st;
    vnet_get_stats(host, &st);
    ASSERT_GT(st.impair_lost, 0u);
    ASSERT_LT(st.impair_lost, (uint64_t)N);
    size_t want = (size_t)N - (size_t)st.impair_lost;
    ASSERT_TRUE(Collect({host, guest}, guest, gs, want, &rx)) << "got " << rx.data.size() << " of " << want;
    // Exactly the datagrams the impairment layer let through, in order.
    int last = -1;
    for (auto &d : rx.data) {
        int i = std::stoi(d);
        EXPECT_GT(i, last);
        last = i;
    }
    std::string extra;
    for (int k = 0; k < 20; k++) vnet_poll(guest);
    EXPECT_EQ(Recv(guest, gs, &extra), VNET_EAGAIN) << "more datagrams than the impairment layer let through";
    vnet_destroy(guest);
    vnet_destroy(host);
}

TEST_F(RelayIntegration, LatencyIsAppliedBeforeTheRelay) {
    RelayProc relay;
    vnet_impair slow{};
    slow.latency_ms = 150;
    vnet *host = RelayClientFor(relay.udp(), nullptr, &slow);
    ASSERT_EQ(Settle(host), VNET_STATUS_READY);
    vnet *guest = RelayClientFor(relay.udp(), RoomOf(host).c_str());
    ASSERT_EQ(Settle(guest), VNET_STATUS_READY);
    int hs = vnet_open(host, 1), gs = vnet_open(guest, 2);
    auto t0 = Clock::now();
    ASSERT_EQ(Send(host, hs, "slow", A("10.0.0.2:2")), 4);
    Rx rx;
    ASSERT_TRUE(Collect({host, guest}, guest, gs, 1, &rx));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    EXPECT_GE(ms, 150);
    vnet_destroy(guest);
    vnet_destroy(host);
}

TEST_F(RelayIntegration, RoutingStaysInsideEachRoom) {
    RelayProc relay;
    vnet *h1 = RelayClientFor(relay.udp(), nullptr);
    vnet *h2 = RelayClientFor(relay.udp(), nullptr);
    ASSERT_EQ(Settle(h1), VNET_STATUS_READY);
    ASSERT_EQ(Settle(h2), VNET_STATUS_READY);
    ASSERT_NE(RoomOf(h1), RoomOf(h2));
    vnet *g1 = RelayClientFor(relay.udp(), RoomOf(h1).c_str());
    vnet *g2 = RelayClientFor(relay.udp(), RoomOf(h2).c_str());
    ASSERT_EQ(Settle(g1), VNET_STATUS_READY);
    ASSERT_EQ(Settle(g2), VNET_STATUS_READY);
    // Same virtual addresses in both rooms.
    vnet_status_info a, b;
    vnet_status(g1, &a);
    vnet_status(g2, &b);
    ASSERT_EQ(Str(a.self), Str(b.self));

    int sh1 = vnet_open(h1, 7), sg1 = vnet_open(g1, 7), sh2 = vnet_open(h2, 7), sg2 = vnet_open(g2, 7);
    std::vector<vnet *> all = {h1, g1, h2, g2};
    // Room 1's host sends to .2 and also probes every other address.
    ASSERT_EQ(Send(h1, sh1, "room1-only", A("10.0.0.2:7")), 10);
    for (int o = 3; o < 12; o++) Send(h1, sh1, "probe", A(("10.0.0." + std::to_string(o) + ":7").c_str()));
    Send(h1, sh1, "probe", A("8.8.8.8:7"));
    // Then room 2 sends its own marker to its guest.
    ASSERT_EQ(Send(h2, sh2, "room2-marker", A("10.0.0.2:7")), 12);
    Rx r1, r2;
    ASSERT_TRUE(Collect(all, g1, sg1, 1, &r1));
    EXPECT_EQ(r1.data[0], "room1-only");
    ASSERT_TRUE(Collect(all, g2, sg2, 1, &r2));
    // The first thing room 2's guest ever receives is its own room's marker.
    EXPECT_EQ(r2.data[0], "room2-marker");
    for (int k = 0; k < 50; k++)
        for (vnet *p : all) vnet_poll(p);
    std::string extra;
    EXPECT_EQ(Recv(g2, sg2, &extra), VNET_EAGAIN);
    EXPECT_EQ(Recv(g1, sg1, &extra), VNET_EAGAIN);
    EXPECT_EQ(Recv(h2, sh2, &extra), VNET_EAGAIN);
    EXPECT_EQ(Recv(h1, sh1, &extra), VNET_EAGAIN);
    EXPECT_GE(relay.Stat("unroutable"), 9);
    for (vnet *p : all) vnet_destroy(p);
}

TEST_F(RelayIntegration, JoiningAnUnknownRoomFails) {
    RelayProc relay;
    vnet *g = RelayClientFor(relay.udp(), "ABCDEF");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(Settle(g), VNET_STATUS_FAILED);
    vnet_status_info st;
    vnet_status(g, &st);
    EXPECT_EQ(st.error, VNET_ENOROOM);
    vnet_destroy(g);
    EXPECT_EQ(relay.Stat("sessions"), 0);
}

TEST_F(RelayIntegration, FullRoomsAreRefusedAndLeavingFreesTheAddress) {
    RelayProc relay({"-max-members", "3"});
    vnet *h = RelayClientFor(relay.udp(), nullptr);
    ASSERT_EQ(Settle(h), VNET_STATUS_READY);
    std::string code = RoomOf(h);
    vnet *g2 = RelayClientFor(relay.udp(), code.c_str());
    vnet *g3 = RelayClientFor(relay.udp(), code.c_str());
    ASSERT_EQ(Settle(g2), VNET_STATUS_READY);
    ASSERT_EQ(Settle(g3), VNET_STATUS_READY);
    vnet *g4 = RelayClientFor(relay.udp(), code.c_str());
    EXPECT_EQ(Settle(g4), VNET_STATUS_FAILED);
    vnet_status_info st;
    vnet_status(g4, &st);
    EXPECT_EQ(st.error, VNET_EROOMFULL);
    vnet_destroy(g4);

    vnet_status(g2, &st);
    std::string freed = Str(st.self);
    vnet_destroy(g2);  // sends LEAVE
    ASSERT_TRUE(relay.WaitStat("sessions", 2));
    vnet *g5 = RelayClientFor(relay.udp(), code.c_str());
    ASSERT_EQ(Settle(g5), VNET_STATUS_READY);
    vnet_status(g5, &st);
    EXPECT_EQ(Str(st.self), freed);  // the lowest free address is reused
    vnet_destroy(g5);
    vnet_destroy(g3);
    vnet_destroy(h);
    ASSERT_TRUE(relay.WaitStat("rooms", 0));  // the room dies when empty
}

TEST_F(RelayIntegration, UnreachableRelayTimesOutOnVirtualTime) {
    // Nothing listens on this port. The injected clock lets the 10 s default
    // timeout pass without waiting.
    FakeRelay silent;  // a socket that reads nothing back
    TestClockState clk;
    vnet *v = RelayClientFor(silent.addr(), nullptr, nullptr, TestClockFn, &clk);
    ASSERT_NE(v, nullptr);
    for (int i = 0; i < 100 && vnet_status(v, nullptr) == VNET_STATUS_CONNECTING; i++) {
        clk.now += 250;
        vnet_poll(v);
    }
    vnet_status_info st;
    EXPECT_EQ(vnet_status(v, &st), VNET_STATUS_FAILED);
    EXPECT_EQ(st.error, VNET_ETIMEDOUT);
    EXPECT_GE(clk.now, 10000u);
    vnet_destroy(v);
}

// A UDP forwarder that loses the first `drop_replies` datagrams the relay sends
// back, so the client's CREATE reaches the relay several times.
class LossyProxy {
public:
    LossyProxy(uint16_t relay_port, int drop_replies) : drop_(drop_replies) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(fd_, (sockaddr *)&a, sizeof a), 0);
        socklen_t l = sizeof a;
        getsockname(fd_, (sockaddr *)&a, &l);
        port_ = ntohs(a.sin_port);
        up_ = socket(AF_INET, SOCK_DGRAM, 0);
        relay_.sin_family = AF_INET;
        relay_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        relay_.sin_port = htons(relay_port);
        th_ = std::thread([this] { Run(); });
    }
    ~LossyProxy() {
        stop_ = true;
        th_.join();
        close(fd_);
        close(up_);
    }
    uint16_t port() const { return port_; }
    int forwarded_requests() const { return requests_; }
    int dropped() const { return dropped_; }

private:
    void Run() {
        sockaddr_in client{};
        bool have_client = false;
        while (!stop_) {
            pollfd p[2] = {{fd_, POLLIN, 0}, {up_, POLLIN, 0}};
            if (poll(p, 2, 20) <= 0) continue;
            uint8_t buf[2048];
            if (p[0].revents & POLLIN) {
                socklen_t l = sizeof client;
                ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, (sockaddr *)&client, &l);
                if (n > 0) {
                    have_client = true;
                    requests_++;
                    sendto(up_, buf, (size_t)n, 0, (sockaddr *)&relay_, sizeof relay_);
                }
            }
            if (p[1].revents & POLLIN) {
                ssize_t n = recv(up_, buf, sizeof buf, 0);
                if (n > 0 && have_client) {
                    if (dropped_ < drop_) {
                        dropped_++;
                    } else {
                        sendto(fd_, buf, (size_t)n, 0, (sockaddr *)&client, sizeof client);
                    }
                }
            }
        }
    }
    int fd_, up_;
    uint16_t port_;
    sockaddr_in relay_{};
    int drop_;
    std::atomic<int> requests_{0}, dropped_{0};
    std::atomic<bool> stop_{false};
    std::thread th_;
};

TEST_F(RelayIntegration, LostRepliesCauseRetriesButNeverASecondRoomOrMember) {
    RelayProc relay;
    LossyProxy proxy(relay.udp_port(), 2);  // the first two ROOM replies are lost
    std::string via = "127.0.0.1:" + std::to_string(proxy.port());
    vnet *host = RelayClientFor(via, nullptr);
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(Settle(host), VNET_STATUS_READY);  // after ~250 + 500 ms of real retries
    EXPECT_EQ(proxy.dropped(), 2);
    EXPECT_GE(proxy.forwarded_requests(), 3);
    EXPECT_EQ(relay.Stat("rooms_created"), 1) << "a retried CREATE_ROOM must not create another room";
    EXPECT_EQ(relay.Stat("sessions"), 1);
    EXPECT_GE(relay.Stat("handshake_retries"), 2);
    vnet_status_info st;
    vnet_status(host, &st);
    EXPECT_EQ(Str(st.self), "10.0.0.1:0");

    // The same for JOIN_ROOM: the joiner takes one address, not several.
    LossyProxy proxy2(relay.udp_port(), 2);
    std::string via2 = "127.0.0.1:" + std::to_string(proxy2.port());
    vnet *guest = RelayClientFor(via2, st.room);
    ASSERT_NE(guest, nullptr);
    ASSERT_EQ(Settle(guest), VNET_STATUS_READY);
    vnet_status_info gs;
    vnet_status(guest, &gs);
    EXPECT_EQ(Str(gs.self), "10.0.0.2:0");
    EXPECT_EQ(relay.Stat("sessions"), 2);
    vnet *third = RelayClientFor(relay.udp(), st.room);
    ASSERT_EQ(Settle(third), VNET_STATUS_READY);
    vnet_status(third, &gs);
    EXPECT_EQ(Str(gs.self), "10.0.0.3:0") << "the retried JOIN used up an address";
    vnet_destroy(third);
    vnet_destroy(guest);
    vnet_destroy(host);
}

TEST_F(RelayIntegration, SessionSurvivesNatRebindingBecauseTheTokenIsTheIdentity) {
    // Two relay clients behind a forwarder that changes its source port in
    // the middle of the session (what a NAT mapping change looks like).
    RelayProc relay;
    vnet *host = RelayClientFor(relay.udp(), nullptr);
    ASSERT_EQ(Settle(host), VNET_STATUS_READY);
    // The guest's traffic goes through a forwarder whose upstream socket we replace.
    struct Fwd {
        int client_side, up;
        sockaddr_in relay{}, client{};
        bool have = false;
    } f;
    f.client_side = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(f.client_side, (sockaddr *)&a, sizeof a), 0);
    socklen_t l = sizeof a;
    getsockname(f.client_side, (sockaddr *)&a, &l);
    uint16_t fport = ntohs(a.sin_port);
    f.up = socket(AF_INET, SOCK_DGRAM, 0);
    f.relay.sin_family = AF_INET;
    f.relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f.relay.sin_port = htons(relay.udp_port());

    std::atomic<bool> stop{false};
    std::mutex mu;
    std::thread th([&] {
        while (!stop) {
            std::lock_guard<std::mutex> lk(mu);
            pollfd p[2] = {{f.client_side, POLLIN, 0}, {f.up, POLLIN, 0}};
            if (poll(p, 2, 0) <= 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            uint8_t buf[2048];
            if (p[0].revents & POLLIN) {
                socklen_t cl = sizeof f.client;
                ssize_t n = recvfrom(f.client_side, buf, sizeof buf, 0, (sockaddr *)&f.client, &cl);
                if (n > 0) {
                    f.have = true;
                    sendto(f.up, buf, (size_t)n, 0, (sockaddr *)&f.relay, sizeof f.relay);
                }
            }
            if (p[1].revents & POLLIN) {
                ssize_t n = recv(f.up, buf, sizeof buf, 0);
                if (n > 0 && f.have) sendto(f.client_side, buf, (size_t)n, 0, (sockaddr *)&f.client, sizeof f.client);
            }
        }
    });
    std::string via = "127.0.0.1:" + std::to_string(fport);
    vnet *guest = RelayClientFor(via, RoomOf(host).c_str());
    ASSERT_EQ(Settle(guest), VNET_STATUS_READY);
    int hs = vnet_open(host, 1), gs = vnet_open(guest, 2);
    ASSERT_EQ(Send(host, hs, "before", A("10.0.0.2:2")), 6);
    Rx rx;
    ASSERT_TRUE(Collect({host, guest}, guest, gs, 1, &rx));
    EXPECT_EQ(rx.data[0], "before");

    {  // The mapping changes: a new upstream socket, hence a new source port at the relay.
        std::lock_guard<std::mutex> lk(mu);
        close(f.up);
        f.up = socket(AF_INET, SOCK_DGRAM, 0);
    }
    // Traffic from the guest re-registers it (any authenticated frame does).
    ASSERT_EQ(Send(guest, gs, "hello-from-new-port", A("10.0.0.1:1")), 19);
    Rx at_host;
    ASSERT_TRUE(Collect({host, guest}, host, hs, 1, &at_host));
    ASSERT_EQ(Send(host, hs, "after", A("10.0.0.2:2")), 5);
    ASSERT_TRUE(Collect({host, guest}, guest, gs, 2, &rx));
    EXPECT_EQ(rx.data[1], "after");  // the relay now sends to the guest's new address
    EXPECT_EQ(vnet_status(guest, nullptr), VNET_STATUS_READY);
    stop = true;
    th.join();
    vnet_destroy(guest);
    vnet_destroy(host);
    close(f.client_side);
    close(f.up);
}

}  // namespace
