// SPDX-License-Identifier: MIT
// Test seam for getentropy() (GNU ld --wrap): lets a test make the OS random
// source fail, or return known bytes, without any hook in the library itself.
#include <cerrno>
#include <cstddef>
#include <cstdint>

extern "C" {
int g_getentropy_errno = 0;  // 0: pass through; otherwise fail with this errno
int g_getentropy_fill = -1;  // >= 0: succeed with the bytes fill, fill+1, ...
int __real_getentropy(void *, size_t);
int __wrap_getentropy(void *buf, size_t n) {
    if (g_getentropy_errno != 0) {
        errno = g_getentropy_errno;
        return -1;
    }
    if (g_getentropy_fill >= 0) {
        for (size_t i = 0; i < n; i++) static_cast<uint8_t *>(buf)[i] = (uint8_t)(g_getentropy_fill + (int)i);
        return 0;
    }
    return __real_getentropy(buf, n);
}
}
