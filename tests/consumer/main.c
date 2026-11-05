/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "vnet.h"

int main(void) {
    vnet_config cfg = {.backend = VNET_BACKEND_LOOPBACK, .ignore_env = 1};
    vnet *v = vnet_create(&cfg, NULL);
    if (!v) return 1;
    int s1 = vnet_open(v, 1), s2 = vnet_open(v, 2);
    vnet_addr to;
    vnet_resolve(v, "127.0.0.1:2", &to);
    vnet_sendto(v, s1, "ok", 2, &to);
    vnet_poll(v);
    char buf[8];
    int n = vnet_recvfrom(v, s2, buf, sizeof buf, NULL);
    vnet_destroy(v);
    printf("consumer: %s\n", n == 2 && memcmp(buf, "ok", 2) == 0 ? "ok" : "FAILED");
    return n == 2 ? 0 : 1;
}
