/* SPDX-License-Identifier: MIT */
/*
 * wake_probe: the test program for vnet_config.wake (the wake callback), and a
 * small demonstration of driving vnet from events instead of a frame loop.
 *
 *   Browser (Emscripten) build, the "host": creates a room and echoes every
 *   datagram it receives back to its sender. Query/--options:
 *     --mode freeze   polls from a main loop only until it is READY, then the
 *                     loop stops for good (a page whose loop is frozen, as a
 *                     hidden tab's is). The wake callback only counts. Nothing
 *                     receives the datagrams that arrive afterwards.
 *     --mode drive    has no main loop at all. Every wake schedules one poll
 *                     with emscripten_async_call(); the poll drains the sockets
 *                     and echoes. The whole exchange runs on wake-ups.
 *   Counters are published as window.vnetWake = {wakes, polls, received,
 *   echoed, ready} and a "ROOM <code> <addr> <transport>" line is printed.
 *
 *   Native build, the "guest": joins the room with --room and sends --count
 *   datagrams to room-host:5000 (10 ms apart), waiting with vnet_wait() instead
 *   of sleeping, then prints "RESULT {"sent":N,"echoed":M}" after --linger ms.
 *
 * Options common to both: --relay URL|HOST:PORT (native: the relay's UDP
 * address, given as relay_udp with VNET_BACKEND_RELAY_AUTO), --backend ws|wt|
 * auto, --relay-wt URL, --cert-hash HEX, --wt-timeout MS, --timeout MS.
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

typedef struct probe {
    vnet *v;
    int sock;
    int drive;     /* browser: wake-driven, no main loop */
    int ready;
    unsigned wakes, polls, received, echoed;
    int scheduled; /* a poll is already queued by a wake */
    int done;
} probe;

static probe g;

#if defined(__EMSCRIPTEN__)
static void publish(void) {
    char script[200];
    snprintf(script, sizeof script, "window.vnetWake={wakes:%u,polls:%u,received:%u,echoed:%u,ready:%d};", g.wakes,
             g.polls, g.received, g.echoed, g.ready);
    emscripten_run_script(script);
}

/* One poll: status, then drain the sockets and echo each datagram. */
static void service(void) {
    g.polls++;
    vnet_poll(g.v);
    vnet_status_info st;
    int state = vnet_status(g.v, &st);
    if (state == VNET_STATUS_READY && !g.ready) {
        g.ready = 1;
        char self[40];
        vnet_addr_to_string(&st.self, self, sizeof self);
        printf("ROOM %s %s %s\n", st.room, self, st.transport);
        fflush(stdout);
    }
    if (state == VNET_STATUS_READY) {
        uint8_t buf[VNET_MAX_DATAGRAM];
        vnet_addr from;
        int n;
        while ((n = vnet_recvfrom(g.v, g.sock, buf, sizeof buf, &from)) >= 0) {
            g.received++;
            if (vnet_sendto(g.v, g.sock, buf, (size_t)n, &from) >= 0) g.echoed++;
        }
    }
    publish();
}

static void poll_scheduled(void *arg) {
    (void)arg;
    g.scheduled = 0;
    service();
}

/* The wake callback: it may only arrange for a poll. (Counting is fine too.) */
static void on_wake(void *user) {
    (void)user;
    g.wakes++;
    publish(); /* test instrumentation only: it reads nothing from vnet */
    if (g.drive && !g.scheduled) {
        g.scheduled = 1;
        emscripten_async_call(poll_scheduled, NULL, 0);
    }
}

static void frozen_loop(void) {
    service();
    if (g.ready) emscripten_cancel_main_loop(); /* frozen from now on */
}
#else
static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = {0, (long)ms * 1000000L};
    nanosleep(&ts, NULL);
#endif
}
#endif

static void log_line(void *user, int level, const char *msg) {
    (void)user;
    printf("LOG %d %s\n", level, msg);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *relay = NULL, *room = NULL, *backend = "auto", *relay_wt = NULL, *cert = NULL, *mode = "drive";
    int verbose = 0, wt_timeout = 0, count = 20, timeout_ms = 30000, linger_ms = 500;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--relay") && val) relay = argv[++i];
        else if (!strcmp(a, "--room") && val) room = argv[++i];
        else if (!strcmp(a, "--backend") && val) backend = argv[++i];
        else if (!strcmp(a, "--relay-wt") && val) relay_wt = argv[++i];
        else if (!strcmp(a, "--cert-hash") && val) cert = argv[++i];
        else if (!strcmp(a, "--wt-timeout") && val) wt_timeout = atoi(argv[++i]);
        else if (!strcmp(a, "--mode") && val) mode = argv[++i];
        else if (!strcmp(a, "--count") && val) count = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && val) timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--linger") && val) linger_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else {
            fprintf(stderr, "usage: %s --relay URL|HOST:PORT [--room CODE] [--backend ws|wt|auto] [--mode freeze|drive] ...\n",
                    argv[0]);
            return 2;
        }
    }
    if (!relay) return 2;

    vnet_config cfg;
    vnet_config_init(&cfg);
    cfg.backend = !strcmp(backend, "ws") ? VNET_BACKEND_RELAY_WS : !strcmp(backend, "wt") ? VNET_BACKEND_RELAY_WT
                                                                                           : VNET_BACKEND_RELAY_AUTO;
    cfg.room = room;
    cfg.relay_wt = relay_wt;
    cfg.relay_cert_sha256 = cert;
    cfg.wt_timeout_ms = wt_timeout;
    cfg.ignore_env = 1;
    if (verbose) cfg.log = log_line;
#if defined(__EMSCRIPTEN__)
    cfg.relay = relay;
    cfg.wake = on_wake;
    g.drive = !strcmp(mode, "drive");
#else
    (void)mode;
    cfg.backend = VNET_BACKEND_RELAY_AUTO; /* the same constant as in the page */
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
    (void)count;
    (void)timeout_ms;
    (void)linger_ms;
    (void)wt_timeout;

#if defined(__EMSCRIPTEN__)
    publish();
    if (g.drive) {
        /* No main loop: the first wake (the transport opening) starts it all. */
        emscripten_exit_with_live_runtime();
    }
    emscripten_set_main_loop(frozen_loop, 50, 0); /* a timer loop: 50 Hz */
    return 0;
#else
    /* Native guest: send `count` pings once READY, wait with vnet_wait(). */
    vnet_addr host;
    vnet_resolve(g.v, "room-host:5000", &host);
    int sent = 0, echoed = 0, ok = 1;
    uint64_t t0 = vnet_time(g.v), next_send = 0, end = 0;
    while (vnet_time(g.v) - t0 < (uint64_t)timeout_ms) {
        vnet_poll(g.v);
        vnet_status_info st;
        int state = vnet_status(g.v, &st);
        if (state == VNET_STATUS_FAILED) {
            ok = 0;
            break;
        }
        uint64_t now = vnet_time(g.v);
        if (state == VNET_STATUS_READY) {
            if (sent == 0 && next_send == 0) {
                printf("ROOM %s\n", st.room);
                fflush(stdout);
            }
            if (sent < count && now >= next_send) {
                char msg[32];
                int n = snprintf(msg, sizeof msg, "ping %d", sent);
                if (vnet_sendto(g.v, g.sock, msg, (size_t)n, &host) >= 0) sent++;
                next_send = now + 10;
                if (sent == count) end = now + (uint64_t)linger_ms;
            }
            char buf[VNET_MAX_DATAGRAM];
            while (vnet_recvfrom(g.v, g.sock, buf, sizeof buf, NULL) >= 0) echoed++;
            if (sent == count && (echoed >= count || now >= end)) break;
        }
        int w = vnet_wait(g.v, 10);
        if (w == VNET_ENOTSUP) sleep_ms(1);
    }
    printf("RESULT {\"ok\":%s,\"sent\":%d,\"echoed\":%d}\n", ok ? "true" : "false", sent, echoed);
    vnet_destroy(g.v);
    return ok ? 0 : 1;
#endif
}
