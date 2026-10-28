# vnet

Virtual UDP for games: a small, dependency-free C11 library with a non-blocking datagram API and pluggable backends.

| Backend | Use |
|---|---|
| native UDP (`VNET_BACKEND_UDP`) | BSD sockets / Winsock, IPv4 and IPv6 |
| in-process loopback (`VNET_BACKEND_LOOPBACK`) | deterministic multi-peer tests in one thread |
| relay over UDP (`VNET_BACKEND_RELAY_UDP`) | native clients in shared rooms with virtual addresses `10.0.0.N` |
| relay over WebSocket (`VNET_BACKEND_RELAY_WS`) | browser (Emscripten) clients, in the same rooms as native ones |
| impairment layer | latency, jitter, loss, duplication, reordering on top of any backend |

The relay itself is a small Go server in [`relay/`](relay/README.md); its wire protocol is in [`doco/relay-protocol.md`](doco/relay-protocol.md). A WebTransport backend is planned (see [`doco/`](doco/README.md)).

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

## Playing through the relay (browser and native together)

Start a relay (`relay/README.md`: `go build`, or `docker run`), then point a context at it. The relay backends connect and enter a room asynchronously, so poll `vnet_poll()` and check `vnet_status()`:

```c
vnet_config cfg = {.backend = VNET_BACKEND_RELAY_UDP,   /* VNET_BACKEND_RELAY_WS in an Emscripten build */
                   .relay = "relay.example.org:7777",   /* ws://host:8080/vnet or wss://host/vnet for _WS */
                   .room = NULL};                       /* NULL: create a room; "K7M2QX": join it */
vnet *v = vnet_create(&cfg, NULL);
int s = vnet_open(v, 5000);

vnet_status_info st;
while (vnet_status(v, &st) == VNET_STATUS_CONNECTING) { vnet_poll(v); /* once per frame */ }
if (st.state == VNET_STATUS_FAILED) { /* st.error: VNET_ENOROOM, VNET_EROOMFULL, VNET_ETIMEDOUT, ... */ }
/* st.room is the code to show the other players; st.self is our address, 10.0.0.N */

vnet_addr host;
vnet_resolve(v, "room-host:5000", &host);              /* the creator is always 10.0.0.1 */
vnet_sendto(v, s, "hello", 5, &host);                  /* from here on, the ordinary API */
```

Everything after setup is the usual API: sockets are local ports, destinations are `10.0.0.N:port`, datagrams are unreliable and at most `VNET_MAX_DATAGRAM` bytes. Datagrams only ever reach members of your room. Over WebSocket the transport happens to be reliable and ordered; do not rely on that. A context that falls out of its room (relay restart, timeout) becomes `VNET_STATUS_FAILED` with `VNET_ECLOSED` or `VNET_ETIMEDOUT`; create a new one to rejoin.

`examples/relay_peer.c` is a complete two-player program for both worlds. Building for the browser needs a pinned Emscripten SDK:

```sh
scripts/setup-emsdk.sh                 # once: installs emsdk into .emsdk (git clone + pinned release)
scripts/test-browser.sh                # builds relay, native peer and wasm page; runs Chromium via Playwright
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

With Go installed, the same `ctest` run also builds the relay, runs its Go tests (`relay_go_tests`) and the integration tests that start it (option `VNET_BUILD_RELAY`, default on when `go` is found). `scripts/test-browser.sh` runs the Chromium test, and `scripts/loadtest.sh` the load sanity test (50 rooms x 4 clients x 30 packets/s).

The test suite needs GoogleTest (`apt install libgtest-dev`, or it is fetched with `FetchContent` from git). The IPv6 UDP test needs a `::1` interface; it skips without one unless `VNET_REQUIRE_IPV6=1` is set in the environment (CI sets it), in which case a missing `::1` fails the test. Options: `VNET_BUILD_TESTS`, `VNET_BUILD_SMOKE` (only the plain-C smoke test, for cross builds), `VNET_INSTALL`, `VNET_WARNINGS_AS_ERRORS`.

Windows via MinGW-w64 cross-compile:

```sh
cmake -S . -B build-mingw -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_SMOKE=ON
cmake --build build-mingw      # tests/vnet_smoke.exe runs under Wine
```

Licensed under the [MIT licence](LICENSE).
