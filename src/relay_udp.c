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

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

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

static const relay_transport_ops udp_ops = {"relay-udp", RELAY_TPF_UDP_STYLE, tp_state, tp_send, tp_recv, tp_destroy, NULL};

// Resolve "host:port" / "[v6]:port": numeric addresses never block; a host
// name is looked up once, here, with getaddrinfo().
static int resolve_relay(vnet *inner, const char *hp, int family, vnet_addr *out) {
    if (vnet_resolve(inner, hp, out) == 0) return VNET_OK;

    // Not numeric. Accept "name:port" only, with a plain decimal port, and a
    // name that is not a malformed number ("1.2.3", "1.2.3.4" with a bad port).
    size_t n = strlen(hp);
    const char *colon = n ? memchr(hp, ':', n) : NULL;
    if (!colon || colon == hp || hp[0] == '[' || memchr(colon + 1, ':', n - (size_t)(colon + 1 - hp)))
        return VNET_EINVAL;
    unsigned port = 0;
    size_t pn = (size_t)(hp + n - (colon + 1));
    if (pn == 0 || pn > 5) return VNET_EINVAL;
    for (const char *c = colon + 1; *c; c++) {
        if (*c < '0' || *c > '9') return VNET_EINVAL;
        port = port * 10 + (unsigned)(*c - '0');
    }
    if (port > 65535) return VNET_EINVAL;
    char host[256];
    size_t hn = (size_t)(colon - hp);
    if (hn >= sizeof host) return VNET_EINVAL;
    memcpy(host, hp, hn);
    host[hn] = 0;
    int has_letter = 0;
    for (size_t i = 0; i < hn; i++)
        if ((host[i] >= 'a' && host[i] <= 'z') || (host[i] >= 'A' && host[i] <= 'Z')) has_letter = 1;
    if (!has_letter) return VNET_EINVAL;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family == VNET_FAMILY_IPV4 ? AF_INET : family == VNET_FAMILY_IPV6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return VNET_ECONNREFUSED;
    int rc = VNET_ECONNREFUSED;
    // Prefer IPv4 (what a relay on a typical VPS listens on), else the first.
    struct addrinfo *pick = res;
    for (struct addrinfo *a = res; a; a = a->ai_next)
        if (a->ai_family == AF_INET) {
            pick = a;
            break;
        }
    memset(out, 0, sizeof *out);
    if (pick->ai_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)pick->ai_addr;
        out->family = VNET_AF_IPV4;
        memcpy(out->bytes, &sin->sin_addr, 4);
        out->port = (uint16_t)port;
        rc = VNET_OK;
    } else if (pick->ai_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)pick->ai_addr;
        out->family = VNET_AF_IPV6;
        memcpy(out->bytes, &s6->sin6_addr, 16);
        out->port = (uint16_t)port;
        rc = VNET_OK;
    }
    freeaddrinfo(res);
    return rc;
}

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
    int rc = resolve_relay(inner, cfg->relay, cfg->family, &t->relay);
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
