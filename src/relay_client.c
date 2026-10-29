// SPDX-License-Identifier: MIT
// Relay client core: the vnet backend for VNET_BACKEND_RELAY_UDP / _WS. It
// owns the protocol (framing, handshake, keepalives, session state), the
// local socket table and the receive queues; a transport (relay_udp.c,
// relay_ws.c) only moves whole frames. See doco/relay-protocol.md.
#include "relay_proto.h"
#include "vnet_internal.h"

#include <stdlib.h>
#include <string.h>

#define EPHEMERAL_FIRST 49152u
#define DEFAULT_RX_QUEUE 128
#define DEFAULT_HANDSHAKE_TIMEOUT_MS 10000u
#define DEFAULT_KEEPALIVE_MS 5000u
#define HANDSHAKE_RETRY_FIRST_MS 250u
#define HANDSHAKE_RETRY_MAX_MS 1000u
#define MAX_FRAMES_PER_POLL 2048
#define KEEPALIVE_MIN_MS 100
#define KEEPALIVE_MAX_MS 5000  /* a third of the relay's default 15 s session timeout */
// A gap between two vnet_poll() calls longer than the keepalive interval (but
// never less than this) is taken to be the application being suspended, not
// the relay going silent. The floor keeps a game that polls slower than a very
// short keepalive from switching the silence check off altogether.
#define SUSPEND_GAP_FLOOR_MS 1000u

typedef struct rc_pkt {
    size_t len;
    vnet_addr from;
    uint8_t data[VNET_MAX_DATAGRAM];
} rc_pkt;

typedef struct rc_sock {
    int used;
    uint16_t port;
    int cap, head, count;
    rc_pkt *q;
} rc_sock;

enum { RC_HANDSHAKE, RC_PROBE, RC_READY, RC_FAILED };

typedef struct rc_state {
    const relay_transport_ops *ops;
    void *tp;
    int udp_style;
    int phase;
    int error;  // VNET_E*, when FAILED

    // The request. The nonce makes CREATE/JOIN idempotent: every retry
    // repeats it, so the relay never makes a second member or room.
    uint8_t nonce[RELAY_NONCE_LEN];
    int is_join;
    char want_code[RELAY_CODE_LEN + 1];
    int hs_sent;
    uint8_t hs_cookie[RELAY_HS_COOKIE_LEN];  // from RETRY (UDP): proves our address to the relay
    uint64_t hs_next;      // when to resend (UDP style)
    unsigned hs_backoff;

    // The session, once in a room.
    uint8_t token[RELAY_TOKEN_LEN];
    uint8_t octet;
    char code[RELAY_CODE_LEN + 1];

    uint64_t t_start;      // creation time: the handshake deadline counts from here
    unsigned hs_timeout_ms;
    unsigned cfg_keepalive_ms;  // as configured (0: the relay's suggestion)
    unsigned keepalive_ms;

    // Datagram-size probe (transports that cannot promise a full-size datagram).
    uint64_t probe_next, probe_deadline;

    // Fallback (VNET_BACKEND_RELAY_AUTO): if the first transport fails before
    // the room is entered, fb opens the next one and the handshake starts over.
    relay_fallback_fn fb;  // NULL when there is none (or once it has been used)
    void *fb_arg;
    unsigned primary_timeout_ms;
    uint64_t stage_start;
    uint64_t ka_next;
    uint64_t last_rx;
    uint64_t last_poll;  // when rc_poll last ran (a gap here is the application sleeping)

    // Status: the latest KEEPALIVE round trip and the room's size limit.
    uint64_t ka_sent_at;
    int ka_pending;      // a KEEPALIVE is out and its answer has not come
    int rtt_ms;          // -1 until an answer has been timed
    int max_members;     // from ROOM; 0 before the room is entered

    rc_sock *socks;
    int nsocks;
    int rx_cap;
    uint16_t next_eph;
} rc_state;

// ---------------------------------------------------------------- helpers

static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// Upper-case a room code and check it against the alphabet.
static int normalise_code(const char *in, char out[RELAY_CODE_LEN + 1]) {
    if (strlen(in) != RELAY_CODE_LEN) return -1;
    for (int i = 0; i < RELAY_CODE_LEN; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == 0 || !memchr(kAlphabet, c, sizeof kAlphabet - 1)) return -1;
        out[i] = c;
    }
    out[RELAY_CODE_LEN] = 0;
    return 0;
}

static void fail(vnet *v, rc_state *st, int err) {
    if (st->phase == RC_FAILED) return;
    st->phase = RC_FAILED;
    st->error = err;
    vnet__logf(v, VNET_LOG_WARN, "relay session failed: %s", vnet_strerror(err));
}

static int error_from_reason(int reason) {
    switch (reason) {
        case RELAY_E_ROOM_NOT_FOUND: return VNET_ENOROOM;
        case RELAY_E_ROOM_FULL: return VNET_EROOMFULL;
        case RELAY_E_LIMIT:
        case RELAY_E_RATE: return VNET_ELIMIT;
        case RELAY_E_NO_SESSION:
        case RELAY_E_CLOSED:
        case RELAY_E_TIMEOUT: return VNET_ECLOSED;
        default: return VNET_EPROTO;  // BAD_VERSION, MALFORMED, NONCE_REUSED, ALREADY_IN_ROOM, unknown
    }
}

static void put16(uint8_t *p, uint16_t x) {
    p[0] = (uint8_t)(x >> 8);
    p[1] = (uint8_t)x;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static rc_sock *get_sock(rc_state *st, int s) {
    if (s < 0 || s >= st->nsocks || !st->socks[s].used) return NULL;
    return &st->socks[s];
}

// ----------------------------------------------------------------- frames

static void send_handshake(vnet *v, rc_state *st) {
    uint8_t buf[RELAY_MIN_UDP_HANDSHAKE];
    memset(buf, 0, sizeof buf);
    size_t n = 0;
    buf[n++] = st->is_join ? RELAY_T_JOIN : RELAY_T_CREATE;
    buf[n++] = RELAY_PROTO_VERSION;
    memcpy(buf + n, st->nonce, RELAY_NONCE_LEN);
    n += RELAY_NONCE_LEN;
    if (st->is_join) {
        memcpy(buf + n, st->want_code, RELAY_CODE_LEN);
        n += RELAY_CODE_LEN;
    }
    if (st->udp_style) {
        // The relay keeps no state for a source that has not shown it receives
        // there: the first request draws a RETRY with a cookie, and the repeat
        // carries it (zeros until then). The datagram is padded so that every
        // reply to it is smaller.
        memcpy(buf + n, st->hs_cookie, RELAY_HS_COOKIE_LEN);
        n = RELAY_MIN_UDP_HANDSHAKE;
    }
    int rc = st->ops->send(st->tp, buf, n);
    if (rc < 0) vnet__logf(v, VNET_LOG_DEBUG, "handshake send failed (%d)", rc);
}

// A frame with only the type (WebSocket) or type and token (UDP).
static void send_simple(vnet *v, rc_state *st, uint8_t type) {
    uint8_t buf[1 + RELAY_TOKEN_LEN];
    size_t n = 0;
    buf[n++] = type;
    if (st->udp_style) {
        memcpy(buf + n, st->token, RELAY_TOKEN_LEN);
        n += RELAY_TOKEN_LEN;
    }
    int rc = st->ops->send(st->tp, buf, n);
    if (rc < 0) vnet__logf(v, VNET_LOG_DEBUG, "frame 0x%02x send failed (%d)", type, rc);
}

// A KEEPALIVE, timed: the relay answers it with a one-byte KEEPALIVE, and the
// time between the two is the round trip to the relay.
static void send_keepalive(vnet *v, rc_state *st, uint64_t now) {
    send_simple(v, st, RELAY_T_KEEPALIVE);
    st->ka_sent_at = now;
    st->ka_pending = 1;
}

static void queue_data(vnet *v, rc_state *st, const uint8_t *p, size_t n) {
    if (n < RELAY_DATA_HDR_WS) return;  // malformed
    size_t plen = n - RELAY_DATA_HDR_WS;
    if (plen > VNET_MAX_DATAGRAM) {
        v->stats.rx_oversize++;
        return;
    }
    if (p[1] != 10 || p[2] != 0 || p[3] != 0) return;  // not a room address: never from the relay
    uint16_t dport = get16(p + 5), sport = get16(p + 7);
    rc_sock *sk = NULL;
    for (int i = 0; i < st->nsocks; i++)
        if (st->socks[i].used && st->socks[i].port == dport) {
            sk = &st->socks[i];
            break;
        }
    if (!sk) {
        v->stats.unroutable++;
        return;
    }
    if (sk->count == sk->cap) {
        v->stats.rx_overflow++;
        return;
    }
    rc_pkt *q = &sk->q[(sk->head + sk->count) % sk->cap];
    sk->count++;
    q->len = plen;
    if (plen) memcpy(q->data, p + RELAY_DATA_HDR_WS, plen);
    memset(&q->from, 0, sizeof q->from);
    q->from.family = VNET_AF_IPV4;
    q->from.bytes[0] = 10;
    q->from.bytes[3] = p[4];
    q->from.port = sport;
}

// ------------------------------------------------- datagram-size probe

static int transport_may_truncate(const rc_state *st) {
    return st->ops->max_data_frame && st->ops->max_data_frame(st->tp) < RELAY_WT_DATA_MAX;
}

static void enter_ready(rc_state *st, uint64_t now) {
    st->phase = RC_READY;
    st->ka_next = now + st->keepalive_ms;
    st->ka_pending = 0;
}

static uint8_t probe_byte(size_t i, uint8_t octet) { return (uint8_t)(i * 7u + octet + 0x5au); }

// A full-size DATA frame to ourselves (port 0: no socket has it), through the
// relay. If it comes back, a VNET_MAX_DATAGRAM datagram fits both ways.
static void send_probe(vnet *v, rc_state *st) {
    uint8_t frame[RELAY_MAX_FRAME];
    size_t n = 0;
    frame[n++] = RELAY_T_DATA;
    if (st->udp_style) {
        memcpy(frame + n, st->token, RELAY_TOKEN_LEN);
        n += RELAY_TOKEN_LEN;
    }
    frame[n++] = 10;
    frame[n++] = 0;
    frame[n++] = 0;
    frame[n++] = st->octet;
    put16(frame + n, RELAY_PROBE_PORT);
    n += 2;
    put16(frame + n, RELAY_PROBE_PORT);
    n += 2;
    for (size_t i = 0; i < VNET_MAX_DATAGRAM; i++) frame[n++] = probe_byte(i, st->octet);
    int rc = st->ops->send(st->tp, frame, n);
    if (rc < 0) vnet__logf(v, VNET_LOG_DEBUG, "datagram-size probe send failed (%d)", rc);
}

static void check_probe(vnet *v, rc_state *st, const uint8_t *p, size_t n, uint64_t now) {
    if (n != RELAY_WT_DATA_MAX || p[1] != 10 || p[2] != 0 || p[3] != 0 || p[4] != st->octet ||
        get16(p + 5) != RELAY_PROBE_PORT || get16(p + 7) != RELAY_PROBE_PORT)
        return;  // some other member's datagram, or not ours
    for (size_t i = 0; i < VNET_MAX_DATAGRAM; i++)
        if (p[RELAY_DATA_HDR_WS + i] != probe_byte(i, st->octet)) return;
    st->last_rx = now;
    vnet__logf(v, VNET_LOG_INFO, "a %d byte datagram fits over %s", (int)VNET_MAX_DATAGRAM, st->ops->name);
    enter_ready(st, now);
}

// ------------------------------------------------------------- fallback

// Start (or restart) the handshake with a fresh nonce.
static int begin_handshake(rc_state *st) {
    if (vnet__random(st->nonce, sizeof st->nonce) != 0) return -1;
    st->hs_sent = 0;
    memset(st->hs_cookie, 0, sizeof st->hs_cookie);
    st->hs_backoff = 0;
    st->hs_next = 0;
    return 0;
}

// The first transport did not work out. Close it (a room or member it made
// goes with the connection), open the fallback and handshake again on it,
// with a new nonce: the relay would answer the old one with NONCE_REUSED.
static void switch_transport(vnet *v, rc_state *st, const char *why, uint64_t now) {
    relay_fallback_fn fb = st->fb;
    st->fb = NULL;  // once
    vnet__logf(v, VNET_LOG_INFO, "%s did not work (%s): falling back", st->ops->name, why);
    if (st->tp) st->ops->destroy(st->tp);
    st->tp = NULL;
    const relay_transport_ops *ops = NULL;
    void *tp = NULL;
    int rc = fb(v, st->fb_arg, &ops, &tp);
    if (rc != VNET_OK) {
        fail(v, st, rc == VNET_ENOTSUP ? VNET_ECONNREFUSED : rc);
        return;
    }
    st->ops = ops;
    st->tp = tp;
    st->udp_style = (ops->flags & RELAY_TPF_UDP_STYLE) != 0;
    st->phase = RC_HANDSHAKE;
    st->octet = 0;
    memset(st->token, 0, sizeof st->token);
    st->code[0] = 0;
    st->keepalive_ms = st->cfg_keepalive_ms;
    st->ka_pending = 0;
    st->rtt_ms = -1;
    st->max_members = 0;
    st->t_start = now;  // the fallback gets the whole handshake timeout
    st->last_rx = now;
    if (begin_handshake(st) != 0) fail(v, st, VNET_EIO);
}

static void handle_frame(vnet *v, rc_state *st, const uint8_t *p, size_t n, uint64_t now) {
    if (n == 0) return;
    switch (p[0]) {
        case RELAY_T_ROOM:
            if (n != RELAY_ROOM_LEN || p[1] != RELAY_PROTO_VERSION ||
                memcmp(p + 2, st->nonce, RELAY_NONCE_LEN) != 0)
                return;  // not the answer to our request
            st->last_rx = now;
            if (st->phase != RC_HANDSHAKE) return;  // a repeat of a ROOM we already have
            if (p[10] != 10 || p[11] != 0 || p[12] != 0 || p[13] == 0) {
                fail(v, st, VNET_EPROTO);
                return;
            }
            st->octet = p[13];
            memcpy(st->token, p + 14, RELAY_TOKEN_LEN);
            memcpy(st->code, p + 14 + RELAY_TOKEN_LEN, RELAY_CODE_LEN);
            st->code[RELAY_CODE_LEN] = 0;
            st->max_members = p[14 + RELAY_TOKEN_LEN + RELAY_CODE_LEN];
            if (st->keepalive_ms == 0) {
                unsigned s = p[14 + RELAY_TOKEN_LEN + RELAY_CODE_LEN + 1];
                st->keepalive_ms = s ? s * 1000u : DEFAULT_KEEPALIVE_MS;
                if (st->keepalive_ms > KEEPALIVE_MAX_MS) st->keepalive_ms = KEEPALIVE_MAX_MS;  // never let a relay's suggestion outlast its own timeout
            }
            vnet__logf(v, VNET_LOG_INFO, "in room %s as 10.0.0.%u over %s", st->code, st->octet, st->ops->name);
            if (transport_may_truncate(st)) {
                // The transport cannot promise a full-size datagram: find out
                // what really fits before the game is told it can send.
                st->phase = RC_PROBE;
                st->probe_deadline = now + RELAY_PROBE_TIMEOUT_MS;
                send_probe(v, st);
                st->probe_next = now + RELAY_PROBE_RETRY_MS;
            } else {
                enter_ready(st, now);
            }
            break;
        case RELAY_T_ERROR: {
            if (n != RELAY_ERROR_LEN) return;
            int reason = p[10];
            if (st->phase == RC_HANDSHAKE) {
                // Only an answer to our own request ends the handshake.
                if (memcmp(p + 2, st->nonce, RELAY_NONCE_LEN) == 0) fail(v, st, error_from_reason(reason));
            } else if (st->phase == RC_READY) {
                // The relay no longer knows this session.
                if (reason == RELAY_E_NO_SESSION || reason == RELAY_E_CLOSED || reason == RELAY_E_TIMEOUT)
                    fail(v, st, VNET_ECLOSED);
            }
            break;
        }
        case RELAY_T_KEEPALIVE:
            if (n == 1 && st->phase == RC_READY) {
                st->last_rx = now;
                if (st->ka_pending) {
                    st->ka_pending = 0;
                    st->rtt_ms = now >= st->ka_sent_at ? (int)(now - st->ka_sent_at) : 0;
                }
            }
            break;
        case RELAY_T_RETRY:
            // Stateless retry: the relay wants proof that we receive at our
            // address before it creates anything. Repeat the request with the
            // cookie. (Only from the relay's address: the transport checks.)
            if (n == RELAY_RETRY_LEN && p[1] == RELAY_PROTO_VERSION && st->phase == RC_HANDSHAKE &&
                st->udp_style && memcmp(p + 2, st->nonce, RELAY_NONCE_LEN) == 0) {
                memcpy(st->hs_cookie, p + 2 + RELAY_NONCE_LEN, RELAY_HS_COOKIE_LEN);
                st->last_rx = now;
                send_handshake(v, st);
            }
            break;
        case RELAY_T_PATH_CHALLENGE:
            // The relay saw a frame of ours from an address it does not know
            // (a NAT rebinding) and asks us to prove we receive here. Only
            // answer from the relay's address (the transport checks), and only
            // in a room; the answer goes out from the same socket, hence from
            // the new address.
            if (n == RELAY_CHALLENGE_LEN && st->phase == RC_READY && st->udp_style) {
                st->last_rx = now;
                uint8_t r[RELAY_RESPONSE_LEN];
                r[0] = RELAY_T_PATH_RESPONSE;
                memcpy(r + 1, st->token, RELAY_TOKEN_LEN);
                memcpy(r + 1 + RELAY_TOKEN_LEN, p + 1, RELAY_COOKIE_LEN);
                st->ops->send(st->tp, r, sizeof r);
            }
            break;
        case RELAY_T_DATA:
            if (st->phase == RC_PROBE) {
                check_probe(v, st, p, n, now);
                return;
            }
            if (st->phase != RC_READY) return;
            st->last_rx = now;
            queue_data(v, st, p, n);
            break;
        default:
            break;  // unknown frame types are ignored (forward compatibility)
    }
}

// ---------------------------------------------------------------- backend

static int rc_open(vnet *v, uint16_t port) {
    rc_state *st = v->bs;
    int slot = -1;
    for (int i = 0; i < st->nsocks; i++)
        if (!st->socks[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return VNET_ENOSOCK;
    if (port == 0) {
        unsigned span = 65536u - EPHEMERAL_FIRST;
        unsigned cand = st->next_eph, tries = 0;
        for (;;) {
            int taken = 0;
            for (int i = 0; i < st->nsocks; i++)
                if (st->socks[i].used && st->socks[i].port == cand) taken = 1;
            if (!taken || tries == span) break;
            cand = EPHEMERAL_FIRST + (cand - EPHEMERAL_FIRST + 1) % span;
            tries++;
        }
        if (tries == span) return VNET_EADDRINUSE;
        port = (uint16_t)cand;
        st->next_eph = (uint16_t)(EPHEMERAL_FIRST + (cand - EPHEMERAL_FIRST + 1) % span);
    } else {
        for (int i = 0; i < st->nsocks; i++)
            if (st->socks[i].used && st->socks[i].port == port) return VNET_EADDRINUSE;
    }
    rc_sock *sk = &st->socks[slot];
    sk->q = calloc((size_t)st->rx_cap, sizeof *sk->q);
    if (!sk->q) return VNET_ENOMEM;
    sk->used = 1;
    sk->port = port;
    sk->cap = st->rx_cap;
    sk->head = sk->count = 0;
    return slot;
}

static void rc_close(vnet *v, int s) {
    rc_state *st = v->bs;
    rc_sock *sk = get_sock(st, s);
    if (!sk) return;
    free(sk->q);
    memset(sk, 0, sizeof *sk);
}

static int rc_local_addr(vnet *v, int s, vnet_addr *out) {
    rc_state *st = v->bs;
    rc_sock *sk = get_sock(st, s);
    if (!sk) return VNET_EBADF;
    if (st->phase != RC_READY) return VNET_ENOTCONN;
    memset(out, 0, sizeof *out);
    out->family = VNET_AF_IPV4;
    out->bytes[0] = 10;
    out->bytes[3] = st->octet;
    out->port = sk->port;
    return VNET_OK;
}

static int rc_check_send(vnet *v, int s, const vnet_addr *to) {
    rc_state *st = v->bs;
    if (!get_sock(st, s)) return VNET_EBADF;
    if (to->family != VNET_AF_IPV4) return VNET_EAFNOSUPPORT;
    if (st->phase != RC_READY) return VNET_ENOTCONN;
    return VNET_OK;
}

static int rc_send(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    rc_state *st = v->bs;
    rc_sock *sk = get_sock(st, s);
    if (!sk) return VNET_EBADF;
    if (to->family != VNET_AF_IPV4) return VNET_EAFNOSUPPORT;
    if (st->phase != RC_READY) return VNET_ENOTCONN;

    uint8_t frame[RELAY_MAX_FRAME];
    size_t n = 0;
    frame[n++] = RELAY_T_DATA;
    if (st->udp_style) {
        memcpy(frame + n, st->token, RELAY_TOKEN_LEN);
        n += RELAY_TOKEN_LEN;
    }
    memcpy(frame + n, to->bytes, 4);
    n += 4;
    put16(frame + n, to->port);
    n += 2;
    put16(frame + n, sk->port);
    n += 2;
    if (len) memcpy(frame + n, buf, len);
    n += len;
    int rc = st->ops->send(st->tp, frame, n);
    if (rc < 0) return rc;
    return (int)len;
}

static int rc_recv(vnet *v, int s, void *buf, size_t cap, vnet_addr *from) {
    rc_state *st = v->bs;
    rc_sock *sk = get_sock(st, s);
    if (!sk) return VNET_EBADF;
    if (sk->count == 0) return VNET_EAGAIN;
    rc_pkt *p = &sk->q[sk->head];
    sk->head = (sk->head + 1) % sk->cap;
    sk->count--;
    if (p->len > cap) return VNET_EMSGSIZE;  // consumed, never truncated
    if (p->len) memcpy(buf, p->data, p->len);
    *from = p->from;
    return (int)p->len;
}

// The application did not call vnet_poll() for `gap` ms: a hidden browser tab,
// a laptop that slept, a debugger. That says nothing about the relay, which
// keeps the session alive for as long as the connection answers its pings, so
// the clocks that judge the relay (the handshake deadline, the primary
// transport's deadline, the probe, three silent keepalive intervals) are moved
// forward by the gap instead of being found expired. In a room that is the
// silence clock, and the KEEPALIVE that falls due during any gap longer than an
// interval goes out in the same poll: if the relay really did forget us, its
// answer (an error) says so now rather than after three more intervals.
static void resume_after_gap(rc_state *st, uint64_t gap, uint64_t now) {
    switch (st->phase) {
        case RC_HANDSHAKE:
        case RC_PROBE:
            st->t_start += gap;
            st->stage_start += gap;
            st->probe_deadline += gap;
            st->probe_next += gap;
            break;
        case RC_READY:
            st->last_rx = now;
            break;
        default:
            break;
    }
}

static void rc_poll(vnet *v) {
    rc_state *st = v->bs;
    if (st->phase == RC_FAILED) return;
    uint64_t now = vnet__now(v);

    {
        uint64_t gap = now > st->last_poll ? now - st->last_poll : 0;
        uint64_t limit = st->keepalive_ms ? st->keepalive_ms : DEFAULT_KEEPALIVE_MS;
        if (limit < SUSPEND_GAP_FLOOR_MS) limit = SUSPEND_GAP_FLOOR_MS;
        if (gap > limit) {
            vnet__logf(v, VNET_LOG_INFO, "no vnet_poll() for %u ms: treated as the application being suspended",
                       (unsigned)(gap > 0xffffffffu ? 0xffffffffu : gap));
            resume_after_gap(st, gap, now);
        }
        st->last_poll = now;
    }

    // Before the room is entered the first transport of a RELAY_AUTO context
    // has primary_timeout_ms to get there; failing that, the fallback.
    if (st->fb && st->phase != RC_READY && now - st->stage_start >= st->primary_timeout_ms) {
        switch_transport(v, st, "timed out", now);
        if (st->phase == RC_FAILED) return;
    }

    int ts = st->ops->state(st->tp);
    if (ts == RELAY_TP_FAILED) {
        if (st->fb && st->phase != RC_READY) {
            switch_transport(v, st, "could not be opened", now);
            if (st->phase == RC_FAILED) return;
            ts = st->ops->state(st->tp);
        }
        if (ts == RELAY_TP_FAILED) {
            fail(v, st, st->phase == RC_READY ? VNET_ECLOSED : VNET_ECONNREFUSED);
            return;
        }
    }
    if (ts == RELAY_TP_CONNECTING) {
        if (now - st->t_start >= st->hs_timeout_ms) fail(v, st, VNET_ETIMEDOUT);
        return;
    }

    if (!st->hs_sent) {
        st->hs_sent = 1;
        st->hs_backoff = HANDSHAKE_RETRY_FIRST_MS;
        st->hs_next = now + st->hs_backoff;
        send_handshake(v, st);
    }

    uint8_t buf[RELAY_MAX_FRAME + 1];
    for (int i = 0; i < MAX_FRAMES_PER_POLL && st->phase != RC_FAILED; i++) {
        int n = st->ops->recv(st->tp, buf, sizeof buf);
        if (n == VNET_EAGAIN) break;
        if (n < 0) {
            vnet__logf(v, VNET_LOG_DEBUG, "transport recv error (%d)", n);
            break;
        }
        if ((size_t)n > RELAY_MAX_FRAME) {
            v->stats.rx_oversize++;
            continue;
        }
        handle_frame(v, st, buf, (size_t)n, now);
    }
    if (st->phase == RC_FAILED) return;

    if (st->phase == RC_HANDSHAKE) {
        if (now - st->t_start >= st->hs_timeout_ms) {
            fail(v, st, VNET_ETIMEDOUT);
        } else if (st->udp_style && now >= st->hs_next) {
            // The same request, the same nonce: idempotent at the relay.
            send_handshake(v, st);
            st->hs_backoff = st->hs_backoff * 2 > HANDSHAKE_RETRY_MAX_MS ? HANDSHAKE_RETRY_MAX_MS
                                                                          : st->hs_backoff * 2;
            st->hs_next = now + st->hs_backoff;
        }
    } else if (st->phase == RC_PROBE) {
        if (now >= st->probe_deadline) {
            vnet__logf(v, VNET_LOG_WARN, "a %d byte datagram does not fit over %s", (int)VNET_MAX_DATAGRAM,
                       st->ops->name);
            if (st->fb) switch_transport(v, st, "datagrams of full size do not fit", now);
            else fail(v, st, VNET_EMSGSIZE);
        } else if (now >= st->probe_next) {
            send_probe(v, st);
            st->probe_next = now + RELAY_PROBE_RETRY_MS;
        }
    } else {  // READY
        if (now - st->last_rx > 3u * st->keepalive_ms) {
            fail(v, st, VNET_ETIMEDOUT);
        } else if (now >= st->ka_next) {
            send_keepalive(v, st, now);
            st->ka_next = now + st->keepalive_ms;
        }
    }
}

// vnet_wait(): sleep on the transport until it has something, but not past the
// next moment this client has something to do by itself.
static int rc_wait(vnet *v, int timeout_ms) {
    rc_state *st = v->bs;
    if (!st->ops->wait) return VNET_ENOTSUP;
    for (int i = 0; i < st->nsocks; i++)
        if (st->socks[i].used && st->socks[i].count > 0) return 1;  // already receivable
    uint64_t now = vnet__now(v);
    uint64_t next = 0;  // 0: no deadline of ours
    switch (st->phase) {
        case RC_HANDSHAKE:
            next = st->t_start + st->hs_timeout_ms;
            if (st->udp_style && st->hs_sent && st->hs_next < next) next = st->hs_next;
            break;
        case RC_PROBE:
            next = st->probe_deadline < st->probe_next ? st->probe_deadline : st->probe_next;
            break;
        case RC_READY: {
            uint64_t silent = st->last_rx + 3u * st->keepalive_ms + 1;
            next = st->ka_next < silent ? st->ka_next : silent;
            break;
        }
        default:
            break;
    }
    if (next) {
        uint64_t left = next > now ? next - now : 0;
        if (left < (uint64_t)timeout_ms) timeout_ms = (int)left;
    }
    return st->ops->wait(st->tp, timeout_ms);
}

static int rc_status(vnet *v, vnet_status_info *out) {
    rc_state *st = v->bs;
    out->transport = st->ops->name;
    out->relay_rtt_ms = -1;  // unknown until a KEEPALIVE has been answered
    switch (st->phase) {
        case RC_READY:
            out->state = VNET_STATUS_READY;
            out->relay_rtt_ms = st->rtt_ms;
            out->max_members = st->max_members;
            memcpy(out->room, st->code, RELAY_CODE_LEN + 1);
            out->self.family = VNET_AF_IPV4;
            out->self.bytes[0] = 10;
            out->self.bytes[3] = st->octet;
            break;
        case RC_FAILED:
            out->state = VNET_STATUS_FAILED;
            out->error = st->error;
            break;
        default:
            out->state = VNET_STATUS_CONNECTING;
            break;
    }
    return out->state;
}

// "room-host:PORT" is the room creator, 10.0.0.1. Known before the room is
// entered, because the creator is always .1.
static int rc_resolve(vnet *v, const char *hp, vnet_addr *out) {
    (void)v;
    static const char pfx[] = "room-host:";
    if (strncmp(hp, pfx, sizeof pfx - 1) != 0) return 1;
    const char *port = hp + sizeof pfx - 1;
    if (*port == 0) return VNET_EINVAL;
    unsigned x = 0;
    for (const char *c = port; *c; c++) {
        if (*c < '0' || *c > '9') return VNET_EINVAL;
        x = x * 10 + (unsigned)(*c - '0');
        if (x > 65535) return VNET_EINVAL;
    }
    memset(out, 0, sizeof *out);
    out->family = VNET_AF_IPV4;
    out->bytes[0] = 10;
    out->bytes[3] = 1;
    out->port = (uint16_t)x;
    return 0;
}

static void rc_destroy(vnet *v) {
    rc_state *st = v->bs;
    if ((st->phase == RC_READY || st->phase == RC_PROBE) && st->tp) send_simple(v, st, RELAY_T_LEAVE);  // best effort
    if (st->tp) st->ops->destroy(st->tp);
    for (int i = 0; i < st->nsocks; i++) free(st->socks[i].q);
    free(st->socks);
    free(st->fb_arg);
    free(st);
}

static const vnet_backend rc_backend = {
    "relay", rc_open,  rc_close, rc_local_addr, rc_check_send, rc_send, rc_recv,
    rc_poll, rc_destroy, rc_status, rc_resolve, rc_wait,
};

int vnet__relay_init_fallback(vnet *v, const vnet_config *cfg, const relay_transport_ops *ops, void *tp,
                              unsigned primary_timeout_ms, relay_fallback_fn fb, const void *fb_arg,
                              size_t fb_arg_len) {
    int rc = VNET_OK;
    rc_state *st = calloc(1, sizeof *st);
    if (!st) {
        ops->destroy(tp);
        return VNET_ENOMEM;
    }
    st->ops = ops;
    st->tp = tp;
    st->udp_style = (ops->flags & RELAY_TPF_UDP_STYLE) != 0;
    st->phase = RC_HANDSHAKE;
    if (cfg->room && cfg->room[0]) {
        if (normalise_code(cfg->room, st->want_code) != 0) {
            vnet__logf(v, VNET_LOG_ERROR, "bad room code \"%s\"", cfg->room);
            rc = VNET_EINVAL;
            goto fail;
        }
        st->is_join = 1;
    }
    if (cfg->relay_timeout_ms < 0 ||
        (cfg->keepalive_ms != 0 && (cfg->keepalive_ms < KEEPALIVE_MIN_MS || cfg->keepalive_ms > KEEPALIVE_MAX_MS))) {
        vnet__logf(v, VNET_LOG_ERROR, "keepalive_ms must be %d..%d", KEEPALIVE_MIN_MS, KEEPALIVE_MAX_MS);
        rc = VNET_EINVAL;
        goto fail;
    }
    st->hs_timeout_ms = cfg->relay_timeout_ms ? (unsigned)cfg->relay_timeout_ms : DEFAULT_HANDSHAKE_TIMEOUT_MS;
    st->cfg_keepalive_ms = (unsigned)cfg->keepalive_ms;
    st->keepalive_ms = st->cfg_keepalive_ms;  // 0: take the relay's suggestion
    if (fb) {
        st->fb_arg = malloc(fb_arg_len ? fb_arg_len : 1);
        if (!st->fb_arg) {
            rc = VNET_ENOMEM;
            goto fail;
        }
        if (fb_arg_len) memcpy(st->fb_arg, fb_arg, fb_arg_len);
        st->fb = fb;
        st->primary_timeout_ms = primary_timeout_ms ? primary_timeout_ms : 3000u;
    }
    if (begin_handshake(st) != 0) {
        // No unpredictable nonce, no session: the nonce is a credential.
        vnet__logf(v, VNET_LOG_ERROR, "no secure random source for the handshake nonce");
        rc = VNET_EIO;
        goto fail;
    }
    st->nsocks = v->max_sockets;
    st->rx_cap = cfg->rx_queue_len > 0 ? cfg->rx_queue_len : DEFAULT_RX_QUEUE;
    st->next_eph = EPHEMERAL_FIRST;
    st->socks = calloc((size_t)st->nsocks, sizeof *st->socks);
    if (!st->socks) {
        rc = VNET_ENOMEM;
        goto fail;
    }
    v->be = &rc_backend;
    v->bs = st;
    // The clock may be injected (tests); take the start time only now.
    st->t_start = vnet__now(v);
    st->stage_start = st->t_start;
    st->last_rx = st->t_start;
    st->last_poll = st->t_start;
    st->rtt_ms = -1;
    // First poll sends the handshake once the transport is open. Do it now
    // too, so that a UDP relay is asked immediately.
    rc_poll(v);
    return VNET_OK;

fail:
    ops->destroy(tp);
    free(st->socks);
    free(st->fb_arg);
    free(st);
    return rc;
}

int vnet__relay_init(vnet *v, const vnet_config *cfg, const relay_transport_ops *ops, void *tp) {
    return vnet__relay_init_fallback(v, cfg, ops, tp, 0, NULL, NULL, 0);
}
