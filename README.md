# vnet

Virtual UDP for games: a small, dependency-free C11 library with a non-blocking datagram API and pluggable backends.

| Backend | Use |
|---|---|
| native UDP (`VNET_BACKEND_UDP`) | BSD sockets / Winsock, IPv4 and IPv6 |
| in-process loopback (`VNET_BACKEND_LOOPBACK`) | deterministic multi-peer tests in one thread |
| impairment layer | latency, jitter, loss, duplication, reordering on top of any backend |

Relay, WebSocket and WebTransport backends for browser builds are planned (see [`doco/`](doco/README.md)).

vnet carries **unreliable, unordered datagrams only** (at most `VNET_MAX_DATAGRAM` = 1100 bytes). Reliability and ordering are the game protocol's job. Nothing blocks, there are no threads and no global state, and steady-state send/receive does not allocate.

The API is documented in [`include/vnet.h`](include/vnet.h).

## Using it from CMake

As a subdirectory or with `FetchContent` (use `GIT_REPOSITORY`; archive downloads can be blocked on some CI/cloud hosts):

```cmake
include(FetchContent)
FetchContent_Declare(vnet
    GIT_REPOSITORY https://github.com/VenKamikaze/vnet.git
    GIT_TAG        v0.1.0)          # pin a release tag or commit
FetchContent_MakeAvailable(vnet)

target_link_libraries(mygame PRIVATE vnet::vnet)
```

Until `v0.1.0` is tagged, the code lives on the `claude` branch (use `GIT_TAG claude`, or better a commit hash). (Or `add_subdirectory(ext/vnet)` for a submodule.) As a subproject, the tests and install rules are off by default.

Or install it and use `find_package`:

```sh
cmake -S vnet -B build && cmake --build build
cmake --install build --prefix /some/prefix
```

```cmake
find_package(vnet 0.1 REQUIRED)     # with -DCMAKE_PREFIX_PATH=/some/prefix
target_link_libraries(mygame PRIVATE vnet::vnet)
```

Either way the target `vnet::vnet` brings the include path (`#include "vnet.h"`, valid C and C++) and, on Windows, `ws2_32`.

## Example: two sockets exchange a datagram

This uses the in-process loopback backend; swap `VNET_BACKEND_LOOPBACK` for `VNET_BACKEND_UDP` (and real ports) for the network.

```c
#include "vnet.h"
#include <stdio.h>

int main(void) {
    vnet_loopback_network *net = vnet_loopback_network_create();
    vnet_config cfg = {.backend = VNET_BACKEND_LOOPBACK, .loopback_network = net};
    vnet *a = vnet_create(&cfg, NULL);   /* host 127.1.0.1 */
    vnet *b = vnet_create(&cfg, NULL);   /* host 127.1.0.2 */
    int sa = vnet_open(a, 0), sb = vnet_open(b, 5000);

    vnet_addr to;
    vnet_resolve(a, "127.1.0.2:5000", &to);
    vnet_sendto(a, sa, "hello", 5, &to);

    vnet_poll(b);                        /* delivery happens on poll */
    char buf[VNET_MAX_DATAGRAM];
    vnet_addr from;
    int n = vnet_recvfrom(b, sb, buf, sizeof buf, &from);   /* VNET_EAGAIN if empty */
    if (n >= 0) {
        char who[64];
        vnet_addr_to_string(&from, who, sizeof who);
        printf("%.*s from %s\n", n, buf, who);
    }

    vnet_destroy(a); vnet_destroy(b);
    vnet_loopback_network_destroy(net);
}
```

## Testing under bad network conditions

Any context can be impaired from code (`vnet_config.impair`) or, with no code changes, from the environment:

```sh
VNET_IMPAIR="lat=80,jit=20,loss=5,dup=1,reorder=3,seed=7" ./mygame
```

Keys: `lat`, `jit` (ms), `loss`, `dup`, `reorder` (percent, decimals allowed), `rdelay` (extra ms for reordered packets, default 50), `seed`. The same seed, settings and sends give exactly the same outcome. **Without `seed=` every process gets seed 0 and therefore the same loss pattern**; give each peer its own (`seed=1`, `seed=2`, ...). Impairment is applied to what a context *sends*; delayed datagrams are released by `vnet_poll()`, so keep polling once per frame. A malformed `VNET_IMPAIR` makes `vnet_create()` fail rather than silently testing nothing; set `vnet_config.ignore_env` to opt out of the variable.

On the loopback backend time is virtual (`vnet_loopback_network_step(net, ms)`), so latency tests never sleep.

## Building and testing

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

`vnet_time(v)` gives the context's clock in milliseconds (virtual time on loopback), so timeout and RTT code can run on loopback in tests.

The test suite needs GoogleTest (`apt install libgtest-dev`, or it is fetched with `FetchContent` from git). The IPv6 UDP test needs a `::1` interface; it skips without one unless `VNET_REQUIRE_IPV6=1` is set in the environment (CI sets it), in which case a missing `::1` fails the test. Options: `VNET_BUILD_TESTS`, `VNET_BUILD_SMOKE` (only the plain-C smoke test, for cross builds), `VNET_INSTALL`, `VNET_WARNINGS_AS_ERRORS`.

Windows via MinGW-w64 cross-compile:

```sh
cmake -S . -B build-mingw -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_SMOKE=ON
cmake --build build-mingw      # tests/vnet_smoke.exe runs under Wine
```

Licensed under the [MIT licence](LICENSE).
