// SPDX-License-Identifier: MIT
// Relay transport over UDP: relay frames are datagrams to the relay's UDP
// port. Built on the native UDP backend (an inner, private vnet context), so
// IPv4/IPv6 and Winsock handling are shared with it.
#include "relay_proto.h"
#include "vnet_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
// No UDP in browsers: use VNET_BACKEND_RELAY_WS.
int vnet__relay_udp_init(vnet *v, const vnet_config *cfg) {
    (void)v;
    (void)cfg;
    return VNET_ENOTSUP;
}
#else

typedef struct udp_tp {
    vnet *inner;
    int sock;
    vnet_addr relay;
} udp_tp;

static int tp_state(void *p) {
    (void)p;
    return RELAY_TP_OPEN;  // UDP has no connection to wait for
}

static int tp_send(void *p, const uint8_t *frame, size_t len) {
    udp_tp *t = p;
    // Straight to the inner backend: its frames are larger than
    // VNET_MAX_DATAGRAM, so vnet_sendto()'s size check does not apply.
    return t->inner->be->send(t->inner, t->sock, frame, len, &t->relay);
}

static int tp_recv(void *p, uint8_t *buf, size_t cap) {
    udp_tp *t = p;
    for (;;) {
        vnet_addr from;
        int n = t->inner->be->recv(t->inner, t->sock, buf, cap, &from);
        if (n < 0) return n;
        // Frames from the relay carry no token, so the source address is the
        // only check: anything not from the relay is ignored.
        if (vnet_addr_equal(&from, &t->relay)) return n;
    }
}

static void tp_destroy(void *p) {
    udp_tp *t = p;
    if (!t) return;
    vnet_destroy(t->inner);
    free(t);
}

static int tp_wait(void *p, int timeout_ms) {
    udp_tp *t = p;
    return t->inner->be->wait(t->inner, timeout_ms);
}

static const relay_transport_ops udp_ops = {"relay-udp", RELAY_TPF_UDP_STYLE, tp_state, tp_send, tp_recv, tp_destroy, NULL, tp_wait};

int vnet__relay_udp_init(vnet *v, const vnet_config *cfg) {
    if (!cfg->relay || !cfg->relay[0]) return VNET_EINVAL;

    // The inner context: one plain UDP socket, no impairment (the outer
    // context impairs what the game sends), its own large-frame limit.
    vnet_config icfg;
    vnet_config_init(&icfg);
    icfg.backend = VNET_BACKEND_UDP;
    icfg.family = cfg->family;
    icfg.max_sockets = 1;
    icfg.ignore_env = 1;
    icfg.log = cfg->log;
    icfg.log_user = cfg->log_user;
    int err = VNET_OK;
    vnet *inner = vnet_create(&icfg, &err);
    if (!inner) return err;
    inner->max_dgram = RELAY_MAX_FRAME;

    udp_tp *t = calloc(1, sizeof *t);
    if (!t) {
        vnet_destroy(inner);
        return VNET_ENOMEM;
    }
    t->inner = inner;
    int rc = vnet__resolve_host(inner, cfg->relay, cfg->family, &t->relay);
    if (rc != VNET_OK) {
        vnet__logf(v, VNET_LOG_ERROR, "cannot resolve relay \"%s\"", cfg->relay);
        tp_destroy(t);
        return rc == VNET_ECONNREFUSED ? VNET_ECONNREFUSED : VNET_EINVAL;
    }
    if (t->relay.port == 0) {
        tp_destroy(t);
        return VNET_EINVAL;
    }
    t->sock = vnet_open(inner, 0);
    if (t->sock < 0) {
        rc = t->sock;
        tp_destroy(t);
        return rc;
    }
    return vnet__relay_init(v, cfg, &udp_ops, t);
}

#endif /* !__EMSCRIPTEN__ */
