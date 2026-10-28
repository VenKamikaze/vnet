/* SPDX-License-Identifier: MIT */
/* Force-included into vnet's own sources in developer builds (top-level
 * project, GCC/Clang, not Windows), after the C library headers. These
 * functions are deprecated or unsafe on some platforms (Apple's Libc marks
 * sprintf deprecated and the macOS build runs with -Werror), so using one is
 * a compile error everywhere, not only on that platform. A name that the C
 * library has turned into a macro (e.g. sprintf under _FORTIFY_SOURCE) cannot
 * be poisoned without a warning, so it is skipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef sprintf
#pragma GCC poison sprintf
#endif
#ifndef vsprintf
#pragma GCC poison vsprintf
#endif
#ifndef strcpy
#pragma GCC poison strcpy
#endif
#ifndef strcat
#pragma GCC poison strcat
#endif
#ifndef gets
#pragma GCC poison gets
#endif
