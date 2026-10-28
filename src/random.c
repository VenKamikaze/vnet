// SPDX-License-Identifier: MIT
// OS random bytes, for handshake nonces (uniqueness, not secrecy).
#if defined(_WIN32)
#define _CRT_RAND_S
#endif
#include "vnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t fallback_state;

// Last resort if the OS source is unavailable: mix the clock and addresses.
// Good enough to tell two clients apart; never used for secrets.
static void fallback_fill(uint8_t *p, size_t n) {
    if (fallback_state == 0) {
        fallback_state = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL ^ (uint64_t)clock() ^
                         (uint64_t)(uintptr_t)&fallback_state ^ ((uint64_t)(uintptr_t)p << 17);
    }
    for (size_t i = 0; i < n; i++) {
        fallback_state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = fallback_state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        p[i] = (uint8_t)((z ^ (z >> 31)) >> 24);
    }
}

void vnet__random(void *buf, size_t n) {
    uint8_t *p = buf;
#if defined(_WIN32)
    size_t i = 0;
    while (i < n) {
        unsigned int r;
        if (rand_s(&r) != 0) break;
        for (size_t k = 0; k < sizeof r && i < n; k++, i++) p[i] = (uint8_t)(r >> (8 * k));
    }
    if (i < n) fallback_fill(p + i, n - i);
#else
    // /dev/urandom also exists in Emscripten's file system (backed by
    // crypto.getRandomValues).
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f) {
        got = fread(p, 1, n, f);
        fclose(f);
    }
    if (got < n) fallback_fill(p + got, n - got);
#endif
}
