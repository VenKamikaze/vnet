// SPDX-License-Identifier: MIT
#include "vnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

const char *vnet_strerror(int err) {
    switch (err) {
        case VNET_OK: return "success";
        case VNET_EAGAIN: return "no data available (would block)";
        case VNET_EINVAL: return "invalid argument";
        case VNET_EMSGSIZE: return "datagram too large";
        case VNET_EBADF: return "bad socket handle";
        case VNET_EADDRINUSE: return "address already in use";
        case VNET_ENOMEM: return "out of memory";
        case VNET_ENOSOCK: return "no free socket slot";
        case VNET_EAFNOSUPPORT: return "address family not supported";
        case VNET_EIO: return "I/O error";
        case VNET_ENOTSUP: return "backend not supported in this build";
        case VNET_ENOBUFS: return "output buffer too small";
        case VNET_ETIMEDOUT: return "timed out";
        case VNET_ENOTCONN: return "not connected to a room";
        case VNET_ENOROOM: return "no such room";
        case VNET_EROOMFULL: return "room is full";
        case VNET_ELIMIT: return "refused by the relay (limit reached)";
        case VNET_EPROTO: return "relay protocol error";
        case VNET_ECLOSED: return "session closed by the relay";
        case VNET_ECONNREFUSED: return "relay unreachable";
        default: return "unknown error";
    }
}

void vnet__logf(vnet *v, int level, const char *fmt, ...) {
    if (!v || !v->log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    v->log(v->log_user, level, line);
}

uint64_t vnet__system_clock(void *user) {
    (void)user;
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    // Split to avoid overflowing QuadPart * 1000 on long uptimes.
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000u +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000u / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

uint64_t vnet__now(vnet *v) { return v->clock(v->clock_user); }

uint64_t vnet_time(vnet *v) { return v ? vnet__now(v) : 0; }

void vnet_config_init(vnet_config *cfg) {
    if (cfg) memset(cfg, 0, sizeof *cfg);
}

static vnet *create(const vnet_config *cfg_in, int *err, const relay_transport_ops *ops, void *tp) {
    vnet_config cfg;
    int rc;
    int owns_tp = ops != NULL;  // the caller's transport is ours to destroy until the relay backend takes it
    if (cfg_in) cfg = *cfg_in;
    else memset(&cfg, 0, sizeof cfg);
    if (err) *err = VNET_OK;

    vnet *v = calloc(1, sizeof *v);
    if (!v) {
        rc = VNET_ENOMEM;
        goto fail_noctx;
    }
    v->log = cfg.log;
    v->log_user = cfg.log_user;
    v->clock = cfg.clock;
    v->clock_user = cfg.clock_user;
    v->max_sockets = cfg.max_sockets > 0 ? cfg.max_sockets : 16;
    v->max_dgram = VNET_MAX_DATAGRAM;

    // Impairment: environment replaces the configured value unless ignored.
    vnet_impair imp = cfg.impair;
    if (!cfg.ignore_env) {
        const char *env = getenv("VNET_IMPAIR");
        if (env && *env) {
            // Everything but the seed is replaced; the seed survives unless given.
            vnet_impair parsed;
            memset(&parsed, 0, sizeof parsed);
            parsed.seed = cfg.impair.seed;
            if (vnet__impair_parse_into(env, &parsed) != VNET_OK) {
                vnet__logf(v, VNET_LOG_ERROR, "VNET_IMPAIR is malformed: \"%s\"", env);
                rc = VNET_EINVAL;
                goto fail;
            }
            imp = parsed;
            vnet__logf(v, VNET_LOG_INFO, "impairment from VNET_IMPAIR=\"%s\"", env);
        }
    }
    rc = vnet__imp_validate(&imp);
    if (rc != VNET_OK) {
        vnet__logf(v, VNET_LOG_ERROR, "invalid impairment settings");
        goto fail;
    }

    // Relay backends time their handshake from creation: they need the clock
    // during init. (Loopback installs its virtual clock in its own init.)
    if (!v->clock && (ops || cfg.backend == VNET_BACKEND_RELAY_UDP || cfg.backend == VNET_BACKEND_RELAY_WS))
        v->clock = vnet__system_clock;
    if (ops) {  // test seam: a relay backend over the caller's transport
        owns_tp = 0;  // vnet__relay_init destroys it itself on failure
        rc = vnet__relay_init(v, &cfg, ops, tp);
    } else {
        switch (cfg.backend) {
            case VNET_BACKEND_UDP: rc = vnet__udp_init(v, &cfg); break;
            case VNET_BACKEND_LOOPBACK: rc = vnet__loopback_init(v, &cfg); break;
            case VNET_BACKEND_RELAY_UDP: rc = vnet__relay_udp_init(v, &cfg); break;
            case VNET_BACKEND_RELAY_WS: rc = vnet__relay_ws_init(v, &cfg); break;
            default: rc = VNET_ENOTSUP; break;
        }
    }
    if (rc != VNET_OK) goto fail;

    if (!v->clock) v->clock = vnet__system_clock;

    if (vnet__imp_enabled(&imp)) {
        v->imp = vnet__imp_create(v, &imp, cfg.impair_queue_len);
        if (!v->imp) {
            rc = VNET_ENOMEM;
            v->be->destroy(v);
            goto fail;
        }
    }
    return v;

fail:
    free(v);
fail_noctx:
    if (owns_tp) ops->destroy(tp);
    if (err) *err = rc;
    return NULL;
}

vnet *vnet_create(const vnet_config *cfg, int *err) { return create(cfg, err, NULL, NULL); }

vnet *vnet__create_with_relay_transport(const vnet_config *cfg, const relay_transport_ops *ops,
                                        void *tp, int *err) {
    return create(cfg, err, ops, tp);
}

void vnet_destroy(vnet *v) {
    if (!v) return;
    vnet__imp_destroy(v->imp);
    v->be->destroy(v);
    free(v);
}

int vnet_open(vnet *v, uint16_t port) {
    if (!v) return VNET_EINVAL;
    return v->be->open(v, port);
}

void vnet_close(vnet *v, int s) {
    if (!v) return;
    vnet__imp_purge_socket(v, s);
    v->be->close(v, s);
}

int vnet_local_addr(vnet *v, int s, vnet_addr *out) {
    if (!v || !out) return VNET_EINVAL;
    return v->be->local_addr(v, s, out);
}

int vnet__wire_send(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    int rc = v->be->send(v, s, buf, len, to);
    if (rc >= 0) v->stats.wire_datagrams++;  // failed sends are not counted
    return rc;
}

int vnet_sendto(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    if (!v) return VNET_EINVAL;
    if (len > VNET_MAX_DATAGRAM) return VNET_EMSGSIZE;
    if (!buf && len > 0) return VNET_EINVAL;
    if (!to || to->port == 0 || (to->family != VNET_AF_IPV4 && to->family != VNET_AF_IPV6))
        return VNET_EINVAL;

    if (!v->imp) {
        int rc = vnet__wire_send(v, s, buf, len, to);
        if (rc < 0) return rc;
        v->stats.tx_datagrams++;
        v->stats.tx_bytes += len;
        return (int)len;
    }
    // Impaired: the datagram is accepted if the socket and family are usable,
    // exactly as the unimpaired path would have reported.
    int rc = v->be->check_send(v, s, to);
    if (rc < 0) return rc;
    v->stats.tx_datagrams++;
    v->stats.tx_bytes += len;
    vnet__imp_submit(v, s, buf, len, to);
    return (int)len;
}

int vnet_recvfrom(vnet *v, int s, void *buf, size_t cap, vnet_addr *from) {
    if (!v) return VNET_EINVAL;
    if (!buf && cap > 0) return VNET_EINVAL;
    vnet_addr tmp;
    int rc = v->be->recv(v, s, buf, cap, from ? from : &tmp);
    if (rc >= 0) {
        v->stats.rx_datagrams++;
        v->stats.rx_bytes += (uint64_t)rc;
    }
    return rc;
}

void vnet__release(vnet *v) { vnet__imp_release(v); }

void vnet_poll(vnet *v) {
    if (!v) return;
    vnet__imp_release(v);
    v->be->poll(v);
}

int vnet_status(vnet *v, vnet_status_info *out) {
    if (!v) return VNET_EINVAL;
    vnet_status_info tmp;
    if (!out) out = &tmp;
    memset(out, 0, sizeof *out);
    out->state = VNET_STATUS_READY;
    out->transport = v->be->name;
    if (v->be->status) return v->be->status(v, out);
    return out->state;
}

void vnet_get_stats(const vnet *v, vnet_stats *out) {
    if (!out) return;
    if (v) *out = v->stats;
    else memset(out, 0, sizeof *out);
}
