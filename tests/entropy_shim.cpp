// SPDX-License-Identifier: MIT
// Test seam for getentropy() (GNU ld --wrap): lets a test make the OS random
// source fail, to prove that vnet then fails rather than use something weaker.
#include <cerrno>
#include <cstddef>

extern "C" {
int g_getentropy_errno = 0;  // 0: pass through; otherwise fail with this errno
int __real_getentropy(void *, size_t);
int __wrap_getentropy(void *buf, size_t n) {
    if (g_getentropy_errno != 0) {
        errno = g_getentropy_errno;
        return -1;
    }
    return __real_getentropy(buf, n);
}
}
