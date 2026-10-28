// SPDX-License-Identifier: MIT
// Helpers for the relay-client tests: a scripted fake relay on a plain UDP
// socket, frame builders, and a controllable clock.
#ifndef VNET_RELAY_TEST_UTIL_H
#define VNET_RELAY_TEST_UTIL_H

#include "test_util.h"

extern "C" {
#include "relay_proto.h"
#include "vnet_internal.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using Bytes = std::vector<uint8_t>;

inline Bytes B(std::initializer_list<int> l) {
    Bytes b;
    for (int x : l) b.push_back((uint8_t)x);
    return b;
}

inline Bytes Cat(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

inline Bytes Str2Bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

// A relay stand-in: one UDP socket on 127.0.0.1 that the test reads and
// writes by hand. Presence is checked with a bounded wait; absence with a
// non-blocking read (loopback delivery is complete when sendto() returns).
class FakeRelay {
public:
    FakeRelay() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        int ok = bind(fd_, (sockaddr *)&a, sizeof a);
        EXPECT_EQ(ok, 0);
        socklen_t l = sizeof a;
        getsockname(fd_, (sockaddr *)&a, &l);
        port_ = ntohs(a.sin_port);
    }
    ~FakeRelay() { close(fd_); }
    uint16_t port() const { return port_; }
    std::string addr() const { return "127.0.0.1:" + std::to_string(port_); }

    // Wait up to timeout_ms for a datagram; remembers who sent it.
    bool Recv(Bytes *out, int timeout_ms = 3000) {
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        uint8_t buf[4096];
        socklen_t l = sizeof from_;
        ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, (sockaddr *)&from_, &l);
        if (n < 0) return false;
        out->assign(buf, buf + n);
        have_client_ = true;
        return true;
    }
    bool TryRecv(Bytes *out) { return Recv(out, 0); }
    int Drain() {
        Bytes b;
        int n = 0;
        while (TryRecv(&b)) n++;
        return n;
    }
    bool RecvType(uint8_t type, Bytes *out) {
        for (int i = 0; i < 1000; i++) {
            if (!Recv(out)) return false;
            if (!out->empty() && (*out)[0] == type) return true;
        }
        return false;
    }

    // Send to the client (the last source seen), or from a different socket.
    void Send(const Bytes &b) {
        ASSERT_TRUE(have_client_);
        sendto(fd_, b.data(), b.size(), 0, (sockaddr *)&from_, sizeof from_);
    }
    sockaddr_in client() const { return from_; }
    // A third party sends to the client's socket.
    void SpoofFrom(const Bytes &b) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        sendto(s, b.data(), b.size(), 0, (sockaddr *)&from_, sizeof from_);
        close(s);
    }

private:
    int fd_;
    uint16_t port_;
    sockaddr_in from_{};
    bool have_client_ = false;
};

inline Bytes RoomFrame(const Bytes &request, int octet, const char *code, int max_members = 8,
                       int keepalive_s = 5, uint8_t token_fill = 0xA0) {
    Bytes f;
    f.push_back(RELAY_T_ROOM);
    f.push_back(RELAY_PROTO_VERSION);
    f.insert(f.end(), request.begin() + 2, request.begin() + 2 + RELAY_NONCE_LEN);
    f.push_back(10), f.push_back(0), f.push_back(0), f.push_back((uint8_t)octet);
    for (int i = 0; i < RELAY_TOKEN_LEN; i++) f.push_back((uint8_t)(token_fill + i));
    for (int i = 0; i < RELAY_CODE_LEN; i++) f.push_back((uint8_t)code[i]);
    f.push_back((uint8_t)max_members);
    f.push_back((uint8_t)keepalive_s);
    return f;
}

inline Bytes ErrorFrame(const Bytes &request, int reason) {
    Bytes f{RELAY_T_ERROR, RELAY_PROTO_VERSION};
    if (request.size() >= 2 + RELAY_NONCE_LEN)
        f.insert(f.end(), request.begin() + 2, request.begin() + 2 + RELAY_NONCE_LEN);
    else
        f.insert(f.end(), RELAY_NONCE_LEN, 0);
    f.push_back((uint8_t)reason);
    return f;
}

// Relay-to-client DATA: type, src vaddr, dst port, src port, payload.
inline Bytes DataToClient(int src_octet, uint16_t dport, uint16_t sport, const Bytes &payload) {
    Bytes f{RELAY_T_DATA, 10, 0, 0, (uint8_t)src_octet, (uint8_t)(dport >> 8), (uint8_t)dport,
            (uint8_t)(sport >> 8), (uint8_t)sport};
    return Cat(f, payload);
}

// A clock the test moves by hand.
struct TestClockState {
    uint64_t now = 1000;
};
inline uint64_t TestClockFn(void *u) { return static_cast<TestClockState *>(u)->now; }

inline vnet_config RelayUdpConfig(const FakeRelay &r, TestClockState *clk, const char *room = nullptr) {
    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_UDP;
    cfg.relay = nullptr;  // caller sets (the string must outlive the call)
    cfg.room = room;
    cfg.ignore_env = 1;
    cfg.clock = TestClockFn;
    cfg.clock_user = clk;
    (void)r;
    return cfg;
}

#endif
