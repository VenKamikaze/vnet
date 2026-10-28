/* SPDX-License-Identifier: MIT */
/* Compiled as C11 to prove vnet.h is valid C (not just C++), and that the
 * zero-initialised-config / designated-initialiser style works from C. */
#include "vnet.h"

#include <string.h>

_Static_assert(VNET_MAX_DATAGRAM == 1100, "VNET_MAX_DATAGRAM is part of the ABI");

int vnet_c_header_selftest(void) {
    int err = 0;
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet_config cfg = {.backend = VNET_BACKEND_LOOPBACK, .loopback_network = net, .ignore_env = 1};
    vnet *a = vnet_create(&cfg, &err);
    vnet *b = vnet_create(&cfg, &err);
    int rc = 1;
    if (!a || !b) goto out;

    int sa = vnet_open(a, 0), sb = vnet_open(b, 5000);
    vnet_addr to = {0};
    if (sa < 0 || sb < 0 || vnet_resolve(a, "127.1.0.2:5000", &to) != 0) goto out;
    if (vnet_sendto(a, sa, "hello", 5, &to) != 5) goto out;
    vnet_poll(b);
    char buf[16];
    vnet_addr from = {0};
    int n = vnet_recvfrom(b, sb, buf, sizeof buf, &from);
    if (n != 5 || memcmp(buf, "hello", 5) != 0) goto out;
    rc = 0;
out:
    vnet_destroy(a);
    vnet_destroy(b);
    vnet_loopback_network_destroy(net);
    return rc;
}
