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

enum { RC_HANDSHAKE, RC_READY, RC_FAILED };

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
    unsigned keepalive_ms;
    uint64_t ka_next;
    uint64_t last_rx;

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
            if (st->keepalive_ms == 0) {
                unsigned s = p[14 + RELAY_TOKEN_LEN + RELAY_CODE_LEN + 1];
                st->keepalive_ms = s ? s * 1000u : DEFAULT_KEEPALIVE_MS;
                if (st->keepalive_ms > KEEPALIVE_MAX_MS) st->keepalive_ms = KEEPALIVE_MAX_MS;  // never let a relay's suggestion outlast its own timeout
            }
            st->phase = RC_READY;
            st->ka_next = now + st->keepalive_ms;
            vnet__logf(v, VNET_LOG_INFO, "entered room %s as 10.0.0.%u", st->code, st->octet);
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
            if (n == 1 && st->phase == RC_READY) st->last_rx = now;
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

static void rc_poll(vnet *v) {
    rc_state *st = v->bs;
    if (st->phase == RC_FAILED) return;
    uint64_t now = vnet__now(v);

    int ts = st->ops->state(st->tp);
    if (ts == RELAY_TP_FAILED) {
        fail(v, st, st->phase == RC_READY ? VNET_ECLOSED : VNET_ECONNREFUSED);
        return;
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
    } else {  // READY
        if (now - st->last_rx > 3u * st->keepalive_ms) {
            fail(v, st, VNET_ETIMEDOUT);
        } else if (now >= st->ka_next) {
            send_simple(v, st, RELAY_T_KEEPALIVE);
            st->ka_next = now + st->keepalive_ms;
        }
    }
}

static int rc_status(vnet *v, vnet_status_info *out) {
    rc_state *st = v->bs;
    out->transport = st->ops->name;
    switch (st->phase) {
        case RC_READY:
            out->state = VNET_STATUS_READY;
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
    if (st->phase == RC_READY) send_simple(v, st, RELAY_T_LEAVE);  // best effort
    st->ops->destroy(st->tp);
    for (int i = 0; i < st->nsocks; i++) free(st->socks[i].q);
    free(st->socks);
    free(st);
}

static const vnet_backend rc_backend = {
    "relay", rc_open,  rc_close, rc_local_addr, rc_check_send, rc_send, rc_recv,
    rc_poll, rc_destroy, rc_status, rc_resolve,
};

int vnet__relay_init(vnet *v, const vnet_config *cfg, const relay_transport_ops *ops, void *tp) {
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
    st->keepalive_ms = (unsigned)cfg->keepalive_ms;  // 0: take the relay's suggestion
    if (vnet__random(st->nonce, sizeof st->nonce) != 0) {
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
    st->last_rx = st->t_start;
    // First poll sends the handshake once the transport is open. Do it now
    // too, so that a UDP relay is asked immediately.
    rc_poll(v);
    return VNET_OK;

fail:
    ops->destroy(tp);
    free(st->socks);
    free(st);
    return rc;
}
