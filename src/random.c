// SPDX-License-Identifier: MIT
// Cryptographically secure random bytes, for the relay handshake nonce.
//
// The nonce is a credential: the relay answers a repeat of it with the
// session's token, so it must be unpredictable. There is deliberately no
// weak fallback; if the OS cannot supply random bytes, the caller fails.
#if defined(_WIN32)
#define _CRT_RAND_S
#endif
#include "vnet_internal.h"

#include <stdio.h>
#include <stdlib.h>

#if !defined(_WIN32)
#include <errno.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#else
// glibc (2.25+) and musl (Emscripten) export it, but only declare it under
// feature-test macros this strict-C11 build does not set.
extern int getentropy(void *buffer, size_t length);
#endif
#endif

int (*vnet__random_hook)(void *buf, size_t n) = NULL;

int vnet__random(void *buf, size_t n) {
    if (vnet__random_hook) return vnet__random_hook(buf, n);
    if (n > 256) return -1;
    uint8_t *p = buf;
#if defined(_WIN32)
    // rand_s() is backed by the system CSPRNG (RtlGenRandom) and needs no
    // extra import library.
    size_t i = 0;
    while (i < n) {
        unsigned int r;
        if (rand_s(&r) != 0) return -1;
        for (size_t k = 0; k < sizeof r && i < n; k++, i++) p[i] = (uint8_t)(r >> (8 * k));
    }
    return 0;
#else
    // getentropy(): Linux (glibc 2.25+), macOS, and Emscripten, where it reads
    // crypto.getRandomValues() without needing a file system.
    if (getentropy(p, n) == 0) return 0;
    if (errno != ENOSYS) return -1;
    // Kernels without getrandom(): the same CSPRNG through its device file.
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(p, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
}
