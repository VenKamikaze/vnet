// SPDX-License-Identifier: MIT
// Native UDP backend: non-blocking BSD sockets (POSIX) / Winsock.
#include "vnet_internal.h"

#if defined(__EMSCRIPTEN__)
// Browsers have no UDP sockets: use the relay backends there.
int vnet__udp_init(vnet *v, const vnet_config *cfg) {
    (void)v;
    (void)cfg;
    return VNET_ENOTSUP;
}
#else

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define SOCK_ERRNO() WSAGetLastError()
#define E_WOULDBLOCK WSAEWOULDBLOCK
#define E_AGAIN WSAEWOULDBLOCK
#define E_INTR WSAEINTR
#define E_ADDRINUSE WSAEADDRINUSE
#define E_CONNRESET WSAECONNRESET
#define E_CONNREFUSED WSAECONNREFUSED
#define E_NOBUFS WSAENOBUFS
#define E_AFNOSUPPORT WSAEAFNOSUPPORT
#define E_MSGSIZE WSAEMSGSIZE
#define CLOSE_SOCK closesocket
typedef int sa_len_t;
typedef char *sockopt_t;
typedef WSAPOLLFD pollfd_t;
#define POLL_IN POLLRDNORM
#define POLL_CALL(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef struct pollfd pollfd_t;
#define POLL_IN POLLIN
#define POLL_CALL(fds, n, ms) poll((fds), (nfds_t)(n), (ms))
typedef int sock_t;
#define BAD_SOCK (-1)
#define SOCK_ERRNO() errno
#define E_WOULDBLOCK EWOULDBLOCK
#define E_AGAIN EAGAIN
#define E_INTR EINTR
#define E_ADDRINUSE EADDRINUSE
#define E_CONNRESET ECONNRESET
#define E_CONNREFUSED ECONNREFUSED
#define E_NOBUFS ENOBUFS
#define E_AFNOSUPPORT EAFNOSUPPORT
#define E_MSGSIZE EMSGSIZE
#define CLOSE_SOCK close
typedef socklen_t sa_len_t;
typedef void *sockopt_t;
#endif

// Receive buffer: room for the largest max_dgram (the relay's inner socket
// carries VNET_MAX_DATAGRAM plus relay framing) plus one byte to detect oversize.
#define VNET_UDP_RECV_BUF (VNET_MAX_DATAGRAM + 64 + 1)

typedef struct udp_sock {
    int used;
    sock_t fd;
    int family;  // AF_INET or AF_INET6
    int v6only;  // AF_INET6 socket that refuses IPv4 (VNET_FAMILY_IPV6)
} udp_sock;

// Kernel receive buffer asked for at open (best effort). The default holds only
// ~270 small datagrams, which one frame hitch at a busy server's packet rate
// overflows.
#define VNET_UDP_RCVBUF (256 * 1024)

typedef struct udp_state {
    udp_sock *socks;
    pollfd_t *pfds;  // scratch for vnet_wait(), one slot per socket
    int n;
    int family_mode;  // VNET_FAMILY_*
#if defined(_WIN32)
    int wsa_started;
#endif
} udp_state;

static int set_nonblocking(sock_t fd) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

static udp_sock *get_sock(udp_state *st, int s) {
    if (s < 0 || s >= st->n || !st->socks[s].used) return NULL;
    return &st->socks[s];
}

// vnet_addr <-> sockaddr ------------------------------------------------

static int to_sockaddr(const udp_sock *sk, const vnet_addr *a, struct sockaddr_storage *ss,
                       sa_len_t *len) {
    memset(ss, 0, sizeof *ss);
    if (sk->family == AF_INET) {
        if (a->family != VNET_AF_IPV4) return VNET_EAFNOSUPPORT;
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(a->port);
        memcpy(&sin->sin_addr, a->bytes, 4);
        *len = sizeof *sin;
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a->port);
        if (a->family == VNET_AF_IPV6) {
            memcpy(&s6->sin6_addr, a->bytes, 16);
        } else if (sk->v6only) {  // IPv6-only socket cannot reach IPv4 peers
            return VNET_EAFNOSUPPORT;
        } else {  // IPv4 destination through a dual-stack socket: ::ffff:a.b.c.d
            uint8_t *b = (uint8_t *)&s6->sin6_addr;
            b[10] = b[11] = 0xff;
            memcpy(b + 12, a->bytes, 4);
        }
        *len = sizeof *s6;
    }
    return VNET_OK;
}

static void from_sockaddr(const struct sockaddr_storage *ss, vnet_addr *a) {
    memset(a, 0, sizeof *a);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
        a->family = VNET_AF_IPV4;
        memcpy(a->bytes, &sin->sin_addr, 4);
        a->port = ntohs(sin->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;
        a->family = VNET_AF_IPV6;
        memcpy(a->bytes, &s6->sin6_addr, 16);
        a->port = ntohs(s6->sin6_port);
        vnet__addr_canonicalise(a);
    }
}

// Backend ----------------------------------------------------------------

static sock_t make_socket(vnet *v, int family, int v6only, uint16_t port, int *rc) {
#if defined(SOCK_CLOEXEC)
    // Atomic close-on-exec: no window for a concurrent fork()/exec() to inherit the socket.
    sock_t fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
    sock_t fd = socket(family, SOCK_DGRAM, 0);
#endif
    if (fd == BAD_SOCK) {
        *rc = SOCK_ERRNO() == E_AFNOSUPPORT ? VNET_EAFNOSUPPORT : VNET_EIO;
        return BAD_SOCK;
    }
#if !defined(_WIN32) && defined(FD_CLOEXEC) && !defined(SOCK_CLOEXEC)
    {  // Fallback where socket() has no SOCK_CLOEXEC (e.g. macOS).
        int fl = fcntl(fd, F_GETFD, 0);
        if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
#endif
    if (set_nonblocking(fd) != 0) {
        vnet__logf(v, VNET_LOG_ERROR, "cannot make socket non-blocking (%d)", SOCK_ERRNO());
        *rc = VNET_EIO;
        goto fail;
    }
#if defined(_WIN32)
    {  // Don't let ICMP "port unreachable" show up as recv errors.
        DWORD bytes = 0;
        BOOL off = FALSE;
        WSAIoctl(fd, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
    }
#endif
    {
        int rcvbuf = VNET_UDP_RCVBUF;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (sockopt_t)&rcvbuf, sizeof rcvbuf) != 0)
            vnet__logf(v, VNET_LOG_DEBUG, "SO_RCVBUF %d refused (%d): keeping the default", rcvbuf, SOCK_ERRNO());
    }
    struct sockaddr_storage ss;
    sa_len_t len;
    memset(&ss, 0, sizeof ss);
    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        sin->sin_port = htons(port);
        len = sizeof *sin;
    } else {
        // VNET_FAMILY_ANY: dual-stack (accept IPv4 too). VNET_FAMILY_IPV6: v6 only.
        int only = v6only ? 1 : 0;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (sockopt_t)&only, sizeof only) != 0) {
            *rc = VNET_EAFNOSUPPORT;
            goto fail;
        }
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_addr = in6addr_any;
        s6->sin6_port = htons(port);
        len = sizeof *s6;
    }
    if (bind(fd, (struct sockaddr *)&ss, len) != 0) {
        int e = SOCK_ERRNO();
        *rc = e == E_ADDRINUSE ? VNET_EADDRINUSE : VNET_EIO;
        if (*rc == VNET_EIO) vnet__logf(v, VNET_LOG_ERROR, "bind failed (%d)", e);
        goto fail;
    }
    return fd;
fail:
    CLOSE_SOCK(fd);
    return BAD_SOCK;
}

static int udp_open(vnet *v, uint16_t port) {
    udp_state *st = v->bs;
    int slot = -1;
    for (int i = 0; i < st->n; i++)
        if (!st->socks[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return VNET_ENOSOCK;

    int rc = VNET_EIO;
    int family = AF_INET;
    sock_t fd = BAD_SOCK;
    if (st->family_mode != VNET_FAMILY_IPV4) {
        family = AF_INET6;
        fd = make_socket(v, AF_INET6, st->family_mode == VNET_FAMILY_IPV6, port, &rc);
        if (fd == BAD_SOCK && st->family_mode == VNET_FAMILY_ANY && rc == VNET_EAFNOSUPPORT) {
            vnet__logf(v, VNET_LOG_INFO, "IPv6 unavailable, falling back to IPv4");
            family = AF_INET;
            fd = make_socket(v, AF_INET, 0, port, &rc);
        }
    } else {
        fd = make_socket(v, AF_INET, 0, port, &rc);
    }
    if (fd == BAD_SOCK) return rc;
    st->socks[slot].used = 1;
    st->socks[slot].fd = fd;
    st->socks[slot].family = family;
    st->socks[slot].v6only = family == AF_INET6 && st->family_mode == VNET_FAMILY_IPV6;
    return slot;
}

static void udp_close(vnet *v, int s) {
    udp_state *st = v->bs;
    udp_sock *sk = get_sock(st, s);
    if (!sk) return;
    CLOSE_SOCK(sk->fd);
    memset(sk, 0, sizeof *sk);
}

static int udp_local_addr(vnet *v, int s, vnet_addr *out) {
    udp_sock *sk = get_sock(v->bs, s);
    if (!sk) return VNET_EBADF;
    struct sockaddr_storage ss;
    sa_len_t len = sizeof ss;
    if (getsockname(sk->fd, (struct sockaddr *)&ss, &len) != 0) return VNET_EIO;
    from_sockaddr(&ss, out);
    return VNET_OK;
}

static int udp_check_send(vnet *v, int s, const vnet_addr *to) {
    udp_sock *sk = get_sock(v->bs, s);
    if (!sk) return VNET_EBADF;
    struct sockaddr_storage ss;
    sa_len_t sl;
    return to_sockaddr(sk, to, &ss, &sl);
}

static int udp_send(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    udp_sock *sk = get_sock(v->bs, s);
    if (!sk) return VNET_EBADF;
    struct sockaddr_storage ss;
    sa_len_t sl;
    int rc = to_sockaddr(sk, to, &ss, &sl);
    if (rc != VNET_OK) return rc;
    static const char empty = 0;
    for (;;) {
        int n = (int)sendto(sk->fd, len ? (const char *)buf : &empty, (int)len, 0,
                            (struct sockaddr *)&ss, sl);
        if (n >= 0) return n;
        int e = SOCK_ERRNO();
        if (e == E_INTR) continue;
        if (e == E_WOULDBLOCK || e == E_AGAIN || e == E_NOBUFS) return VNET_EAGAIN;
        vnet__logf(v, VNET_LOG_DEBUG, "sendto failed (%d)", e);
        return VNET_EIO;
    }
}

static int udp_recv(vnet *v, int s, void *buf, size_t cap, vnet_addr *from) {
    udp_sock *sk = get_sock(v->bs, s);
    if (!sk) return VNET_EBADF;
    // One byte more than the maximum so that oversize datagrams are detected,
    // not silently truncated into something plausible.
    uint8_t tmp[VNET_UDP_RECV_BUF];
    for (;;) {
        struct sockaddr_storage ss;
        sa_len_t sl = sizeof ss;
        // One byte more than max_dgram so that oversize datagrams are detected.
        int n = (int)recvfrom(sk->fd, (char *)tmp, (int)v->max_dgram + 1, 0, (struct sockaddr *)&ss, &sl);
        if (n < 0) {
            int e = SOCK_ERRNO();
            if (e == E_WOULDBLOCK || e == E_AGAIN) return VNET_EAGAIN;
            if (e == E_INTR || e == E_CONNRESET || e == E_CONNREFUSED) continue;
            if (e == E_MSGSIZE) {  // Winsock: datagram bigger than buffer
                v->stats.rx_oversize++;
                continue;
            }
            vnet__logf(v, VNET_LOG_DEBUG, "recvfrom failed (%d)", e);
            return VNET_EIO;
        }
        if ((size_t)n > v->max_dgram) {
            v->stats.rx_oversize++;
            continue;
        }
        if ((size_t)n > cap) return VNET_EMSGSIZE;  // consumed, never truncated
        if (n) memcpy(buf, tmp, (size_t)n);
        from_sockaddr(&ss, from);
        return n;
    }
}

static void udp_poll(vnet *v) { (void)v; }

static int udp_wait(vnet *v, int timeout_ms) {
    udp_state *st = v->bs;
    int n = 0;
    for (int i = 0; i < st->n; i++) {
        if (!st->socks[i].used) continue;
        st->pfds[n].fd = st->socks[i].fd;
        st->pfds[n].events = POLL_IN;
        st->pfds[n].revents = 0;
        n++;
    }
    if (n == 0) {  // nothing can arrive: just sleep (WSAPoll refuses an empty set)
#if defined(_WIN32)
        Sleep((DWORD)timeout_ms);
#else
        poll(NULL, 0, timeout_ms);
#endif
        return 0;
    }
    int rc = POLL_CALL(st->pfds, n, timeout_ms);
    if (rc > 0) return 1;
    if (rc == 0) return 0;
#if defined(_WIN32)
    int e = SOCK_ERRNO();
    return e == E_INTR ? 0 : VNET_EIO;
#else
    return errno == EINTR ? 0 : VNET_EIO;  // a signal: let the caller look around
#endif
}

static void udp_destroy(vnet *v) {
    udp_state *st = v->bs;
    for (int i = 0; i < st->n; i++)
        if (st->socks[i].used) CLOSE_SOCK(st->socks[i].fd);
#if defined(_WIN32)
    if (st->wsa_started) WSACleanup();
#endif
    free(st->pfds);
    free(st->socks);
    free(st);
}

static const vnet_backend udp_backend = {
    "udp", udp_open, udp_close, udp_local_addr, udp_check_send, udp_send, udp_recv, udp_poll, udp_destroy, NULL, NULL,
    udp_wait,
};

int vnet__udp_init(vnet *v, const vnet_config *cfg) {
    if (cfg->family != VNET_FAMILY_ANY && cfg->family != VNET_FAMILY_IPV4 &&
        cfg->family != VNET_FAMILY_IPV6)
        return VNET_EINVAL;
    udp_state *st = calloc(1, sizeof *st);
    if (!st) return VNET_ENOMEM;
    st->n = v->max_sockets;
    st->family_mode = cfg->family;
    st->socks = calloc((size_t)st->n, sizeof *st->socks);
    st->pfds = calloc((size_t)st->n, sizeof *st->pfds);
    if (!st->socks || !st->pfds) {
        free(st->socks);
        free(st->pfds);
        free(st);
        return VNET_ENOMEM;
    }
#if defined(_WIN32)
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            free(st->pfds);
            free(st->socks);
            free(st);
            return VNET_EIO;
        }
        st->wsa_started = 1;
    }
#endif
    v->be = &udp_backend;
    v->bs = st;
    return VNET_OK;
}

#endif /* !__EMSCRIPTEN__ */
