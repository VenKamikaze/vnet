// SPDX-License-Identifier: MIT
// Counts heap allocations (GNU ld --wrap) so a test can prove vnet's steady
// state is allocation-free.
#include <cstddef>

extern "C" {
unsigned long g_alloc_calls = 0;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n) { g_alloc_calls++; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { g_alloc_calls++; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { g_alloc_calls++; return __real_realloc(p, n); }
}
