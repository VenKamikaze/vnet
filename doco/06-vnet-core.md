# 06 — vnet core: API, loopback and native UDP

- **Repo:** `VenKamikaze/vnet` (this repo)
- **Branch:** `vnet/06-core`, from `claude`
- **Depends on:**
  - nothing in this repo
  - none of BitRiot's briefs either: this runs in parallel with BitRiot's determinism track (01–05b)
- **Status:** done

## Goal

Build a small, dependency-free C library that gives games a non-blocking, datagram-style "virtual UDP" API. Backends sit underneath it.

This brief delivers:

- the **API**
- a **loopback** backend: in-process, deterministic, for tests
- a **native UDP** backend
- an **impairment** layer (latency, jitter, loss, duplication, reordering) that can wrap any backend

Briefs 10 and 11 add relay and browser backends without changing the API.

## Background

- Every target game already isolates its networking behind a small driver: Bit Riot's netplay layer (07), jfbuild's `mmulti.c`, Doom ports' `i_net.c`. vnet slots in under those drivers.
- Browsers have no UDP. They get WebSocket and WebTransport (briefs 10–11) through a relay. That is why the API is "virtual": a relay can assign addresses.
- Lockstep games need every packet but tolerate latency. Client/server games such as Odamex want unreliable datagrams. vnet itself is **unreliable datagrams only**, and reliability is the game protocol's job (D7). Keep it that way.

## API sketch

Refine it, but keep the shape: C11, a single public header `vnet.h`, and `extern "C"`-safe.

```c
typedef struct vnet vnet;                 // one network context ("host"); no global state
typedef struct vnet_addr { uint8_t family; uint8_t bytes[16]; uint16_t port; } vnet_addr; // or opaque + accessors
vnet *vnet_create(const vnet_config *cfg, int *err);   // backend: VNET_BACKEND_UDP | _LOOPBACK (later _RELAY_*); NULL + *err (VNET_E* code) on failure
void  vnet_destroy(vnet *v);
int   vnet_open(vnet *v, uint16_t port);     // socket handle within v; port 0 = ephemeral
void  vnet_close(vnet *v, int s);
int   vnet_sendto(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to);   // never blocks
int   vnet_recvfrom(vnet *v, int s, void *buf, size_t cap, vnet_addr *from);          // never blocks; VNET_EAGAIN when empty
void  vnet_poll(vnet *v);                    // pump the backend once per frame (needed by async/browser backends)
int   vnet_local_addr(vnet *v, int s, vnet_addr *out);
int   vnet_resolve(vnet *v, const char *host_port, vnet_addr *out);  // "1.2.3.4:5000", "[::1]:5000"; relay backends add their own forms later
int   vnet_addr_to_string(const vnet_addr *a, char *buf, size_t cap);
int   vnet_addr_equal(const vnet_addr *a, const vnet_addr *b);
const char *vnet_strerror(int err);
```

A **context handle** is what lets several loopback "hosts" exist in one process, each with its own virtual address. It also means there is no global state, and create/destroy can be repeated freely. Loopback contexts that should see each other share a `vnet_loopback_network` object, passed in `vnet_config`.

**Rules**

- **Max payload:** `VNET_MAX_DATAGRAM` = **1100 bytes**. That fits inside a WebTransport datagram at the minimum path MTU, after QUIC and WebTransport overhead plus relay framing (briefs 10 and 11). Lockstep packets are tiny anyway.
  - This constant becomes part of the ABI once games use it, so don't raise it later.
  - Oversize sends fail with a clear error and are never truncated.
- **No blocking calls**, no threads, and no allocation per packet in steady state (use ring buffers).
- **Logging** goes through a user-supplied callback, silent by default.

## In scope

1. **Repo skeleton:**
   - `CMakeLists.txt` with a `vnet` static library target.
   - Usable via `add_subdirectory`/`FetchContent` and via `find_package(vnet)` after install (export a config package).
   - `include/vnet.h`, `src/`, `tests/`, and expand `README.md` (`doco/` already exists).
   - The licence is MIT (already in `LICENSE`). Add SPDX headers (`// SPDX-License-Identifier: MIT`) to source files.
2. **Loopback backend:**
   - Multiple "hosts" in one process, each with its own virtual address (for example `127.1.0.N`).
   - Delivery happens on `vnet_poll()`, in a deterministic order, so that a test can run several peers in a single thread with exact repeatability.
3. **Native UDP backend:**
   - BSD sockets, non-blocking, IPv4 and IPv6.
   - Builds on Linux and macOS. Windows/Winsock should at least compile behind `#ifdef`; full Windows CI isn't required. Bit Riot has a MinGW build, so keep that path in mind.
4. **Impairment layer:**
   - Configurable latency, jitter, loss %, duplicate % and reorder %, using a seeded PRNG so results repeat.
   - It wraps any backend: on loopback it is used in unit tests, and on UDP it is used in multi-process tests in Bit Riot's brief 07.
   - Enable it through `vnet_config` and through environment variables (for example `VNET_IMPAIR="lat=80,jit=20,loss=5"`), so a game can be tested under bad conditions without code changes.
5. **Tests:**
   - GoogleTest, for consistency with Bit Riot. Get it from the system package (`libgtest-dev`, `find_package(GTest)`) or with `FetchContent` using `GIT_REPOSITORY`, **never an archive URL**; see the network notes in [`README.md`](README.md).
   - **MinGW:** Bit Riot has a MinGW build, and its netplay option will default to ON. Cross-compile vnet with `mingw-w64` from apt and record whether it builds. If it can't be verified, say so, so that brief 07 defaults `BITRIOT_NETPLAY` to OFF on MinGW.
   - Coverage:
     - send/recv on loopback and on UDP over 127.0.0.1
     - `EAGAIN` semantics
     - oversize rejection
     - address round-trip to string
     - impairment statistics within tolerance over N packets
     - deterministic impairment for a given seed
     - repeated init and shutdown
6. **CI:**
   - A GitHub Actions workflow that builds and tests on ubuntu-latest.
   - A macOS job is optional.
   - The cloud session can't watch Actions runs, so verify locally as well.

## Out of scope

- Relay, WebSocket and WebTransport (briefs 10 and 11).
- Reliability and ordering layers.
- Encryption.
- A POSIX socket shim, i.e. intercepting `socket()`/`sendto()`. Possibly later, for unmodified games.

## Acceptance criteria

- `vnet.h` is documented: every function, error code, threading and blocking guarantees, and max payload.
- Loopback, UDP and impairment are tested and green locally and in CI.
- A short `README.md` explains how to consume vnet from a CMake project, with a 20-line example of two sockets exchanging a datagram.
- Bit Riot can consume it, via a submodule or `FetchContent` with `GIT_REPOSITORY`. Verify this by building a throwaway consumer. The repo is public, so no credentials are needed to fetch it.

## Outcome

Landed on `vnet/06-core`. Everything in scope is implemented and tested; nothing was cut.

**What is there**

- `include/vnet.h`: the single public header, documented per function, error code, threading/blocking guarantee and payload limit. Valid C11 and C++ (a test compiles it as C).
- `src/`: `vnet.c` (core, validation, stats, logging), `addr.c` (own numeric IPv4/IPv6 parser and RFC 5952 formatter), `impair.c`, `loopback.c`, `udp.c` (POSIX and Winsock behind `#ifdef _WIN32`).
- CMake: static target `vnet` with alias `vnet::vnet`; works through `add_subdirectory`, `FetchContent` (`GIT_REPOSITORY`) and `find_package(vnet)` after `cmake --install` (exported targets, version file, `SameMajorVersion`). Tests and install rules are on only when vnet is the top-level project.
- `.github/workflows/ci.yml`: ubuntu-latest (gcc, clang, gcc with ASan+UBSan, `-Werror`), macOS (clang), an install-and-`find_package` check, and a MinGW cross build whose smoke test runs under Wine.
- `README.md`: how to consume it, a two-socket example, `VNET_IMPAIR`, building and testing.

**API deviations from the sketch**

- `vnet_create(cfg, int *err)` takes an optional error out-parameter, because the sketch said "NULL + error" without saying where the error goes.
- `vnet_open` binds the wildcard address (UDP) or the host's virtual address (loopback). `vnet_config.family` selects the UDP socket type: `VNET_FAMILY_ANY` (default) is a dual-stack IPv6 socket that also reaches IPv4 peers, falling back to an IPv4 socket if the OS has no IPv6; `VNET_FAMILY_IPV4` is IPv4 only; `VNET_FAMILY_IPV6` is IPv6 only (`IPV6_V6ONLY=1`) and returns `VNET_EAFNOSUPPORT` for IPv4 destinations. IPv4-mapped source and destination addresses are converted so a peer always compares equal to what `vnet_resolve()` returns.
- `vnet_resolve` is numeric only (plus the literal `localhost`), so it can never block on DNS.
- `vnet_sendto` returns the byte count on success; `vnet_recvfrom` returns the datagram length. Zero-length datagrams are allowed.
- A too-small receive buffer returns `VNET_EMSGSIZE` and consumes the datagram (never silent truncation). UDP datagrams over `VNET_MAX_DATAGRAM` from foreign senders are dropped and counted (`rx_oversize`).
- Additions: `vnet_config_init`, `vnet_get_stats`/`vnet_stats`, `vnet_impair_parse`, `vnet_time` (the context's clock: virtual on loopback, real or injected on UDP, so brief 07 can run RTT/timeout logic on loopback virtual time), and the loopback network API (`vnet_loopback_network_create/destroy/time/advance/step`).
- Impairment (`vnet_impair`, applied to what a context sends): `latency_ms`, `jitter_ms`, `loss_pct`, `duplicate_pct`, `reorder_pct`, `reorder_delay_ms` (0 = 50 ms) and `seed`. Delayed datagrams are released by `vnet_poll()` and on every `vnet_sendto()`. The PRNG is splitmix64 with a fixed draw order, so changing one percentage does not shift the draws of the others.
- `VNET_IMPAIR` **replaces** the configured impairment when set (so a developer's shell can degrade any build) unless `vnet_config.ignore_env` is 1; a malformed value makes `vnet_create` fail with `VNET_EINVAL` instead of testing nothing.
- Time is milliseconds everywhere, through an optional `vnet_clock_fn`. The loopback backend defaults to a virtual clock owned by the loopback network, so impairment tests are exact and never sleep. `vnet_loopback_network_step(net, ms)` advances the clock, releases every context's due datagrams, then polls every context.
- Loopback delivery model: a send stages the datagram at the destination; it is receivable only after the **destination** calls `vnet_poll()`. Delivery order is global send order. Sending to `127.0.0.1` reaches the sender's own host. Unknown destinations vanish (counted as `unroutable`), as with UDP.
- Limits: `max_sockets` (default 16), `rx_queue_len` (loopback, default 128 datagrams per socket), `impair_queue_len` (default 256 delayed datagrams). All memory is allocated in `vnet_create`/`vnet_open`; a test counts heap calls over 20000 send/recv/poll rounds on both backends and sees none.

**Tests** (GoogleTest, 106 tests, plus the plain-C smoke test: 107 ctest entries; 106 pass and 1 skips locally, see the IPv6 note)

- Addresses (including a 5000-address random round trip), loopback, UDP over 127.0.0.1, impairment (rates within 5 sigma over 10-20k packets, exact latency, jitter bounds, bit-identical traces for equal seeds), `VNET_IMPAIR` handling, lifecycle (500 loopback and 300 UDP create/destroy cycles, fd-leak check, same port rebound every time), steady-state allocation.
- Mutation check: 33 deliberate breakages (oversize check removed or turned into truncation, EAGAIN, delivery-on-poll, deterministic seeding, loss/latency/duplication/reorder/jitter, env handling, address parsing, socket leaks and so on) were each applied to a scratch copy. 31 make at least one test fail (a blocking-socket mutation is caught by the 60 s per-test timeout), raising `VNET_MAX_DATAGRAM` fails the build through a `_Static_assert`, and one pure memory leak (close not freeing the queue) is caught by LeakSanitizer in the ASan run, not by the plain build. Two gaps found along the way got new tests (bidirectional `step()`; a pattern fix in the mutation list). The review fix round added and mutation-checked: IPv6-only policy (two mutations), release in due order under coarse frames (`CoarseFramesStillReleaseInDueOrder` fails if release uses send order), `vnet_time`, and `wire_datagrams` accounting.
- Clean under ASan+UBSan and under clang with `-Werror`; 40 consecutive runs under CPU load had no failures. UDP tests poll with a bounded real-time deadline; impairment on UDP uses an injected clock.
- **The IPv6 UDP paths run only in CI, not in the authoring container**, which has no IPv6 at all (no kernel support, so no `::1`). `UdpFamily.Ipv6LoopbackRoundTrip` needs `::1`: locally it skips, but with `VNET_REQUIRE_IPV6=1` (set in every CI job, which also enables `::1` on Linux and fails if it is missing) a missing `::1` is a failure, and the dual-stack test then also asserts the socket really is IPv6, so the IPv4-mapped conversion runs. The review caught a real bug that the skip had hidden (`VNET_FAMILY_IPV6` sockets were dual-stack and accepted IPv4 destinations); it is fixed, and `UdpFamily.Ipv6OnlyModeSetsV6OnlyAndRefusesIpv4Destinations` guards the policy on any host through an emulated-socket seam (`tests/ipv6_shim.cpp`, GNU ld `--wrap`; it fails without the fix). IPv6 address parsing and formatting are fully tested everywhere. The allocation test and the seam use `--wrap`, so they are Linux only.
- A second shim switch makes `sendto` fail with `ENETUNREACH`, to test that `VNET_EIO` is returned and that `wire_datagrams` does not count failed sends.

**MinGW**

`mingw-w64` from apt (GCC 13, x86_64 and i686) builds `libvnet.a` and the plain-C smoke test with `-Wall -Wextra -Wpedantic -Werror`. The x86_64 smoke test **runs under Wine 9.0**: UDP over 127.0.0.1, EAGAIN, oversize rejection, port-in-use, loopback latency and address round trip all pass on the real Winsock code path. i686 builds and links but was not run (64-bit Wine only). So `BITRIOT_NETPLAY` can default to ON on MinGW (this was verified for a cross build, not a native MSYS2 build, and the GoogleTest suite is not run on Windows). Wine here had no IPv6, so it fell back to IPv4 sockets.

**Consumer check** (throwaway C++ project outside the repo): linking `vnet::vnet` and including `<vnet.h>` worked via `add_subdirectory`, via `FetchContent` with `GIT_REPOSITORY file://...` and `GIT_TAG vnet/06-core`, and via `find_package(vnet 0.1)` against a `cmake --install`ed prefix; a request for version 1.0 is correctly refused. `tests/consumer` repeats the install check in CI.

**Not verified / follow-ups**

- The macOS CI job and the GitHub Actions workflow itself could not be run here. The macOS `-Werror` failure from `sprintf` was fixed and `cmake/banned.h` now poisons `sprintf`, `vsprintf`, `strcpy`, `strcat` and `gets` in the library sources on POSIX builds, so such a use fails on Linux too; the Linux job steps (system GTest, FetchContent GTest from git, ASan config, install and consume) and the MinGW/Wine steps were reproduced locally.
- IPv6 UDP is untested in this container (see above); CI is where it runs.
- `vnet_set_impair()` (changing impairment on a live context, e.g. from an in-game debug menu) is not provided; add it if brief 07 needs it.
- The delayed-datagram release scans the queue (O(capacity) per released datagram); fine at the default 256, revisit if queues grow.
- The MinGW i686 build is verified to compile and link only (no 32-bit Wine).
- UDP sockets always bind the wildcard address; there is no bind-to-interface, broadcast or multicast. Add only if a consumer needs it.
- A loopback context should be destroyed before its network; if the network goes first the context degrades safely (sends become `unroutable`) rather than crashing.
- For brief 07: call `vnet_poll()` every frame on UDP when `VNET_IMPAIR` is used, give each process a distinct `seed=` so their loss patterns differ, and note `vnet_resolve` takes `host:port` only (no DNS).
