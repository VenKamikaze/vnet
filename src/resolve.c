// SPDX-License-Identifier: MIT
// Host name resolution: vnet_resolve_host(), and the lookup the relay-UDP
// backend does for its relay address. The only place vnet calls getaddrinfo().
#include "vnet_internal.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#if defined(__EMSCRIPTEN__)
// Browsers have no resolver of their own to call.
int vnet__resolve_host(vnet *inner, const char *host_port, int family, vnet_addr *out) {
    (void)inner;
    (void)host_port;
    (void)family;
    (void)out;
    return VNET_ENOTSUP;
}
#else

#if !defined(_WIN32)
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

// Resolve "host:port" / "[v6]:port": numeric addresses never block; a host
// name is looked up once, here, with getaddrinfo().
int vnet__resolve_host(vnet *inner, const char *hp, int family, vnet_addr *out) {
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

#endif /* !__EMSCRIPTEN__ */

int vnet_resolve_host(const char *host_port, int family, vnet_addr *out) {
    if (!host_port || !out) return VNET_EINVAL;
    if (family != VNET_FAMILY_ANY && family != VNET_FAMILY_IPV4 && family != VNET_FAMILY_IPV6) return VNET_EINVAL;
    vnet_addr a;
    memset(&a, 0, sizeof a);
#if defined(_WIN32)
    // getaddrinfo() needs Winsock up; there is no context here to have done it.
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return VNET_EIO;
#endif
    int rc = vnet__resolve_host(NULL, host_port, family, &a);
#if defined(_WIN32)
    WSACleanup();
#endif
    if (rc == VNET_ECONNREFUSED) return VNET_EIO;  // the lookup itself failed
    if (rc != VNET_OK) return rc;
    if ((family == VNET_FAMILY_IPV4 && a.family != VNET_AF_IPV4) ||
        (family == VNET_FAMILY_IPV6 && a.family != VNET_AF_IPV6))
        return VNET_EAFNOSUPPORT;
    *out = a;
    return VNET_OK;
}
