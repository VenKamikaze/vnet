/* SPDX-License-Identifier: MIT */
/* Force-included into vnet's own sources (GCC/Clang) after the C library
 * headers. These functions are deprecated or unsafe on some platforms (Apple's
 * Libc marks sprintf deprecated, and the macOS build runs with -Werror), so
 * using one is a compile error everywhere, not only on that platform. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC poison sprintf vsprintf strcpy strcat gets
