/* SPDX-License-Identifier: MIT */
/*
 * burst_probe: how many of the datagrams a program sends in one go arrive?
 * The test program for brief 15 (WebTransport dropped datagrams sent in a
 * burst), and a model of a game's send pattern.
 *
 * The SENDER creates the room (no --room). It waits until --peers receivers
 * have said hello, then sends, with the whole of one step in a single run of C
 * code (one JavaScript task in a browser build):
 *
 *   --mode burst   --rounds R times (--gap ms apart): --burst B datagrams to
 *                  each receiver, back to back.
 *   --mode game    --ticks T times, one every --interval ms (a game at 40 tics
 *                  a second is 25): per receiver one datagram, plus --extra E
 *                  more (acks, resends) spread over the receivers. With
 *                  --catchup C a step runs C tics back to back, as a game that
 *                  fell behind does, every --catchup-every E steps (default:
 *                  every step).
 *
 * Datagrams are --payload bytes long (at least 9) and numbered per receiver.
 * It then sends FINISHED to each receiver for a while and prints
 * "RESULT {json}" (also window.vnetResult in a browser) with what vnet_sendto()
 * accepted, what it refused (VNET_EAGAIN: dropped for back-pressure), and
 * vnet_stats.tx_dropped.
 *
 * A RECEIVER (--room CODE) counts the distinct datagrams that arrive and, after
 * FINISHED, prints "RESULT {"received":N,"distinct":D,...}". Native builds are
 * the receivers in the tests, the browser build is the sender.
 *
 * Options: --relay URL|HOST:PORT, --backend ws|wt|auto, --relay-wt URL,
 * --cert-hash HEX, --room CODE, --timeout MS, --verbose.
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

#define PORT 5000
#define MAX_PEERS 16
#define MAX_SEQ 65536
#define FIN_MS 1500
#define LINGER_MS 400

typedef struct peer_info {
    vnet_addr addr;
    uint32_t next_seq;
} peer_info;

typedef struct probe {
    vnet *v;
    int sock;
    int sender;
    int game;
    int peers, burst, rounds, gap_ms, ticks, interval_ms, extra, catchup, catchup_every, payload;
    uint64_t timeout_ms, t0;

    int entered, started;
    peer_info peer[MAX_PEERS];
    int npeers;

    /* sender */
    int round, tick;
    uint64_t next_step, fin_until, next_fin;
    uint64_t attempted, accepted, eagain, other_err;
    unsigned max_per_step;
    int fin_phase;

    /* receiver */
    unsigned char *seen;
    uint64_t received, distinct;
    int got_fin;
    uint32_t fin_expect;
    uint64_t linger_until, next_hello;

    int done, exit_code;
} probe;

static probe g;

static void log_line(void *user, int level, const char *msg) {
    (void)user;
    printf("LOG %d %s\n", level, msg);
    fflush(stdout);
}

static void wr32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void publish(const char *json) {
#if defined(__EMSCRIPTEN__)
    char script[700];
    snprintf(script, sizeof script, "window.vnetResult = %s;", json);
    emscripten_run_script(script);
#else
    (void)json;
#endif
}

static void finish(int ok, const char *error) {
    if (g.done) return;
    g.done = 1;
    g.exit_code = ok ? 0 : 1;
    vnet_stats st;
    vnet_get_stats(g.v, &st);
    char json[600];
    if (g.sender)
        snprintf(json, sizeof json,
                 "{\"ok\":%s,\"role\":\"sender\",\"peers\":%d,\"attempted\":%llu,\"accepted\":%llu,\"eagain\":%llu,"
                 "\"other_err\":%llu,\"tx_dropped\":%llu,\"max_per_step\":%u,\"error\":\"%s\"}",
                 ok ? "true" : "false", g.npeers, (unsigned long long)g.attempted, (unsigned long long)g.accepted,
                 (unsigned long long)g.eagain, (unsigned long long)g.other_err, (unsigned long long)st.tx_dropped,
                 g.max_per_step, error ? error : "");
    else
        snprintf(json, sizeof json,
                 "{\"ok\":%s,\"role\":\"receiver\",\"received\":%llu,\"distinct\":%llu,\"expect\":%u,\"error\":\"%s\"}",
                 ok ? "true" : "false", (unsigned long long)g.received, (unsigned long long)g.distinct, g.fin_expect,
                 error ? error : "");
    printf("RESULT %s\n", json);
    fflush(stdout);
    publish(json);
}

static void send_one(int peer) {
    uint8_t buf[VNET_MAX_DATAGRAM];
    memset(buf, 0xA5, (size_t)g.payload);
    buf[0] = 'D';
    wr32(buf + 1, g.peer[peer].next_seq++);
    int rc = vnet_sendto(g.v, g.sock, buf, (size_t)g.payload, &g.peer[peer].addr);
    g.attempted++;
    if (rc >= 0) g.accepted++;
    else if (rc == VNET_EAGAIN) g.eagain++;
    else g.other_err++;
}

static void on_message(const uint8_t *m, size_t n, const vnet_addr *from) {
    if (n == 0) return;
    if (g.sender) {
        if (m[0] != 'H') return;
        int i;
        for (i = 0; i < g.npeers; i++)
            if (memcmp(&g.peer[i].addr, from, sizeof *from) == 0) break;
        if (i == g.npeers && g.npeers < MAX_PEERS && g.npeers < g.peers) {
            g.peer[g.npeers].addr = *from;
            g.peer[g.npeers].next_seq = 0;
            g.npeers++;
        }
        uint8_t w = 'W';
        vnet_sendto(g.v, g.sock, &w, 1, from);
        return;
    }
    if (m[0] == 'W') g.started = 1;
    else if (m[0] == 'D' && n >= 5) {
        uint32_t seq = rd32(m + 1);
        g.received++;
        if (seq < MAX_SEQ && !g.seen[seq]) {
            g.seen[seq] = 1;
            g.distinct++;
        }
    } else if (m[0] == 'F' && n >= 5) {
        g.got_fin = 1;
        g.fin_expect = rd32(m + 1);
    }
}

static void sender_step(uint64_t now) {
    if (g.fin_phase) {
        if (now >= g.fin_until) {
            finish(1, NULL);
            return;
        }
        if (now >= g.next_fin) {
            g.next_fin = now + 50;
            for (int i = 0; i < g.npeers; i++) {
                uint8_t f[5];
                f[0] = 'F';
                wr32(f + 1, g.peer[i].next_seq);
                vnet_sendto(g.v, g.sock, f, sizeof f, &g.peer[i].addr);
            }
        }
        return;
    }
    if (g.npeers < g.peers) return;
    if (now < g.next_step) return;
    uint64_t before = g.attempted;
    if (!g.game) {
        for (int i = 0; i < g.npeers; i++)
            for (int k = 0; k < g.burst; k++) send_one(i);
        g.next_step = now + (uint64_t)g.gap_ms;
        if (++g.round >= g.rounds) {
            g.fin_phase = 1;
            g.fin_until = now + FIN_MS;
        }
    } else {
        int tics = g.tick % g.catchup_every == 0 ? g.catchup : 1;
        for (int c = 0; c < tics; c++) {
            for (int i = 0; i < g.npeers; i++) send_one(i);
            for (int e = 0; e < g.extra; e++) send_one(e % g.npeers);
        }
        if (++g.tick >= g.ticks) {
            g.fin_phase = 1;
            g.fin_until = now + FIN_MS;
        }
    }
    unsigned per = (unsigned)(g.attempted - before);
    if (per > g.max_per_step) g.max_per_step = per;
}

static void step(void) {
    if (g.done) return;
    vnet_poll(g.v);
    uint64_t now = vnet_time(g.v);
    if (now - g.t0 > g.timeout_ms) {
        finish(0, "timeout");
        return;
    }
    vnet_status_info st;
    int state = vnet_status(g.v, &st);
    if (state == VNET_STATUS_FAILED) {
        char e[120];
        snprintf(e, sizeof e, "relay: %s", vnet_strerror(st.error));
        finish(0, e);
        return;
    }
    if (state != VNET_STATUS_READY) return;
    if (!g.entered) {
        g.entered = 1;
        char self[40];
        vnet_addr_to_string(&st.self, self, sizeof self);
        printf("ROOM %s %s %s\n", st.room, self, st.transport);
        fflush(stdout);
        if (!g.sender) vnet_resolve(g.v, "room-host:5000", &g.peer[0].addr);
    }
    uint8_t buf[VNET_MAX_DATAGRAM];
    vnet_addr from;
    int n;
    while ((n = vnet_recvfrom(g.v, g.sock, buf, sizeof buf, &from)) >= 0) on_message(buf, (size_t)n, &from);

    if (g.sender) {
        sender_step(now);
        return;
    }
    if (!g.started && now >= g.next_hello) {
        uint8_t h = 'H';
        vnet_sendto(g.v, g.sock, &h, 1, &g.peer[0].addr);
        g.next_hello = now + 100;
    }
    if (g.got_fin) {
        if (g.linger_until == 0) g.linger_until = now + LINGER_MS; /* the data is in order ahead of it */
        if (now >= g.linger_until) finish(1, NULL);
    }
}

#if defined(__EMSCRIPTEN__)
static void loop(void) {
    step();
    if (g.done) emscripten_cancel_main_loop();
}
#endif

int main(int argc, char **argv) {
    const char *relay = NULL, *room = NULL, *backend = "auto", *relay_wt = NULL, *cert = NULL;
    int verbose = 0;
    g.peers = 1;
    g.burst = 20;
    g.rounds = 1;
    g.gap_ms = 200;
    g.ticks = 100;
    g.interval_ms = 25;
    g.catchup = 1;
    g.catchup_every = 1;
    g.payload = 40;
    g.timeout_ms = 60000;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--relay") && val) relay = argv[++i];
        else if (!strcmp(a, "--room") && val) room = argv[++i];
        else if (!strcmp(a, "--backend") && val) backend = argv[++i];
        else if (!strcmp(a, "--relay-wt") && val) relay_wt = argv[++i];
        else if (!strcmp(a, "--cert-hash") && val) cert = argv[++i];
        else if (!strcmp(a, "--mode") && val) g.game = !strcmp(argv[++i], "game");
        else if (!strcmp(a, "--peers") && val) g.peers = atoi(argv[++i]);
        else if (!strcmp(a, "--burst") && val) g.burst = atoi(argv[++i]);
        else if (!strcmp(a, "--rounds") && val) g.rounds = atoi(argv[++i]);
        else if (!strcmp(a, "--gap") && val) g.gap_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--ticks") && val) g.ticks = atoi(argv[++i]);
        else if (!strcmp(a, "--interval") && val) g.interval_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--extra") && val) g.extra = atoi(argv[++i]);
        else if (!strcmp(a, "--catchup") && val) g.catchup = atoi(argv[++i]);
        else if (!strcmp(a, "--catchup-every") && val) g.catchup_every = atoi(argv[++i]);
        else if (!strcmp(a, "--payload") && val) g.payload = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && val) g.timeout_ms = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else {
            fprintf(stderr, "usage: %s --relay URL|HOST:PORT [--room CODE] [--backend ws|wt|auto] [--mode burst|game] ...\n",
                    argv[0]);
            return 2;
        }
    }
    if (!relay || g.peers < 1 || g.peers > MAX_PEERS || g.payload < 9 || g.payload > VNET_MAX_DATAGRAM ||
        g.burst < 1 || g.rounds < 1 || g.ticks < 1 || g.catchup < 1 || g.catchup_every < 1 || g.extra < 0)
        return 2;
    g.sender = room == NULL;
    g.seen = calloc(MAX_SEQ, 1);
    if (!g.seen) return 2;

    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = !strcmp(backend, "ws") ? VNET_BACKEND_RELAY_WS : !strcmp(backend, "wt") ? VNET_BACKEND_RELAY_WT
                                                                                           : VNET_BACKEND_RELAY_AUTO;
    cfg.room = room;
    cfg.relay_wt = relay_wt;
    cfg.relay_cert_sha256 = cert;
    cfg.ignore_env = 1;
    cfg.rx_queue_len = 1024;
    if (verbose) cfg.log = log_line;
#if defined(__EMSCRIPTEN__)
    cfg.relay = relay;
#else
    cfg.backend = VNET_BACKEND_RELAY_AUTO; /* the same constant as in the page: the relay over UDP */
    cfg.relay_udp = relay;
#endif
    int err = 0;
    g.v = vnet_create(&cfg, &err);
    if (!g.v) {
        printf("RESULT {\"ok\":false,\"error\":\"%s\"}\n", vnet_strerror(err));
        return 1;
    }
    g.sock = vnet_open(g.v, PORT);
    if (g.sock < 0) return 1;
    g.t0 = vnet_time(g.v);

#if defined(__EMSCRIPTEN__)
    /* Game mode: a timer loop, one step every --interval ms (a task of its
     * own, like a game's frame). Otherwise the browser's frame rate. */
    emscripten_set_main_loop(loop, 0, 0);
    if (g.sender && g.game) emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, g.interval_ms);
    return 0;
#else
    while (!g.done) {
        step();
        if (g.done) break;
        int ms = g.sender && g.game && g.entered ? g.interval_ms : 2;
        if (vnet_wait(g.v, ms) == VNET_ENOTSUP) {
#if defined(_WIN32)
            Sleep((DWORD)ms);
#else
            struct timespec ts = {0, (long)ms * 1000000L};
            nanosleep(&ts, NULL);
#endif
        }
    }
    vnet_destroy(g.v);
    free(g.seen);
    return g.exit_code;
#endif
}
