/* SPDX-License-Identifier: MIT */
/*
 * relay_peer: a small two-player program that talks through the vnet relay.
 * The same source, with the same backend constant (VNET_BACKEND_RELAY_AUTO),
 * is built natively (where it means the relay over UDP) and for the browser with
 * Emscripten (WebTransport or WebSocket), so a browser page and a native process
 * can share a room.
 *
 *   relay_peer --relay 127.0.0.1:7777                 create a room (host)
 *   relay_peer --relay 127.0.0.1:7777 --room K7M2QX   join it (guest)
 *   relay_peer --relay ws://localhost:8080/vnet ...   browser builds: a WebSocket URL
 *   relay_peer --relay ws://h/vnet --relay-udp h:7777 ...   one command line for both
 *
 * The relay address goes to vnet_config.relay (a URL: used by browser builds) and,
 * when it has no "scheme://" or --relay-udp is given, to vnet_config.relay_udp
 * (used by native builds), so no #ifdef is needed to choose between them.
 *
 * Options: --relay-udp HOST:PORT (the relay's UDP address, for native builds),
 * --count N (datagrams each way, default 100), --port P (default 5000),
 * --timeout MS (default 30000), --maxlen N (largest payload sent, default 899,
 * at most 1093), --verbose (print the library's log as "LOG ..." lines),
 *   --backend auto|ws|wt|udp   auto (default): the same constant everywhere.
 *                          In a browser, WebTransport then WebSocket (--relay is
 *                          the WebSocket URL; a plain ws:// URL means WebSocket
 *                          only); natively, the relay over UDP. ws and wt force
 *                          one browser transport (wt: --relay is an https:// URL);
 *                          udp forces the native one.
 * and for browser builds:
 *   --relay-wt URL         auto: the WebTransport URL (default: derived from a wss:// --relay)
 *   --cert-hash HEX        SHA-256 of the relay's -dev-cert certificate
 *   --wt-timeout MS        auto: how long WebTransport gets before the fallback (default 3000)
 *
 * After entering the room it prints "ROOM <code> <virtual address>". The guest
 * sends HELLO to "room-host:5000" until the host answers WELCOME; then each
 * side sends N numbered datagrams of varying length and content to the other
 * and checks that it receives the other's N, complete and IN ORDER. It prints
 * one line "RESULT {json}" and exits 0 on success. (Order is only guaranteed
 * because the relay path on localhost does not lose or reorder; this is a
 * test of the relay, not a protocol to copy.)
 */
#define _POSIX_C_SOURCE 200809L
#include "vnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define MSG_HELLO 'H'
#define MSG_WELCOME 'W'
#define MSG_DATA 'D'
#define MSG_FINISHED 'F'
#define SEND_PER_STEP 10
#define LINGER_MS 300

typedef struct peer {
    vnet *v;
    int sock;
    int is_host;
    int count;
    unsigned port;
    uint64_t timeout_ms;
    uint64_t t0;

    int entered;
    char room[8];
    vnet_addr other;   /* the other player's address (host: learned from HELLO) */
    int have_other;
    int started;       /* data phase */
    int sent, next_recv;
    int peer_finished;
    uint64_t next_hello, next_finished, linger_until;
    int done, exit_code;
    char error[160];
} peer;

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

/* Message i of a sender: length and bytes are a function of (i, sender role). */
static size_t g_maxlen = 899; /* largest payload: message = 7 header bytes + this, so at most VNET_MAX_DATAGRAM */
/* Every tenth message is as long as allowed, so that the largest datagram is always exercised. */
static size_t payload_len(int i) { return i % 10 == 9 ? g_maxlen : (size_t)((i * 37) % ((int)g_maxlen + 1)); }
static void log_line(void *user, int level, const char *msg) {
    (void)user;
    printf("LOG %d %s\n", level, msg);
    fflush(stdout);
}
static uint8_t payload_byte(int i, size_t j, int from_host) {
    return (uint8_t)((unsigned)i * 31u + (unsigned)j + (from_host ? 0x40u : 0x80u));
}

static void finish(peer *p, int ok, const char *error) {
    if (p->done) return;
    p->done = 1;
    p->exit_code = ok ? 0 : 1;
    char json[400];
    if (ok)
        snprintf(json, sizeof json,
                 "{\"ok\":true,\"role\":\"%s\",\"room\":\"%s\",\"sent\":%d,\"received\":%d}",
                 p->is_host ? "host" : "guest", p->room, p->sent, p->next_recv);
    else
        snprintf(json, sizeof json,
                 "{\"ok\":false,\"role\":\"%s\",\"room\":\"%s\",\"sent\":%d,\"received\":%d,\"error\":\"%s\"}",
                 p->is_host ? "host" : "guest", p->room, p->sent, p->next_recv, error);
    printf("RESULT %s\n", json);
    fflush(stdout);
#if defined(__EMSCRIPTEN__)
    char script[480];
    snprintf(script, sizeof script, "window.vnetResult = %s;", json);
    emscripten_run_script(script);
#endif
}

static void send_msg(peer *p, const void *buf, size_t len) {
    vnet_sendto(p->v, p->sock, buf, len, &p->other);  /* a drop is fine: all messages repeat or are checked */
}

static void send_data(peer *p, int i) {
    uint8_t buf[VNET_MAX_DATAGRAM];
    size_t n = payload_len(i);
    buf[0] = MSG_DATA;
    wr32(buf + 1, (uint32_t)i);
    buf[5] = (uint8_t)(n >> 8);
    buf[6] = (uint8_t)n;
    for (size_t j = 0; j < n; j++) buf[7 + j] = payload_byte(i, j, p->is_host);
    send_msg(p, buf, 7 + n);
}

static void on_message(peer *p, const uint8_t *m, size_t n, const vnet_addr *from, uint64_t now) {
    (void)now;
    if (n == 0) return;
    switch (m[0]) {
        case MSG_HELLO:
            if (!p->is_host) return;
            if (!p->have_other) {
                p->other = *from;
                p->have_other = 1;
            }
            {
                uint8_t w = MSG_WELCOME;
                send_msg(p, &w, 1);  /* repeated HELLOs get repeated WELCOMEs */
            }
            p->started = 1;
            break;
        case MSG_WELCOME:
            if (p->is_host) return;
            p->started = 1;
            break;
        case MSG_FINISHED:
            p->peer_finished = 1;
            break;
        case MSG_DATA: {
            if (!p->started) {
                /* The peer's data can overtake our WELCOME/HELLO handling. */
                p->started = 1;
            }
            if (n < 7) {
                snprintf(p->error, sizeof p->error, "short DATA message (%u bytes)", (unsigned)n);
                break;
            }
            uint32_t seq = rd32(m + 1);
            size_t len = ((size_t)m[5] << 8) | m[6];
            int from_host = !p->is_host;
            if ((int)seq != p->next_recv) {
                snprintf(p->error, sizeof p->error, "out of order or lost: got %u, expected %d", (unsigned)seq,
                         p->next_recv);
                break;
            }
            if (len != payload_len((int)seq) || n != 7 + len) {
                snprintf(p->error, sizeof p->error, "message %u has length %u (%u on the wire), expected %u",
                         (unsigned)seq, (unsigned)len, (unsigned)(n - 7), (unsigned)payload_len((int)seq));
                break;
            }
            for (size_t j = 0; j < len; j++)
                if (m[7 + j] != payload_byte((int)seq, j, from_host)) {
                    snprintf(p->error, sizeof p->error, "message %u corrupted at byte %u", (unsigned)seq, (unsigned)j);
                    break;
                }
            if (!p->error[0]) p->next_recv++;
            break;
        }
        default:
            break;
    }
}

/* One iteration of the program: call it once per frame. */
static void step(peer *p) {
    if (p->done) return;
    vnet_poll(p->v);
    uint64_t now = vnet_time(p->v);
    if (now - p->t0 > p->timeout_ms) {
        finish(p, 0, "timeout");
        return;
    }

    vnet_status_info st;
    int state = vnet_status(p->v, &st);
    if (state == VNET_STATUS_FAILED) {
        char e[120];
        snprintf(e, sizeof e, "relay: %s", vnet_strerror(st.error));
        finish(p, 0, e);
        return;
    }
    if (state != VNET_STATUS_READY) return;

    if (!p->entered) {
        p->entered = 1;
        snprintf(p->room, sizeof p->room, "%s", st.room);
        char self[40];
        vnet_addr_to_string(&st.self, self, sizeof self);
        printf("ROOM %s %s %s\n", st.room, self, st.transport);
        fflush(stdout);
        if (!p->is_host) {
            char hp[32];
            snprintf(hp, sizeof hp, "room-host:%u", p->port);
            vnet_resolve(p->v, hp, &p->other);  /* the creator is always 10.0.0.1 */
            p->have_other = 1;
        }
    }

    /* Receive everything that is waiting. */
    uint8_t buf[VNET_MAX_DATAGRAM];
    vnet_addr from;
    int n;
    while ((n = vnet_recvfrom(p->v, p->sock, buf, sizeof buf, &from)) >= 0) {
        on_message(p, buf, (size_t)n, &from, now);
        if (p->error[0]) {
            finish(p, 0, p->error);
            return;
        }
    }

    if (!p->is_host && !p->started && now >= p->next_hello) {
        uint8_t h = MSG_HELLO;
        send_msg(p, &h, 1);
        p->next_hello = now + 100;
    }
    if (!p->started) return;

    for (int k = 0; k < SEND_PER_STEP && p->sent < p->count; k++) send_data(p, p->sent++);

    if (p->sent == p->count && p->next_recv == p->count) {
        if (now >= p->next_finished) {
            uint8_t f = MSG_FINISHED;
            send_msg(p, &f, 1);
            p->next_finished = now + 50;
        }
        if (p->peer_finished) {
            if (p->linger_until == 0) p->linger_until = now + LINGER_MS;  /* keep answering for a moment */
            if (now >= p->linger_until) finish(p, 1, NULL);
        }
    }
}

#if defined(__EMSCRIPTEN__)
static void loop(void *arg) {
    peer *p = arg;
    step(p);
    if (p->done) emscripten_cancel_main_loop();
}
#else
static void sleep_ms(void) {
#if defined(_WIN32)
    Sleep(1);
#else
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
#endif
}
#endif

int main(int argc, char **argv) {
    static peer p; /* zero-initialised */
    const char *relay = NULL, *relay_udp = NULL, *room = NULL, *backend = NULL, *relay_wt = NULL, *cert_hash = NULL;
    int wt_timeout = 0, verbose = 0;
    p.count = 100;
    p.port = 5000;
    p.timeout_ms = 30000;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--relay") && val) relay = argv[++i];
        else if (!strcmp(a, "--relay-udp") && val) relay_udp = argv[++i];
        else if (!strcmp(a, "--room") && val) room = argv[++i];
        else if (!strcmp(a, "--count") && val) p.count = atoi(argv[++i]);
        else if (!strcmp(a, "--port") && val) p.port = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && val) p.timeout_ms = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(a, "--maxlen") && val) g_maxlen = (size_t)atoll(argv[++i]);
        else if (!strcmp(a, "--backend") && val) backend = argv[++i];
        else if (!strcmp(a, "--relay-wt") && val) relay_wt = argv[++i];
        else if (!strcmp(a, "--cert-hash") && val) cert_hash = argv[++i];
        else if (!strcmp(a, "--wt-timeout") && val) wt_timeout = atoi(argv[++i]);
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else {
            fprintf(stderr,
                    "usage: %s --relay HOST:PORT|ws://HOST:PORT/vnet [--relay-udp HOST:PORT] [--room CODE] [--count N] [--port P] [--timeout MS]\n"
                    "          [--maxlen N] [--verbose] [--backend ws|wt|auto] [--relay-wt URL] [--cert-hash HEX] [--wt-timeout MS]\n",
                    argv[0]);
            return 2;
        }
    }
    if (g_maxlen > VNET_MAX_DATAGRAM - 7) {
        fprintf(stderr, "--maxlen is at most %d\n", VNET_MAX_DATAGRAM - 7);
        return 2;
    }
    if (!relay || p.count < 1 || p.count > 100000 || p.port == 0 || p.port > 65535) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }
    if (room && !*room) room = NULL;
    p.is_host = room == NULL;

    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = VNET_BACKEND_RELAY_AUTO;  /* the same constant in every build */
    if (backend) {
        if (!strcmp(backend, "ws")) cfg.backend = VNET_BACKEND_RELAY_WS;
        else if (!strcmp(backend, "wt")) cfg.backend = VNET_BACKEND_RELAY_WT;
        else if (!strcmp(backend, "auto")) cfg.backend = VNET_BACKEND_RELAY_AUTO;
        else if (!strcmp(backend, "udp")) cfg.backend = VNET_BACKEND_RELAY_UDP;
        else {
            fprintf(stderr, "unknown --backend\n");
            return 2;
        }
    }
    cfg.relay = relay;
    cfg.relay_udp = relay_udp ? relay_udp : strstr(relay, "://") ? NULL : relay;
    cfg.relay_wt = relay_wt;
    cfg.relay_cert_sha256 = cert_hash;
    cfg.wt_timeout_ms = wt_timeout;
    if (verbose) cfg.log = log_line;
    cfg.room = room;
    cfg.rx_queue_len = 512;
    int err = 0;
    p.v = vnet_create(&cfg, &err);
    if (!p.v) {
        char e[120];
        snprintf(e, sizeof e, "vnet_create: %s", vnet_strerror(err));
        finish(&p, 0, e);
        return 1;
    }
    p.sock = vnet_open(p.v, (uint16_t)p.port);
    if (p.sock < 0) {
        finish(&p, 0, "vnet_open failed");
        return 1;
    }
    p.t0 = vnet_time(p.v);

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(loop, &p, 0, 1);
#else
    while (!p.done) {
        step(&p);
        /* Sleep until a frame arrives (or 2 ms: the program has timers of its own). */
        if (!p.done && vnet_wait(p.v, 2) == VNET_ENOTSUP) sleep_ms();
    }
#endif
    vnet_destroy(p.v);
    return p.exit_code;
}
