# vnet design review (v0.3.0, `claude` b302029)

> **Status (29 Oct 2026): acted on by [brief 14](14-vnet-api-polish.md), released as v0.4.0.** Brief 14 took up R1 to R9, R12 (burst loss only, no bandwidth cap), R13 (`SO_RCVBUF` and the WebSocket control queue; not the `size` field), the vendoring recipe of R10, and the protocol notes of R11 and R14 (the extension mechanism, the reserved addresses and the Odamex notes, in [`relay-protocol.md`](relay-protocol.md), no code). It left out R11's room control and secret, R14's rate limits and fragmentation helper, R15, R10's pkg-config file and amalgamation, and a bandwidth cap. The text below is the review as written; where the code showed it to be wrong, the entry carries a bracketed *Correction (brief 14)*. The Outcome of brief 14 lists the deviations.

A read-only review of vnet's design from the point of view of its consumers: Bit Riot's briefs 07 (native lockstep), 12 (browser netplay) and 13 (disconnects and hot-join), and later jfSW (`mmulti.c`) and Odamex. It covers the public API, lifecycle and event-loop fit, the backend abstraction, the relay protocol, testability, observability and packaging. Everything here was checked against the headers, the sources, the tests and the briefs' Outcome sections; nothing was run.

**Verdict.** The core design is sound and settled enough to build 07 on today. The API is small, the guarantees (no blocking, no threads, no steady-state allocation, unreliable only, 1100-byte datagrams) are the right ones for lockstep games, and the relay's security work is far beyond what a casual-games relay usually gets. Nothing below reverses a settled decision (D3–D5, D7, D9). The recommendations are mostly small additive changes; the one real bug is R1 (a browser page that was suspended for more than 15 s loses its relay session on resume, which brief 12's background-tab work would hit at once).

## Summary

Priority: **high** = would cost a Bit Riot brief real time or produce a user-visible failure; **medium** = clearly worth it, cheap; **low** = nice to have. "When": *07* = do before Bit Riot 07 starts (or early in it), *12* = before Bit Riot 12, *later* = when a consumer needs it. Cost is a rough implementer estimate including tests. None of the recommendations is a breaking change unless marked.

| # | Recommendation | Priority | When | Breaking | Cost |
|---|---|---|---|---|---|
| R1 | Relay client: treat a gap in `vnet_poll()` calls as "we were asleep", not "the relay went silent" (hidden tab, laptop lid, debugger) | high | 12 | no | small |
| R2 | `vnet_set_impair()`: change impairment on a live context (outage/recovery tests, debug menu) | medium | 07 | no | small |
| R3 | Optional wake callback (`vnet_config.wake`) fired when a browser frame arrives, so a hidden tab can step from network events (12, option a) | medium | 12 | no | small–medium |
| R4 | Make `VNET_BACKEND_RELAY_AUTO` portable: natively it means `RELAY_UDP` (new `relay_udp` config field), so one config and no `#ifdef` per game | medium | 12 | no | small |
| R5 | Expose `relay_rtt_ms` and `max_members` in `vnet_status_info` (D estimation over the relay; lobby cap) | medium | 12 | no | small |
| R6 | Write a "game-loop integration" page: status state machine, SDL and Emscripten main loops, what to do on `FAILED`, ports/ephemeral, per-frame budget | medium | 07 | no | docs |
| R7 | Documentation fixes: `VNET_EAGAIN` is not "UDP only"; protocol v1 is frozen as of v0.2.0 (not "unreleased"); API-stability policy and the `SameMajorVersion` caveat at 0.x; add `vnet_version()` | medium | 07 | no | docs, trivial code |
| R8 | `vnet_wait(v, timeout_ms)`: block until a datagram or timeout (UDP, relay-UDP), for headless tests and dedicated servers | medium | later (Odamex, `bitriot_headless` CPU) | no | small |
| R9 | `vnet_resolve_host()`: an explicit blocking DNS helper, so games can resolve on their own terms; `vnet_create()` keeps accepting names for now | low | 12 | no | small |
| R10 | Packaging for non-CMake consumers: `vnet.pc`, a documented vendoring recipe (`src/*.c` + `include/`), optionally a single-file amalgamation | low | later (jfSW) | no | small |
| R11 | Relay: creator-only room control (`CLOSE_ROOM`, `KICK`) and an optional room secret; fits protocol v1 as new frame types | low | later (08/12 lobby polish) | no (protocol additive) | medium |
| R12 | Impairment: correlated (burst) loss and a bandwidth cap | low | later | no | small–medium |
| R13 | UDP backend: raise `SO_RCVBUF` (or a `so_rcvbuf` option); WS inbox: separate control queue like WT | low | later | no | small |
| R14 | Gateway groundwork (Odamex via browser): per-room/role rate limits, address space for a gateway member, and a fragmentation helper outside the core | low | later (gateway brief) | no | design note now |
| R15 | Loopback relay emulation (rooms, `10.0.0.N`, CONNECTING→READY) for in-process tests of a game's room flow | low | later | no | medium |

**Keep as is** (no change recommended): the API shape (`create/open/sendto/recvfrom/poll/status/destroy`, `int` handles, negative `VNET_E*` codes, `vnet_strerror`); `vnet_config` zero-is-default with `vnet_config_init`; the status model (`CONNECTING/READY/FAILED`, `FAILED` final, `ENOTCONN` before the room); `vnet_addr` as a plain struct; `vnet_time()` with virtual time on loopback; the loopback delivery model and `vnet_loopback_network_step`; `VNET_IMPAIR` replacing the configured impairment; the backend and `relay_transport_ops` split; the relay's stateless retry, path validation, anti-amplification and per-address caps; the `vnet::vnet` CMake target, `find_package` export, `FetchContent` by `GIT_REPOSITORY`, release tags; the test discipline (mutation checks, fake clocks, `--wrap` seams); D7 (unreliable only).

---

## 1. Ease of use and integration

### What works well

- **One header, one call per concept.** A C driver like jfbuild's `mmulti.c` maps onto it one to one: it already uses one non-blocking socket polled with `recvmsg` until empty (no `select`), 256-byte packets (`MAXPAKSIZ`) and peers identified by address, so `vnet_open(v, port)`, `vnet_sendto`/`vnet_recvfrom` in `sendpacket`/`getpacket`, `vnet_addr_equal` for peer identity and `vnet_poll` once per frame replace it line for line. Its `lookuphost()` calls `getaddrinfo` itself, which is where R9's helper slots in. Odamex's `NET_SendPacket`/`NET_GetPacket` and its `netadr_t` (`ip[4]`, `port`) map the same way, with `vnet_addr` carrying IPv6 too; its packet sizes are the one mismatch (R14).
- **Polling fits every target loop.** Nothing blocks, so SDL's main loop, `emscripten_set_main_loop`, `bitriot_headless` and a Go-style server loop can all drive it. `vnet_poll()` is the only obligation and it is cheap.
- **Lifecycle is simple and honest.** `vnet_create` reports failure with a code, `vnet_destroy` is idempotent on NULL, contexts are independent, and `FAILED` is final with "make a new context" as the documented recovery. For brief 13, a reconnecting peer is a new context; that is the right model (cheap, no half-states).
- **Config and ownership are unambiguous.** Strings in `vnet_config` need only live for the duration of `vnet_create`; receive copies into the caller's buffer; `vnet_status_info` is filled, not allocated. Per-packet allocation is zero in vnet's own code.
- **Error reporting** is a flat negative-int space with transient codes called out (`EAGAIN`, `EIO`). Enough for games; details go to the log callback.
- **Build consumption** is right: `vnet::vnet` via `add_subdirectory`, `FetchContent` (`GIT_REPOSITORY`, pinned tag) and `find_package` after install; tests and install off as a subproject; Emscripten link flags (`-lwebsocket.js`) propagate through the target; `ws2_32` on Windows; MinGW verified.

### Gaps (see R-numbers for the recommendations)

- **No wake-up primitive, in either direction.** A browser page cannot be told "a frame arrived, poll now" (R3), and a native headless process cannot sleep until one does (R8). The first matters for brief 12's hidden-tab options; the second for CPU use in headless tests and for an Odamex-style server, which today would spin with `nanosleep(1 ms)` as `examples/relay_peer.c` and `tests/smoke.c` do.
- **Backend selection leaks into the game.** Today a game needs `#ifdef __EMSCRIPTEN__` to pick `RELAY_AUTO` vs `RELAY_UDP` and to format the relay address differently (`host:port` vs `wss://.../vnet`). `examples/relay_peer.c` does this by URL scheme (not by `#ifdef`). R4 removes the need.
- **Hostname resolution blocks inside `vnet_create()`** for `RELAY_UDP` (documented). With a relay URL from settings, a bad network can freeze the UI for the resolver's timeout at "connect". R9.
- **`vnet_sendto` can return `VNET_EAGAIN` on relay backends** (WebSocket backpressure in `relay_ws.c:tp_send`, too many datagrams in flight in `relay_wt.c`), but `vnet.h` says "UDP only". A driver that treats unknown negatives as fatal would tear down a session on a burst. R7.
- **Version at run time.** Only macros exist; a game cannot log which vnet it linked. Trivial (R7).
- **Non-CMake consumers.** jfSW builds with GNU make. vnet is eleven `.c` files with no dependencies, so vendoring is easy, but nothing says so and there is no `pkg-config` file. R10.

## 2. Design improvements

### R1. Relay client: a gap in polling is sleep, not relay silence (high, before 12, not breaking)

**What.** In `src/relay_client.c:rc_poll`, the READY branch declares the session lost when `now - last_rx > 3 * keepalive_ms`. `last_rx` is only advanced by frames the client processes, and the client only sends `KEEPALIVE` when it is polled. A page whose main loop stopped (hidden tab: `requestAnimationFrame` stops entirely, timers run at about 1 Hz), a laptop that slept, or a process paused in a debugger therefore wakes up with `now - last_rx` far past 15 s and, if nothing from the relay is waiting in the inbox, fails with `VNET_ETIMEDOUT` even though the relay kept the session alive (WebSocket pongs and QUIC acknowledgements are answered by the browser without the page). The inbox is empty exactly when the room was quiet: a host alone in the lobby with the tab hidden, two players both backgrounded, a native peer paused. In a busy room other members' datagrams happen to be in the inbox and rescue the session, which is why the browser tests have not seen it. Brief 10's Outcome notes the relay side survives a hidden tab; the client side does not, and brief 11 says so in passing ("the client's three-silent-keepalive-intervals check still applies to a throttled page"). Brief 12's background-tab work will hit this on its first test.

The handshake timer has the same shape (`now - t_start >= hs_timeout_ms`): a page suspended for 10 s while connecting fails instead of finishing.

**Fix.** Remember the time of the previous `rc_poll`. If the gap since then exceeds, say, one keepalive interval, treat it as a suspension: set `last_rx = now`, send a `KEEPALIVE` at once, and extend `t_start` by the gap. The session is then lost only if the relay fails to answer within three intervals *of polling*. The relay already keeps the session (WebSocket pings, QUIC keep-alive) for as long as the browser answers them, so this makes client and relay agree. Natively the same logic covers sleep/debugger pauses for free. *[Correction (brief 14): only in part. Over UDP the relay itself drops a member that was silent for 15 s, so after a longer pause the client's immediate `KEEPALIVE` is answered with `ERROR(NO_SESSION)` and the context ends in `VNET_ECLOSED` at once, instead of `VNET_ETIMEDOUT` after three more intervals. Only a pause shorter than the relay's session timeout is survived natively.]* Test: scripted transport with an injected clock, jump the clock 60 s between two polls, assert the client stays READY and sends a keepalive; mutation (old behaviour) fails it.

**Files.** `src/relay_client.c`; `tests/test_relay_client.cpp`; a note in `vnet.h` under `vnet_status()`. **Cost.** Small (an afternoon).

### R2. `vnet_set_impair()` (medium, before 07, not breaking)

**What.** `int vnet_set_impair(vnet *v, const vnet_impair *imp)`: replace the impairment of a live context (NULL or all-zero turns it off; already-queued delayed datagrams are kept and released on schedule, or dropped: pick one and document it). Brief 06's Outcome lists it as a follow-up "if brief 07 needs it".

**Why.** 07's tests include "stall and catch-up", "leave and timeout" and "host loss is detected". The natural way to write them on loopback is a *change* in conditions mid-test: 500 ms of 100% loss and then recovery, a latency spike, a peer that goes silent. Today the only knobs are fixed at `vnet_create`, so the test has to recreate contexts (losing sockets and queues) or route around it. It is also the hook for an in-game network debug menu (07/08) and for Bit Riot 12's "impairment at the relay" runs when the relay cannot impair.

**Files.** `include/vnet.h`, `src/vnet.c`, `src/impair.c` (`vnet__imp_create`/`destroy` already exist; reseeding should restart the PRNG from `seed` so runs stay reproducible). **Cost.** Small.

### R3. Wake callback for browser builds (medium, before 12, not breaking)

**What.** `typedef void (*vnet_wake_fn)(void *user);` and `vnet_config.wake`/`wake_user`. The relay transports call it (synchronously, from the browser event) when a frame is queued or the transport state changes; it must do nothing but schedule a poll (post a message, set a flag, call `emscripten_async_call`). Native backends never call it (documented). Loopback could call it on delivery, which would let a single-threaded N-peer test drive itself from events, but that is optional.

**Why.** Brief 12 item 5 lists three ways to keep a hidden tab stepping; option (a), "drive stepping from incoming network messages", is the one that needs nothing from the browser (message events are not throttled) and it is impossible today: Emscripten's WebSocket callback (`relay_ws.c:on_message`) and the WebTransport pumps (`relay_wt.c`, in JavaScript) only queue into a ring that `vnet_poll()` drains later. One hook makes option (a) a few lines in the game.

**How.** WebSocket: call the hook at the end of `on_message`/`on_open`/`on_close`. WebTransport: `T.enqueue` and `fail` in the `EM_JS` block call a C function (`Module._vnet_wt_wake(id)` or `EM_ASM`-free `dynCall`; the C side keeps `id -> wt_tp*`), which calls the hook. *[Correction (brief 14): built without an exported function: `vnet_wt_open` is given a C function pointer and a `wt_tp*` and the JavaScript calls them through `getWasmTableEntry` (`EM_JS_DEPS`), so no link flag is needed.]* **Files.** `vnet.h`, `relay_ws.c`, `relay_wt.c`, `tests/browser/*.js` (assert the hook fires in a frozen page). **Cost.** Small–medium (the JS-to-C call is the fiddly part).

### R4. One backend constant for every build (medium, before 12, not breaking)

**What.** Let `VNET_BACKEND_RELAY_AUTO` work natively: it means `RELAY_UDP` there, using a new `const char *relay_udp` field (`host:port`) or, when `relay_udp` is NULL and `relay` has no scheme, `relay` itself. Browser behaviour is unchanged.

**Why.** Brief 12 wires the same netplay layer to the relay for native and browser peers. Without this the game carries `#ifdef __EMSCRIPTEN__` for the backend constant and two differently formatted relay addresses, and `relay_peer.c` shows the shape it takes. *[Correction (brief 14): `relay_peer.c` had no `#ifdef` for this; it chose `RELAY_WS` or `RELAY_UDP` from the URL scheme of `--relay`. It now uses `RELAY_AUTO` with `relay` and `relay_udp`.]* Settings also become simpler: one "relay" record with a UDP address and a WebSocket URL, consumed by one `vnet_config`.

**Files.** `vnet.h` (field at the end of `vnet_config`), `src/relay_auto.c` (native branch), `src/vnet.c` (dispatch), tests. **Cost.** Small.

### R5. Observability for choosing D (medium, before 12, not breaking)

**What.** Add to `vnet_status_info` (new fields at the end, as the struct promises): `int relay_rtt_ms` (the most recent `KEEPALIVE` round trip through the relay; -1 when unknown; UDP/loopback: 0) and `int max_members` (from `ROOM`). Optionally a smoothed RTT.

**Why.** 07 chooses the input delay D from peer RTTs measured by the game's own `PING`/`PONG`, which is right and stays. Over the relay (12) there are two relay hops, and the only early measurement available before any peer answers is the client's own round trip to the relay, which the client already measures implicitly every 5 s but throws away. Exposing it lets the lobby show "relay: 48 ms" and gives the host a floor for D before `PING`s arrive. `max_members` lets the lobby refuse a seventh player with the right message instead of waiting for `VNET_EROOMFULL` on a joiner. Both are one field each; neither changes the wire protocol.

**Files.** `vnet.h`, `src/relay_client.c` (`ka_sent_at`, set `last_rtt` on the one-byte `KEEPALIVE`), tests with the scripted transport. **Cost.** Small.

### R6. A "game-loop integration" page (medium, before 07, docs)

**What.** `doco/integration.md` (or a README section) that a 07/12 implementer reads once: the per-frame sequence (`vnet_poll` → drain `vnet_recvfrom` until `EAGAIN` → game logic → sends); the status state machine with the exact calls allowed in each state; what to do on `FAILED` (destroy, create a new context, re-open ports, re-resolve `room-host`); SDL main loop and `emscripten_set_main_loop` skeletons; ephemeral ports and `vnet_local_addr` for test harnesses; `VNET_IMPAIR` with per-process seeds; the hidden-tab facts from R1/R3; the kernel's ~270-datagram receive backlog and `rx_queue_len`; the clock (`vnet_time`, virtual on loopback) for RTT and timeouts; the one blocking exception (`getaddrinfo`).

**Why.** All of this exists, but scattered across `vnet.h` comments, three briefs' Outcome sections and `relay/README.md`. The briefs are written for a fresh session with no other context; one page saves each implementer an hour of reconstruction and prevents the two mistakes most likely in 07 (not polling in menus; treating `EAGAIN` from `vnet_sendto` as fatal). **Cost.** Docs only.

### R7. Documentation and small API hygiene (medium, before 07)

- `vnet.h:406`: `VNET_EAGAIN` from `vnet_sendto` is documented "UDP only"; relay-WS and relay-WT also return it (backpressure, in-flight cap). Say "any backend: treat as a drop".
- `relay-protocol.md` says of version 1 "(it has not been released)" and that `RETRY`/`PATH_*` were added before release. v0.2.0 and v0.3.0 are tagged; state plainly that **protocol version 1 is frozen** as of v0.2.0 and what may still change additively (new frame types, new error reasons, trailing bytes in `ROOM`).
- **API stability policy.** Write down: 0.x is additive-only from v0.3.0 on (new fields at the end of `vnet_config`/`vnet_status_info`, new error codes, new backends); 1.0 after Bit Riot 07 and 12 have consumed it. Note that CMake's `SameMajorVersion` treats every 0.x as compatible, which is more generous than semver's 0.x rule, so consumers should pin an exact tag (the README already says so).
- Add `const char *vnet_version(void)` returning `"0.3.0"` for logs and bug reports. Trivial.

**Cost.** Docs plus ten lines.

### R8. `vnet_wait()` (medium, later; not breaking)

**What.** `int vnet_wait(vnet *v, int timeout_ms)`: return when a datagram is receivable on any socket of the context, or after `timeout_ms`; returns 1/0/`VNET_ENOTSUP`. UDP and relay-UDP: `poll(2)`/`WSAPoll` on the sockets (relay-UDP: its inner socket; also return early at the next keepalive/retransmit deadline). Loopback and browser: `VNET_ENOTSUP` (loopback is driven by `step`; browsers never block). It is the only call that blocks, by design and by name.

**Why.** Headless peers (`bitriot_headless` in 07's multi-process test, three or more of them under ctest) and dedicated servers (Odamex) otherwise burn a core each with 1 ms sleeps or add latency with longer ones. Multiplexing with a game's own sockets is not needed: games that need that already have their own loop. **Files.** `vnet.h`, `vnet_internal.h` (optional `wait` hook), `udp.c`, `relay_udp.c`. **Cost.** Small.

### R9. DNS without surprises (low, before 12; not breaking)

**What.** Keep `vnet_create()` accepting a relay host name (removing it would break `relay_peer.c` and the documented exception), and add `int vnet_resolve_host(const char *host_port, int family, vnet_addr *out)`: the same blocking lookup, callable from a game's own thread or before the game loop. Document the recommended pattern: resolve once at settings-load time or on a worker thread, pass a numeric address to `vnet_create`.

**Why.** The relay URL in 12 comes from settings; a resolver timeout (seconds) inside `vnet_create` freezes the lobby at the click. The relay-UDP path already has the code (`relay_udp.c:resolve_relay`); this only exposes it. **Cost.** Small.

### R10. Packaging for non-CMake consumers (low, later)

- Install a `vnet.pc` (`Libs: -L${libdir} -lvnet`, plus `-lws2_32` on Windows); `cmake/` already has the install plumbing.
- Document vendoring: "add `src/*.c` to your build with `-Iinclude -Isrc`, C11, `_POSIX_C_SOURCE=200809L` on POSIX; under Emscripten add `-lwebsocket.js`; the relay-WS/WT files compile to `ENOTSUP` stubs natively." jfSW's `GNUmakefile` consumer needs exactly that.
- Optionally a generated single-file `vnet_amalgamated.c` in releases.

**Cost.** Small.

### R11. Relay: creator-only room control and an optional secret (low, later)

**What.** New client frames `CLOSE_ROOM(token)` and `KICK(token, vaddr)`, accepted only from `10.0.0.1`, and an optional room secret: `CREATE_ROOM` with a flag asking the relay to generate a second 6-symbol secret returned in `ROOM`'s trailing bytes, and `JOIN_ROOM` carrying it. All additive within protocol v1 (clients ignore unknown frame types; `ROOM` may grow at the end), but `CREATE_ROOM`/`JOIN_ROOM` have no options field today, so a secret needs new frame types (`CREATE_ROOM_EXT`/`JOIN_ROOM_EXT`) or a documented use of the padding bytes. Decide which before any other extension uses the padding.

**Why.** Today the host's only way to reject a joiner is at the game level, and the rejected member stays in the room and may keep sending datagrams to the host for 30 minutes of room idle time. Codes are 30 bits and not secret (documented); a lobby that wants "private match" has no way to express it. Neither blocks 07/08/12: the game can ignore unwanted members and the per-client rate limit bounds the nuisance. **Cost.** Medium (hub, three transports, C client, protocol doc, tests).

### R12. Impairment realism (low, later)

Correlated loss (a two-state Gilbert–Elliott model: `burst_loss_pct`, `burst_len_ms` *[as built in brief 14: `burst_pct` and `burst_len`, the mean length in packets rather than milliseconds, so that a run is exactly reproducible whatever the send rate; keys `burst=` and `blen=`]*) and a bandwidth cap with a queue. Lockstep input delay is chosen against *bursts*, not uniform loss; 07's tests at 5%/20% uniform loss will pass with a D that a 300 ms blackout would stall. Not needed to start 07, but the first thing a play-tester will ask for. **Files.** `impair.c`, `vnet_impair` (new fields at the end), the `VNET_IMPAIR` parser. **Cost.** Small–medium.

### R13. Small robustness items (low, later)

- **`SO_RCVBUF`.** The UDP backend takes the kernel default (~270 small datagrams, as brief 10 measured). One frame hitch at Odamex-server rates overflows it. Set it to 256 KiB at open (best effort), or add `so_rcvbuf` to `vnet_config`. `udp.c:make_socket`.
- **WebSocket inbox.** `relay_ws.c` keeps control and data frames in one 256-slot ring; a datagram burst in an unpolled page can drop a `KEEPALIVE` answer or an `ERROR(CLOSED)`. `relay_wt.c` already separates them. Match it for consistency (tiny).
- **`vnet_status_info` growth** is fine for a static library; if a shared build is ever offered, add `size_t size` first. Not now.

### R14. Groundwork for the relay UDP gateway and Odamex (low, design note now)

The gateway ("browser clients join native servers", allowlisted) is not briefed, but three facts about today's design will shape it and are worth recording before the protocol gets more consumers:

- **Payload size.** `VNET_MAX_DATAGRAM` = 1100 is ABI and stays (settled). Odamex's `i_net.h` sizes its buffers at 8 KB (`MAX_UDP_PACKET 8192`) and treats 1200 bytes as the "maximum safe size" (`MAX_UDP_SIZE 1200`), so most of its traffic fits but its own safe bound is 100 bytes above vnet's and large server updates do not fit at all. Odamex on vnet therefore needs its packetiser taught 1100, plus fragmentation *above* vnet for anything larger, either in Odamex's `i_net` or as an optional helper library beside vnet (not in the core: D7 is about reliability, and fragmentation of unreliable datagrams is a different, smaller thing, but it still does not belong in the datagram API). jfbuild's `mmulti.c` has `MAXPAKSIZ 256` and fits with room to spare.
- **Rate limits are per member.** A server-style member (one Odamex server with 16 clients at 35 Hz) sends ~560 packets/s, at the edge of the 600/s per-client limit (burst 1200). A gateway brief needs per-room or per-role limits, or a "server" member flag in the handshake.
- **Address space.** `DATA` carries a 4-byte `vaddr`, only `10.0.0.N` (N ≤ `max_members`) is valid, and `CREATE_ROOM` has no options. A gateway target would naturally be a pseudo-member (`10.0.0.254`, say) configured at room creation from an allowlist, which needs a `CREATE_ROOM_EXT` (see R11). Reserve `.254`/`.255` now in `relay-protocol.md` so no game treats them as players.

### R15. Loopback relay emulation (low, later)

A loopback mode that behaves like a relay room (addresses `10.0.0.N`, creator `.1`, `CONNECTING` for a configurable number of polls, `ENOTCONN` before READY, `room-host` resolution, `VNET_ENOROOM`/`EROOMFULL` on bad joins) would let a game unit-test its room flow in one process without Go. Today the only in-process relay tests are vnet's own, through the internal `vnet__create_with_relay_transport` seam. Bit Riot 12 tests the real relay under Playwright anyway, so this is a convenience, not a gap. **Cost.** Medium.

## 3. Transport-agnostic semantics (assessment)

- **Loss, ordering, duplication, MTU** are stated once and hold on every backend: unreliable, unordered, at most 1100 bytes, oversize rejected never truncated, `EMSGSIZE` on a too-small receive buffer consumes the datagram. Good. WebSocket's accidental reliability is documented as not to be relied on, and brief 11's loss measurements show the WT path really is lossy, so Bit Riot's repeat-everything protocol (D7) gets exercised in the browser too.
- **Addresses.** Virtual `10.0.0.N` for relay rooms, `127.1.x.y` for loopback, real IPs for UDP, all in one `vnet_addr`. The one semantic difference a game must know: relay addresses can be **reissued** to a later joiner, so game packets need a session id (documented; 13 already plans a token). Fine.
- **Status.** Only relay backends have a setup phase, but `vnet_status()` is generic, so a driver written against it works on every backend. Good.
- **Impairment** applies to what a context sends, not to control frames, and never on the relay. That is the right layer (the relay stays a dumb router), with the one gap in R2/R12.

## 4. Relay protocol (assessment)

- **Sessions/rooms.** Simple and sufficient: creator `.1`, lowest free address, 6-symbol codes without look-alikes, 30-minute idle, no host migration (matches D2). Rooms die when empty, so a relay restart ends every match (clients see `VNET_ECLOSED`); 13 cannot rejoin a room that no longer exists. Acceptable for casual play; say so in 13's notes.
- **Auth/tokens.** 128-bit tokens issued only to a cookie-proven address, nonces as idempotency keys, no credentials over WS/WT (the connection is the identity). Clear-text over UDP, documented. Rooms have no secret (R11, later).
- **NAT.** Path validation on rebinding, keepalives at 5 s, QUIC path validation for WT. The native **direct** UDP path (07's `--host/--join`) has no hole punching or STUN and needs a forwarded port across the internet; the relay is the answer and the briefs already say so. Worth one sentence in R6.
- **Abuse limits.** Stateless retry, no amplification, per-source and global budgets, per-address caps keyed by /64 and /48, bounded queues, panics recovered and counted, `/stats` off by default. Thoroughly tested (mutation lists in the briefs). The documented residual (a ≥26 Mbit/s forged flood from ~1250 sources stops new handshakes) is the correct thing to leave to the network.
- **Versioning.** One version byte, `BAD_VERSION` carries the relay's version, unknown frame types ignored, `ROOM` may grow. Adequate; fix the "unreleased" wording (R7) and decide the extension mechanism for `CREATE_ROOM`/`JOIN_ROOM` once (R11/R14).

## 5. Testability and observability (assessment)

- **Loopback with virtual time** is the strongest feature for 07: N peers, one thread, exact repeatability, `vnet_loopback_network_step()` releasing due datagrams in a defined order. `vnet_time()` lets `LockstepSession`'s RTT and timeout code run unchanged on virtual time. Keep.
- **Impairment** is deterministic per seed with a fixed draw order (changing one knob does not reshuffle the others), and `VNET_IMPAIR` degrades any build. The default-seed-0 trap is documented. Missing: runtime change (R2) and bursts (R12).
- **Stats** cover sends, wire sends, drops by reason, overflow, oversize, unroutable. Missing: RTT (R5); nothing else a game needs.
- **Relay** has `/stats` and `/healthz`; the browser tests read `/stats` to assert no malformed frames or leaks. Good.

## 6. Notes for the Bit Riot briefs (no vnet change needed)

**07 (native lockstep).**
- Poll and send keepalives in every state, including menus: the relay drops a silent UDP member after 15 s, and 07's own peer timeout is ~10 s.
- Treat `VNET_EAGAIN`/`VNET_EIO` from `vnet_sendto` as a dropped packet (the repeat-everything protocol covers it), never as an error.
- Give each process its own `VNET_IMPAIR` `seed=`; the headless multi-process test should set it per process.
- `vnet_resolve` is numeric only; `--join host:port` with a name needs the game's own lookup until R9.
- Over direct UDP across the internet one side needs a forwarded port; the relay (12) removes that.
- Pin `v0.3.0`; the API is additive from here (R7).

**12 (browser netplay).**
- R1 is the item to land first; otherwise the hidden-tab tests fail for a reason unrelated to the game.
- `RELAY_AUTO` adds up to `wt_timeout_ms` (3 s) to "connecting" where QUIC is blocked; show the transport from `vnet_status().transport`.
- Everyone behind one NAT shares the per-address caps (128 members, 128 WebSockets, 32 pending, the same again for WebTransport, 20 handshakes/s). Fine for a LAN party; a classroom should raise the flags.
- A relay restart ends the match; a dev certificate's hash changes on every relay start.

**13 (disconnects and hot-join).**
- A reconnecting peer is a new `vnet` context with a new nonce and usually a new `10.0.0.N`; the old session lingers up to 15 s (a UDP `LEAVE` frees it at once). Identify peers by the game's token, not by address, as the brief already says.
- Record transfer in 1100-byte chunks is fine over every transport; it is the game's own flow control, by design (D7).
- Time-slice the fast-forward (the brief does) so `vnet_poll` keeps running; R1 also protects a native peer that pauses longer than 15 s.

## 7. Settled decisions: concerns, none requiring reversal

- **D7 / 1100 bytes.** Right for lockstep and for the browser transports. It makes Odamex a fragmentation-above-vnet job (R14); that is a consequence to plan for, not a reason to change the constant.
- **No threads (06).** Right for games. R8's `vnet_wait()` keeps it: blocking only in a call that says so.
- **D9.** WebTransport-with-fallback is implemented as designed; the probe-instead-of-trusting-`maxDatagramSize` decision in brief 11 was the correct deviation.
