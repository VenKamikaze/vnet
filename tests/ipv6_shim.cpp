// SPDX-License-Identifier: MIT
// Test seam (GNU ld --wrap, Linux). g_fail_sendto makes sendto() fail with
// ENETUNREACH. While g_fake_ipv6 is set, socket(AF_INET6)
// hands back a plain IPv4 UDP socket and IPV6_V6ONLY is recorded instead of
// applied. This lets the IPv6-only vs dual-stack *policy* of the UDP backend
// be tested on hosts with no IPv6 at all. Real IPv6 traffic is tested by
// UdpFamily.Ipv6LoopbackRoundTrip where ::1 exists (all CI jobs).
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern "C" {
int __real_socket(int, int, int);
int __real_setsockopt(int, int, int, const void *, socklen_t);
int __real_bind(int, const struct sockaddr *, socklen_t);
int __real_close(int);
ssize_t __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);

int g_fake_ipv6 = 0;
int g_fail_sendto = 0;
int g_fake_v6only_last = -1;
static bool g_fake_fd[4096];

int __wrap_socket(int domain, int type, int proto) {
    if (g_fake_ipv6 && domain == AF_INET6) {
        int fd = __real_socket(AF_INET, type, proto);
        if (fd >= 0 && fd < 4096) g_fake_fd[fd] = true;
        return fd;
    }
    return __real_socket(domain, type, proto);
}

int __wrap_setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
    if (fd >= 0 && fd < 4096 && g_fake_fd[fd] && level == IPPROTO_IPV6 && name == IPV6_V6ONLY) {
        std::memcpy(&g_fake_v6only_last, val, sizeof(int));
        return 0;
    }
    return __real_setsockopt(fd, level, name, val, len);
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (fd >= 0 && fd < 4096 && g_fake_fd[fd] && addr->sa_family == AF_INET6) {
        const sockaddr_in6 *s6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = s6->sin6_port;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        return __real_bind(fd, reinterpret_cast<sockaddr *>(&sin), sizeof sin);
    }
    return __real_bind(fd, addr, len);
}

ssize_t __wrap_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to,
                      socklen_t tolen) {
    if (g_fail_sendto) {
        errno = ENETUNREACH;
        return -1;
    }
    return __real_sendto(fd, buf, len, flags, to, tolen);
}

int __wrap_close(int fd) {
    if (fd >= 0 && fd < 4096) g_fake_fd[fd] = false;
    return __real_close(fd);
}
}
