// SPDX-License-Identifier: MIT
#include "vnet_internal.h"

#include <stdio.h>
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int vnet__parse_ipv4(const char *s, size_t n, uint8_t out[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        size_t st = i;
        unsigned v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (unsigned)(s[i] - '0');
            if (v > 255) return -1;
            i++;
        }
        size_t nd = i - st;
        if (nd == 0 || nd > 3) return -1;
        if (nd > 1 && s[st] == '0') return -1;  // no leading zeros (octal ambiguity)
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (i >= n || s[i] != '.') return -1;
            i++;
        }
    }
    return i == n ? 0 : -1;
}

int vnet__parse_ipv6(const char *s, size_t n, uint8_t out[16]) {
    uint16_t g[8];
    int ng = 0;
    int dc = -1;  // group index at which "::" occurred
    size_t i = 0;

    if (n == 0) return -1;
    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        dc = 0;
        i = 2;
    } else if (s[0] == ':') {
        return -1;
    }
    while (i < n) {
        size_t st = i;
        unsigned v = 0;
        int nd = 0;
        while (i < n && hexval(s[i]) >= 0) {
            v = v * 16 + (unsigned)hexval(s[i]);
            nd++;
            i++;
            if (nd > 4) return -1;
        }
        if (i < n && s[i] == '.') {  // embedded IPv4 tail, must be last
            uint8_t v4[4];
            if (ng > 6) return -1;
            if (vnet__parse_ipv4(s + st, n - st, v4) != 0) return -1;
            g[ng++] = (uint16_t)((v4[0] << 8) | v4[1]);
            g[ng++] = (uint16_t)((v4[2] << 8) | v4[3]);
            i = n;
            break;
        }
        if (nd == 0 || ng >= 8) return -1;
        g[ng++] = (uint16_t)v;
        if (i == n) break;
        if (s[i] != ':') return -1;
        i++;
        if (i < n && s[i] == ':') {
            if (dc >= 0) return -1;
            dc = ng;
            i++;
        } else if (i == n) {
            return -1;  // trailing single colon
        }
    }
    if (dc < 0) {
        if (ng != 8) return -1;
    } else if (ng > 7) {
        return -1;
    }
    uint16_t full[8] = {0};
    if (dc < 0) {
        memcpy(full, g, sizeof full);
    } else {
        int tail = ng - dc;
        for (int k = 0; k < dc; k++) full[k] = g[k];
        for (int k = 0; k < tail; k++) full[8 - tail + k] = g[dc + k];
    }
    for (int k = 0; k < 8; k++) {
        out[2 * k] = (uint8_t)(full[k] >> 8);
        out[2 * k + 1] = (uint8_t)(full[k] & 0xff);
    }
    return 0;
}

void vnet__addr_set_ipv4(vnet_addr *a, const uint8_t ip[4], uint16_t port) {
    memset(a, 0, sizeof *a);
    a->family = VNET_AF_IPV4;
    memcpy(a->bytes, ip, 4);
    a->port = port;
}

void vnet__addr_canonicalise(vnet_addr *a) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (a->family == VNET_AF_IPV6 && memcmp(a->bytes, mapped, 12) == 0) {
        uint8_t ip[4];
        memcpy(ip, a->bytes + 12, 4);
        vnet__addr_set_ipv4(a, ip, a->port);
    }
}

static int parse_port(const char *s, size_t n, uint16_t *out) {
    if (n == 0 || n > 5) return -1;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > 65535) return -1;
    *out = (uint16_t)v;
    return 0;
}

int vnet_resolve(vnet *v, const char *host_port, vnet_addr *out) {
    (void)v;  // built-in forms only; relay backends will extend this
    if (!host_port || !out) return VNET_EINVAL;
    vnet_addr a;
    memset(&a, 0, sizeof a);
    size_t n = strlen(host_port);
    const char *host;
    size_t hn;
    const char *port;
    size_t pn;

    if (n > 0 && host_port[0] == '[') {
        const char *close = memchr(host_port, ']', n);
        if (!close || close + 1 >= host_port + n || close[1] != ':') return VNET_EINVAL;
        host = host_port + 1;
        hn = (size_t)(close - host);
        port = close + 2;
        pn = (size_t)(host_port + n - port);
        if (vnet__parse_ipv6(host, hn, a.bytes) != 0) return VNET_EINVAL;
        a.family = VNET_AF_IPV6;
    } else {
        const char *colon = memchr(host_port, ':', n);
        if (!colon || memchr(colon + 1, ':', n - (size_t)(colon + 1 - host_port))) return VNET_EINVAL;
        host = host_port;
        hn = (size_t)(colon - host_port);
        port = colon + 1;
        pn = (size_t)(host_port + n - port);
        if (hn == 9 && memcmp(host, "localhost", 9) == 0) {
            static const uint8_t lo[4] = {127, 0, 0, 1};
            memcpy(a.bytes, lo, 4);
        } else if (vnet__parse_ipv4(host, hn, a.bytes) != 0) {
            return VNET_EINVAL;
        }
        a.family = VNET_AF_IPV4;
    }
    if (parse_port(port, pn, &a.port) != 0) return VNET_EINVAL;
    vnet__addr_canonicalise(&a);
    *out = a;
    return 0;
}

int vnet_addr_to_string(const vnet_addr *a, char *buf, size_t cap) {
    char tmp[64];
    int len;
    if (!a) return VNET_EINVAL;
    if (a->family == VNET_AF_IPV4) {
        len = snprintf(tmp, sizeof tmp, "%u.%u.%u.%u:%u", a->bytes[0], a->bytes[1], a->bytes[2],
                       a->bytes[3], a->port);
    } else if (a->family == VNET_AF_IPV6) {
        uint16_t g[8];
        for (int k = 0; k < 8; k++) g[k] = (uint16_t)((a->bytes[2 * k] << 8) | a->bytes[2 * k + 1]);
        int best = -1, bestlen = 0;
        for (int k = 0; k < 8;) {
            if (g[k] != 0) {
                k++;
                continue;
            }
            int e = k;
            while (e < 8 && g[e] == 0) e++;
            if (e - k > bestlen) {
                best = k;
                bestlen = e - k;
            }
            k = e;
        }
        if (bestlen < 2) best = -1;
        char *p = tmp;
        *p++ = '[';
        for (int k = 0; k < 8; k++) {
            if (k == best) {
                *p++ = ':';
                if (k == 0) *p++ = ':';
                k += bestlen - 1;
                continue;
            }
            p += snprintf(p, (size_t)(tmp + sizeof tmp - p), "%x", g[k]);
            if (k < 7) *p++ = ':';
        }
        p += snprintf(p, (size_t)(tmp + sizeof tmp - p), "]:%u", a->port);
        len = (int)(p - tmp);
    } else {
        return VNET_EINVAL;
    }
    if (len < 0) return VNET_EINVAL;
    if (!buf || (size_t)len >= cap) {
        if (buf && cap > 0) {
            memcpy(buf, tmp, cap - 1);
            buf[cap - 1] = '\0';
        }
        return VNET_ENOBUFS;
    }
    memcpy(buf, tmp, (size_t)len + 1);
    return len;
}

int vnet_addr_equal(const vnet_addr *a, const vnet_addr *b) {
    if (!a || !b) return 0;
    if (a->family != b->family || a->port != b->port) return 0;
    if (a->family == VNET_AF_IPV4) return memcmp(a->bytes, b->bytes, 4) == 0;
    if (a->family == VNET_AF_IPV6) return memcmp(a->bytes, b->bytes, 16) == 0;
    return a->family == VNET_AF_NONE;
}
