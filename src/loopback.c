// SPDX-License-Identifier: MIT
// In-process loopback backend. Deterministic: no threads, no real time, no
// OS sockets. Contexts on one vnet_loopback_network reach each other by
// virtual IPv4 address (127.1.x.y by default).
#include "vnet_internal.h"

#include <stdlib.h>
#include <string.h>

#define EPHEMERAL_FIRST 49152u
#define DEFAULT_RX_QUEUE 128

typedef struct lb_pkt {
    size_t len;
    vnet_addr from;
    uint8_t data[VNET_MAX_DATAGRAM];
} lb_pkt;

typedef struct lb_sock {
    int used;
    uint16_t port;
    int cap, head, count;
    int vis;  // how many packets from head are receivable (made so by poll)
    lb_pkt *q;
} lb_sock;

typedef struct lb_host {
    vnet *v;
    vnet_loopback_network *net;  // NULL once the network was destroyed
    int owns_net;
    uint8_t ip[4];
    lb_sock *socks;
    int nsocks;
    int rx_cap;
    uint16_t next_eph;
    struct lb_host *next;
} lb_host;

struct vnet_loopback_network {
    uint64_t now;
    lb_host *head, *tail;
};

vnet_loopback_network *vnet_loopback_network_create(void) {
    return calloc(1, sizeof(vnet_loopback_network));
}

void vnet_loopback_network_destroy(vnet_loopback_network *net) {
    if (!net) return;
    // Contexts should be destroyed first, but never leave them dangling.
    for (lb_host *h = net->head; h; h = h->next) h->net = NULL;
    free(net);
}

uint64_t vnet_loopback_network_time(const vnet_loopback_network *net) { return net ? net->now : 0; }

void vnet_loopback_network_advance(vnet_loopback_network *net, uint64_t ms) {
    if (net) net->now += ms;
}

void vnet_loopback_network_step(vnet_loopback_network *net, uint64_t ms) {
    if (!net) return;
    net->now += ms;
    // Phase 1: every sender releases what is due, staging it at the receivers.
    for (lb_host *h = net->head; h; h = h->next) vnet__release(h->v);
    // Phase 2: every receiver makes its staged datagrams receivable.
    for (lb_host *h = net->head; h; h = h->next) vnet_poll(h->v);
}

static uint64_t lb_clock(void *user) {
    lb_host *h = user;
    return h->net ? h->net->now : 0;
}

static lb_sock *get_sock(lb_host *h, int s) {
    if (s < 0 || s >= h->nsocks || !h->socks[s].used) return NULL;
    return &h->socks[s];
}

static int port_in_use(const lb_host *h, uint16_t port) {
    for (int i = 0; i < h->nsocks; i++)
        if (h->socks[i].used && h->socks[i].port == port) return 1;
    return 0;
}

static int lb_open(vnet *v, uint16_t port) {
    lb_host *h = v->bs;
    int slot = -1;
    for (int i = 0; i < h->nsocks; i++)
        if (!h->socks[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return VNET_ENOSOCK;
    if (port == 0) {
        unsigned span = 65536u - EPHEMERAL_FIRST;
        unsigned cand = h->next_eph;
        unsigned tries = 0;
        while (tries < span && port_in_use(h, (uint16_t)cand)) {
            cand = EPHEMERAL_FIRST + (cand - EPHEMERAL_FIRST + 1) % span;
            tries++;
        }
        if (tries == span) return VNET_EADDRINUSE;
        port = (uint16_t)cand;
        h->next_eph = (uint16_t)(EPHEMERAL_FIRST + (cand - EPHEMERAL_FIRST + 1) % span);
    } else if (port_in_use(h, port)) {
        return VNET_EADDRINUSE;
    }
    lb_sock *sk = &h->socks[slot];
    sk->q = calloc((size_t)h->rx_cap, sizeof *sk->q);
    if (!sk->q) return VNET_ENOMEM;
    sk->used = 1;
    sk->port = port;
    sk->cap = h->rx_cap;
    sk->head = sk->count = sk->vis = 0;
    return slot;
}

static void lb_close(vnet *v, int s) {
    lb_host *h = v->bs;
    lb_sock *sk = get_sock(h, s);
    if (!sk) return;
    free(sk->q);
    memset(sk, 0, sizeof *sk);
}

static int lb_local_addr(vnet *v, int s, vnet_addr *out) {
    lb_host *h = v->bs;
    lb_sock *sk = get_sock(h, s);
    if (!sk) return VNET_EBADF;
    vnet__addr_set_ipv4(out, h->ip, sk->port);
    return VNET_OK;
}

static int lb_check_send(vnet *v, int s, const vnet_addr *to) {
    lb_host *h = v->bs;
    if (!get_sock(h, s)) return VNET_EBADF;
    if (to->family != VNET_AF_IPV4) return VNET_EAFNOSUPPORT;
    return VNET_OK;
}

static lb_host *find_host(lb_host *from, const uint8_t ip[4]) {
    static const uint8_t lo[4] = {127, 0, 0, 1};
    if (memcmp(ip, lo, 4) == 0) return from;
    if (!from->net) return NULL;
    for (lb_host *h = from->net->head; h; h = h->next)
        if (memcmp(h->ip, ip, 4) == 0) return h;
    return NULL;
}

static int lb_send(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    lb_host *h = v->bs;
    lb_sock *src = get_sock(h, s);
    if (!src) return VNET_EBADF;
    if (to->family != VNET_AF_IPV4) return VNET_EAFNOSUPPORT;

    lb_host *dh = find_host(h, to->bytes);
    lb_sock *dst = NULL;
    if (dh)
        for (int i = 0; i < dh->nsocks; i++)
            if (dh->socks[i].used && dh->socks[i].port == to->port) {
                dst = &dh->socks[i];
                break;
            }
    if (!dst) {
        v->stats.unroutable++;
        return (int)len;  // like UDP: the sender cannot tell
    }
    if (dst->count == dst->cap) {
        dh->v->stats.rx_overflow++;
        return (int)len;
    }
    lb_pkt *p = &dst->q[(dst->head + dst->count) % dst->cap];
    dst->count++;
    p->len = len;
    if (len) memcpy(p->data, buf, len);
    vnet__addr_set_ipv4(&p->from, h->ip, src->port);
    return (int)len;
}

static int lb_recv(vnet *v, int s, void *buf, size_t cap, vnet_addr *from) {
    lb_host *h = v->bs;
    lb_sock *sk = get_sock(h, s);
    if (!sk) return VNET_EBADF;
    if (sk->vis == 0) return VNET_EAGAIN;
    lb_pkt *p = &sk->q[sk->head];
    sk->head = (sk->head + 1) % sk->cap;
    sk->count--;
    sk->vis--;
    if (p->len > cap) return VNET_EMSGSIZE;  // consumed, never truncated
    if (p->len) memcpy(buf, p->data, p->len);
    *from = p->from;
    return (int)p->len;
}

static void lb_poll(vnet *v) {
    lb_host *h = v->bs;
    for (int i = 0; i < h->nsocks; i++)
        if (h->socks[i].used) h->socks[i].vis = h->socks[i].count;
}

static void lb_destroy(vnet *v) {
    lb_host *h = v->bs;
    for (int i = 0; i < h->nsocks; i++) free(h->socks[i].q);
    free(h->socks);
    vnet_loopback_network *net = h->net;
    if (net) {
        lb_host **pp = &net->head;
        lb_host *prev = NULL;
        while (*pp && *pp != h) {
            prev = *pp;
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = h->next;
            if (net->tail == h) net->tail = prev;
        }
    }
    if (h->owns_net) free(net);
    free(h);
}

static const vnet_backend lb_backend = {
    "loopback", lb_open, lb_close, lb_local_addr, lb_check_send, lb_send, lb_recv, lb_poll, lb_destroy,
};

static int host_ip_taken(const vnet_loopback_network *net, const uint8_t ip[4]) {
    for (const lb_host *h = net->head; h; h = h->next)
        if (memcmp(h->ip, ip, 4) == 0) return 1;
    return 0;
}

int vnet__loopback_init(vnet *v, const vnet_config *cfg) {
    lb_host *h = calloc(1, sizeof *h);
    if (!h) return VNET_ENOMEM;
    int rc = VNET_OK;
    h->v = v;
    h->next_eph = EPHEMERAL_FIRST;
    h->rx_cap = cfg->rx_queue_len > 0 ? cfg->rx_queue_len : DEFAULT_RX_QUEUE;
    h->nsocks = v->max_sockets;
    h->socks = calloc((size_t)h->nsocks, sizeof *h->socks);
    if (!h->socks) {
        rc = VNET_ENOMEM;
        goto fail;
    }
    h->net = cfg->loopback_network;
    if (!h->net) {
        h->net = vnet_loopback_network_create();
        h->owns_net = 1;
        if (!h->net) {
            rc = VNET_ENOMEM;
            goto fail;
        }
    }
    if (cfg->loopback_host) {
        if (vnet__parse_ipv4(cfg->loopback_host, strlen(cfg->loopback_host), h->ip) != 0 ||
            h->ip[0] == 0 || (h->ip[0] == 127 && h->ip[1] == 0 && h->ip[2] == 0 && h->ip[3] == 1)) {
            vnet__logf(v, VNET_LOG_ERROR, "bad loopback_host \"%s\"", cfg->loopback_host);
            rc = VNET_EINVAL;
            goto fail;
        }
        if (host_ip_taken(h->net, h->ip)) {
            rc = VNET_EADDRINUSE;
            goto fail;
        }
    } else {
        int found = 0;
        for (unsigned c = 1; c < 65536u && !found; c++) {
            h->ip[0] = 127;
            h->ip[1] = 1;
            h->ip[2] = (uint8_t)(c >> 8);
            h->ip[3] = (uint8_t)(c & 0xff);
            found = !host_ip_taken(h->net, h->ip);
        }
        if (!found) {
            rc = VNET_EADDRINUSE;
            goto fail;
        }
    }
    if (h->net->tail) h->net->tail->next = h;
    else h->net->head = h;
    h->net->tail = h;

    v->be = &lb_backend;
    v->bs = h;
    if (!v->clock) {
        v->clock = lb_clock;
        v->clock_user = h;
    }
    return VNET_OK;

fail:
    if (h->owns_net) free(h->net);
    free(h->socks);
    free(h);
    return rc;
}
