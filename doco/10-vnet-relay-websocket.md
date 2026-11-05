# 10 — vnet relay and WebSocket transport

- **Repo:** `VenKamikaze/vnet`
- **Branch:** `vnet/10-relay-ws`, from `claude`
- **Depends on:** 06
- **Status:** done

## Goal

Build a relay server, plus two client backends:

- a **browser** backend: WebSocket, written in JS for Emscripten
- a **native relay** backend: UDP to the relay

Together these let browser and native players share a "room" and exchange vnet datagrams as if they were on one virtual LAN. A game sees only the existing vnet API (06). It gets a virtual address and talks to other members' virtual addresses.

## Background (from the original design discussion)

- **Browsers have no raw UDP.** WebSockets are reliable and ordered (TCP), work everywhere, and are trivial to host. Under loss they stall (head-of-line blocking).
  - That is fine for lockstep games, which need every packet anyway. Cloudflare's Chocolate Doom used exactly this, with a message router.
  - Unreliable WebTransport datagrams come in brief 11.
- **Virtual addressing:** the relay assigns each member a virtual IPv4-style address in the room, for example `10.0.0.N`. Some games embed peer addresses in their own packets, so addresses must stay stable for a member's lifetime in the room.
- **Don't build an open proxy.** The relay only routes between members of the same room. A UDP *gateway* to arbitrary internet hosts is explicitly out of scope, and if it is ever added, it must use an allowlist.

## Design

- **Relay:** Go (D4). One static binary.
  - Listens on:
    - a WebSocket endpoint (`/vnet`), with binary frames, served over plain HTTP for local development and `wss://` in production behind TLS;
    - a UDP port for native clients.
  - Configured by flags and env.
  - Has a `Dockerfile`.
- **Framing:** the same message set over WebSocket frames and UDP datagrams, with these UDP differences:
  - **Every UDP frame after `ROOM` carries the session token.**
  - Control messages can be lost over UDP. `CREATE_ROOM` and `JOIN_ROOM` are therefore retried by the client and must be **idempotent**, keyed by a client nonce, so a retry never creates a second room.
  - **Anti-reflection:** never answer an unauthenticated UDP packet with a larger packet, and rate-limit replies to unknown sources. Because `ROOM` is larger than a bare `CREATE_ROOM`, a UDP `CREATE_ROOM`/`JOIN_ROOM` is zero-padded to at least 64 bytes and shorter ones are dropped silently (added to the brief during implementation; see the protocol document).
  - Each frame is a 1-byte message type, then the fields.
  - Control messages: `CREATE_ROOM` → `ROOM(code, your_vaddr)`, `JOIN_ROOM(code)` → `ROOM(...)` | `ERROR(reason)`, `LEAVE`, `KEEPALIVE`.
  - Data: `DATA(dst_vaddr, dst_port, src_port, payload)`. The relay fills in the source vaddr when forwarding.
  - Document the format in `doco/relay-protocol.md`, and version it in the first message.
- **Rooms**
  - Codes are short, human-typeable and unambiguous, for example 6 characters with no 0/O/1/I.
  - The creator gets `.1`, so a game can treat "host = `10.0.0.1`" as a convention. Document this; Bit Riot's brief 12 relies on it.
  - Configurable member cap (default 8).
  - A room dies when empty, or after an idle timeout.
- **Native relay identity:** native clients over UDP are identified by a session token issued in `ROOM`, not by their source IP:port, so a NAT rebinding doesn't break them. Keepalive interval: about 5 s.
- **Limits:** max payload `VNET_MAX_DATAGRAM`; per-client packet-rate and byte-rate limits; a room cap; a cap on total rooms.
- **Client backends**
  - **`VNET_BACKEND_RELAY_WS`** (browser): use Emscripten's `<emscripten/websocket.h>` API (`-lwebsocket.js`) if it fits. Otherwise write a small `--js-library` (`vnet_ws.js`) plus C glue.
    - The vnet repo needs its own emsdk setup script for this (pinned version). See the network notes in [`README.md`](README.md) on GitHub archive downloads. This backend needs no Emscripten ports.
    - It queues incoming frames (the Emscripten callbacks push them into a C ring; no JS library was needed).
    - `vnet_poll()` moves them into the sockets' receive queues, and `vnet_recvfrom` reads those, so it never blocks.
    - Connection and room setup are asynchronous, which shows as a status the game can poll: `vnet_status()` returns CONNECTING, READY or FAILED (the brief's IN_ROOM and ERROR, renamed because the call is generic and every backend answers it; FAILED comes with the reason). Add that to the API carefully, and keep it generic.
  - **`VNET_BACKEND_RELAY_UDP`** (native): the same framing over a UDP socket to the relay. It lets native and browser players share rooms.
  - **Room selection:** `vnet_config` carries the relay URL or address and either "create room" or "join code". `vnet_resolve("room-host")`, or similar, returns the host vaddr.

## In scope

- The relay, both client backends, and the docs.
- A Python or Node CLI test client is fine for debugging.
- A `docker run` example and a short deployment note covering TLS termination (Caddy or nginx in front, `wss://`).

## Out of scope

- WebTransport (11).
- Authentication and accounts.
- Matchmaking and room listing. A room listing could come later.
- The UDP gateway.

## Tests

- **Go unit tests:** room lifecycle, address assignment, routing only within a room, limits, malformed frames rejected, and timeouts.
- **Native integration:** two native C clients on `RELAY_UDP` exchange datagrams through a locally running relay, with an impairment layer optionally in front.
- **Browser integration** with Playwright (Chromium is pre-installed):
  - a test page built with Emscripten, from a vnet example program, joins a room created by a native client;
  - they exchange N datagrams both ways;
  - assert content and order.
- **Load sanity:** one relay process with 50 rooms × 4 clients at 30 packets per second each. Report CPU and memory.

## Acceptance criteria

- One relay binary serves both WebSocket and UDP clients.
- Browser ↔ native exchange works through it.
- Limits are enforced and tested.
- The protocol is documented.
- No path forwards traffic to non-members.

## Outcome

Landed on `vnet/10-relay-ws`. Everything in scope is implemented and tested; nothing was cut. Protocol version **1**, documented in [`relay-protocol.md`](relay-protocol.md); operating notes (flags, Docker, Caddy/nginx TLS) in [`../relay/README.md`](../relay/README.md).

**What is there**

- `relay/` (Go 1.24, one dependency: `gorilla/websocket`): `vnet-relay`, a static binary serving UDP and WebSocket (`/vnet`) in shared rooms, plus `/healthz` and `/stats`. The hub is transport-agnostic (`hub.go`); `server.go` is the single-reader UDP loop, `ws.go` the WebSocket handler. Flags and `VNET_RELAY_*` environment variables. `relay/Dockerfile` (scratch image, ~6 MB binary). `relay/cmd/vnet-loadtest` is the load tool.
- C library: `VNET_BACKEND_RELAY_UDP` and `VNET_BACKEND_RELAY_WS`. One protocol core (`src/relay_client.c`: handshake, retries, keepalives, silence detection, socket table, receive queues) over a small transport interface (`relay_transport_ops`: state / send frame / receive frame). `relay_udp.c` runs the frames over an inner private native UDP context; `relay_ws.c` uses Emscripten's `<emscripten/websocket.h>` (`-lwebsocket.js`, no ports). The browser callbacks queue frames in a C ring and `vnet_poll()` moves them into the sockets' queues. `VNET_BACKEND_UDP` returns `VNET_ENOTSUP` under Emscripten, `RELAY_WS` returns it natively.
- `examples/relay_peer.c`: one program for both worlds, native (UDP) and browser (WebSocket page); host and guest exchange N numbered, varied-length datagrams each way and check content and order.
- `scripts/setup-emsdk.sh` (emsdk pinned to git revision `d7b41306` and **Emscripten 6.0.13**, installed with `git fetch`, no archive download), `scripts/test-browser.sh`, `scripts/test-mingw-relay.sh`, `scripts/loadtest.sh`; CI jobs for the Go relay (vet, race tests, fuzz seconds, Docker build and run), the browser test, and relay under Wine.

**API additions** (all additive; version 0.2.0)

- `vnet_status(v, &info)` returns `VNET_STATUS_CONNECTING | READY | FAILED` and fills `vnet_status_info { state, error, transport, room[8], self }`. Generic: every backend answers (UDP and loopback are always READY). Brief 11 reuses it unchanged and only adds a transport name. The brief's IN_ROOM and ERROR became READY and FAILED, FAILED carrying a `VNET_E*` reason. FAILED is final.
- `vnet_config`: `relay`, `room` (NULL = create, else the code), `relay_timeout_ms` (10 s), `keepalive_ms` (the relay suggests 5 s); `rx_queue_len` also applies to the relay backends.
- `vnet_resolve(v, "room-host:PORT")` = `10.0.0.1:PORT` (works before the room is entered). Relay addresses are plain IPv4 `10.0.0.N:port`, so `vnet_addr` is unchanged.
- Error codes `VNET_ETIMEDOUT, ENOTCONN, ENOROOM, EROOMFULL, ELIMIT, EPROTO, ECLOSED, ECONNREFUSED` (-12..-19). Before the room is entered `vnet_sendto()` and `vnet_local_addr()` return `VNET_ENOTCONN` (nothing is queued, also with impairment on); `vnet_open()` works at once.
- Behaviour: impairment applies to the datagrams the game sends, not to the handshake or keepalives. The relay backends do their I/O in `vnet_poll()` only.
- One documented exception to "nothing blocks": `RELAY_UDP` resolves a relay host *name* with `getaddrinfo()` inside `vnet_create()`. Numeric addresses never block.
- Internal: `vnet_backend` gained optional `status` and `resolve` hooks; the inner UDP socket of the relay carries frames of up to 1125 bytes through an internal `max_dgram` (the public limit stays `VNET_MAX_DATAGRAM`); `vnet__create_with_relay_transport()` is a test seam.

**Protocol decisions and deviations from the brief**

- UDP `CREATE_ROOM`/`JOIN_ROOM` are zero-padded to 64 bytes and shorter ones are dropped silently, so that no reply (at most 38 bytes) is ever larger than the unauthenticated packet that drew it. The brief required the property but not the mechanism. The creator is always `10.0.0.1`; joiners get the lowest free address up to `10.0.0.<max_members>`; `.1` is never reissued, even after the creator has left.
- Tokens are 16 bytes; frames from the relay carry none, so the client trusts only the relay's source address (documented limits in the protocol document). A repeat of a request (same nonce) is answered with the same ROOM only to the address the session was created for; the nonce of an ended session is remembered 60 s so that a late duplicate cannot resurrect a room (see the second fix round for what changed with the stateless retry).
- WebSocket liveness uses the relay's pings (browsers answer them even when the page's timers are throttled) as well as frames. Silent UDP members are dropped after 15 s (`-session-timeout`); the client treats three silent keepalive intervals (15 s) as a lost session. Note for Bit Riot brief 13: this is 15 s, not the "~5 s" assumed there.
- The relay answers `KEEPALIVE` with a one-byte `KEEPALIVE`, so a client can tell a dead relay from a quiet room.
- A browser queues incoming frames in a C ring (via the Emscripten callbacks), not in JS, and no `--js-library` was needed.
- The room is not closed when the host leaves (there is no host migration); a room also dies after 30 min without `DATA`.

**Tests**

- Go: 80 tests (+ `FuzzHandleUDP` and `FuzzHandleWS`, run for 20-25 s each with no finding). Most drive the hub synchronously with a recording hook and a fake clock (no sleeping, "no reply" is an assertion, not a wait); the rest use real UDP and WebSocket sockets (interop of both transports, handshake timeout, pongs, origin check, caps, 12 rooms of mixed transports under the race detector). Clean under `-race`, 20 repeats, also under CPU load.
- C (GoogleTest): `RelayClient` 51 tests against a scripted fake relay on a UDP socket with an injected clock; `RelayWsStyle` 11 tests drive the same core through a WebSocket-style transport (no token, unpadded, unretried handshake, async connect); `RelayIntegration` 10 tests against the real relay as a child process (two native clients, content/order/sender addresses, impairment in front incl. exact loss accounting and latency, room isolation, unknown/full rooms, address reuse after LEAVE, lost replies cause retries but never a second room or member, NAT rebinding through a forwarder). `ctest`: 181 entries (179 GoogleTest + smoke + Go), all pass, 1 skip (the IPv6 test of brief 06). Same result under gcc, clang `-Werror` and ASan+UBSan; 20 repeats of the relay tests under CPU load pass.
- Browser: `tests/browser/relay_browser.test.js`, Playwright 1.63 / Chromium: native host with browser guest, browser host with native guest, two browsers, 150 datagrams each way, content and order asserted; unknown room and unreachable relay show up as FAILED statuses in the page; the relay's `/stats` shows no malformed frames, no rate limiting and no leftover sessions. About 3 s.
- Windows: `relay_peer.exe` (MinGW-w64, Winsock) under Wine 9 against the Go relay, as guest and as host with a native Linux peer; the brief-06 smoke test still passes; i686 builds.
- Mutation checks (broken, run, restored, in scratch copies): 46 Go and 46 C mutations (routing across rooms, broadcast, source spoofing, identity by address, NAT rebinding, nonce idempotency, tombstones, padding rule, reply-size guard, reply and handshake rate limits, room/member/session caps, per-client rates, payload, WebSocket caps/queue/pongs/origin, timeouts, alphabet, host address, retry nonce, retransmission, timeouts, keepalives, source-address check, error mapping, ENOTCONN before the room, framing differences between UDP and WebSocket style ...). All were killed. The first round left one survivor (a redundant port check in the relay address parser); a test was added and it now dies. One mutation (receive buffer size check removed) is killed by a crash, not an assertion.

**Load sanity** (one relay process, 50 rooms x 4 clients x 30 pps, 100-byte payloads, 30 s, load generator on the same 4-core machine; `scripts/loadtest.sh`; re-measured after the second fix round, with the per-address limits lifted because all 200 clients come from one address):

| Clients | Packets into relay | Lost | Latency p50 / p99 | Relay CPU | Relay RSS |
|---|---|---|---|---|---|
| all UDP | 5987/s | 0 | 129 us / 443 us | 10.4% of one core | 6.8 MB |
| all WebSocket | 5986/s | 0 | 195 us / 680 us | 28.4% | 21.2 MB |
| half UDP, half WebSocket | 5943/s | 0 | 178 us / 1.05 ms | 24.8% | 15.5 MB |
| all UDP, each tick to 3 peers | 17959/s | 0 | 145 us / 572 us | 14.6% | 6.8 MB |

**Review fix round** (after the review of `1f2ebe1`)

- **B1, relay crash:** any unauthenticated WebSocket client could kill the process: `reject()` wrote from the read goroutine while the writer goroutine was mid-write, and gorilla/websocket panics on concurrent writes. All writes now go through one mutex-guarded `write()`; connection close is a `defer`; panics in connection, UDP and sweep goroutines are recovered and counted (`panics` in `/stats`). Regression test `TestWebSocketBadFrameWhileTheWriterIsBusyDoesNotCrashTheRelay` holds the writer inside a write (a gated connection, the client "stopped reading") and sends the bad frame; it crashed the test binary with "concurrent write to websocket connection" before the fix. There is also `FuzzHandleWS`.
- **S1, source laundering:** a token no longer moves a session to whatever source it arrives from. A frame from another source earns a `PATH_CHALLENGE(cookie)` (9 bytes, smaller than its trigger, 1 per session per second, subject to the per-address reply limits) and the session moves only on a `PATH_RESPONSE` from that source echoing the cookie (single use, 10 s). New frame types 0x05/0x06 in protocol version 1 (unreleased); the C client answers challenges. (The first version of this fix also kept new UDP sessions "unvalidated" until a keepalive arrived; the second round replaced that by the stateless retry below.)
- **S2, capacity:** per source address (IPv6 keyed by /64): members, live rooms (counted until the room dies, so a creator cannot hold rooms open by leaving), open WebSockets, not-yet-authenticated WebSockets; send queues are bounded in bytes (64 KiB) as well as frames; all are flags (`relay/README.md`). Rotating the host part of an IPv6 address no longer buys a fresh budget.
- **S3, nonce:** `src/random.c` uses `getentropy()` (Linux, macOS, Emscripten without a file system) or `rand_s` (Windows/MinGW), with the CSPRNG device file only as a fallback when `getentropy` is missing from the kernel; there is no time-based fallback any more: with no source, `vnet_create` fails with `VNET_EIO`. The protocol document explains why the nonce still has to be unpredictable.
- **S4, sweeper stall:** `wsConn.close()` no longer waits for the close frame; it is sent from a goroutine with a 200 ms deadline. `TestSweepDoesNotStallBehindClientsThatStopReading` (six clients whose writes block) took 5.0 s to sweep with the old code and takes milliseconds now.
- **Nits:** `X-Forwarded-For`: rightmost entry; `/stats` is off by default and has its own listener (`-stats`); `keepalive_ms` must be 100..5000 (relay config: keepalive at most a third of the session timeout) and the relay's suggestion is clamped to 5 s; `vnet.h` notes Emscripten's per-message allocations and that `vnet_status_info` only grows at the end; the slow-client test fails by assertion (2 s deadline) and a byte-bound test was added.
- **Proof, fix round:** the Go mutation list grew to 66 (B1 unserialised writes, followed-source rebinding, unchecked/reusable/misaddressed/expired PATH_RESPONSE, unlimited challenges, delivery to unvalidated members, full-address IPv6 keys, each per-address cap, pending-slot and counter leaks, unbounded queue bytes, blocking close, leftmost `X-Forwarded-For`, public `/stats`, unchecked keepalive/timeout relation) and all 66 are killed; 14 C mutations of the new client logic (immediate and repeated keepalive, confirmation, challenge answers, keepalive validation, clamping, random source failure, weak fallback) are killed too (the weak-fallback one needed a `getentropy` failure seam). One Go mutation (a PATH_RESPONSE that stays valid after use) was equivalent until a counter assertion was added.

**Second review fix round**

- **Handshake replay bypass (reproduced: 100 of 100 datagrams reached the victim):** forge `JOIN_ROOM` nonce N "from" the victim, repeat nonce N from the attacker's own address to receive the ROOM and token, forge a `KEEPALIVE` with that token from the victim to "validate" the session, then have another member send data to it. Fixed by the stateless retry below plus a rule that the ROOM (and with it the token) goes only to the address the session was created for: a repeat of a nonce from any other address gets `ERROR(NONCE_REUSED)`, whatever its cookie. `TestForgedHandshakeCannotAimASessionAtAVictim` replays the sequence (0 datagrams reach the victim; no migration), `TestHandshakeRepeatFromAnotherAddressLearnsNothing` covers the repeat.
- **Forged handshakes sustained lockouts (2 forged CREATEs/s kept a victim IP from creating rooms for 2 minutes; 70 forged sources refused every innocent CREATE):** UDP now has a QUIC-Retry style **stateless retry**. `CREATE_ROOM`/`JOIN_ROOM` carry a 16-byte cookie field (zero at first); a request without a valid cookie is answered with `RETRY(nonce, cookie)` (26 bytes; the cookie is an HMAC over source address and port, nonce and a 16 s time bucket, under a random per-start secret) and forgotten. Only the repeat that echoes the cookie creates a session, so a session's address is proved from birth: the "unvalidated" state, its timer and the client's fast keepalives are gone. The 64-byte padding rule stays; WebSocket needs none of this (TCP proved the address). `TestForgedHandshakesCreateNoStateAndUseNoBudget` sends 8 forged requests per second for 120 simulated seconds (no state, no counters, no budget used; the victim address still creates its room afterwards, and `MaxRooms` is reachable only by real clients); `TestHandshakeCookieIsBoundToSourceNonceAndTime` covers address, port, nonce, bit flips, one-bucket-old and expired cookies and another relay's secret; the C client has tests for the retry exchange and an integration test that loses RETRYs (the relay holds no state until the third) and ROOMs (idempotent repeats), and the NAT-rebinding test goes through the challenge.
- **IPv6 /56 against `MaxRooms`:** an IPv6 /48 is now also a counting unit: the sum of all its /64s may hold `ipv6-prefix-factor` (default 4) times the per-address caps (`TestIPv6PrefixCapsBoundWhatOneSubscriberCanHold`: 40 /64s of one /48 get 6 rooms with factor 3 and cap 2). A /56 or /64 allocation sits inside one /48 of the same provider block.
- **Locks (nit):** every lock that can be held while a panic is recovered is taken with `defer` (`leave`, the sweeper, `forward`, the WebSocket cleanup and counters). A test hook injects panics under the read and write locks and checks the hub is still lockable (`TestARecoveredPanicNeverLeavesTheHubLocked`; it hangs the old code).
- **WebSocket admission (nit):** the cap check and the reservation are one step (`wsReserved`), tested with 200 parallel upgrades against `MaxSessions=5`.
- **503 on connect (nit):** defaults raised (128 members, 128 open and 32 pending WebSockets per address); the browser backend retries a failed open five times over about 4 s before reporting `VNET_ECONNREFUSED`. The browser test holds the only pending slot with a raw socket, checks the relay refused the page, releases it and checks the page gets in (without the retry it never does). The caps, the carrier-grade NAT consequence and the retry are written up in `relay/README.md` for Bit Riot brief 12.
- **Test seam (nit):** `vnet__random_hook` is gone; tests replace `getentropy` at link time (`--wrap`, Linux).

- **Proof, second round:** the Go mutation list is 77 entries (cookie not checked, not bound to address, port or nonce, never expiring, previous bucket refused, a repeat from another address given the ROOM, the owner's repeat refused, forged requests drawing on the handshake budget, /48 groups missing or unscaled, unlocks without `defer`, admission on `len(conns)`, uncounted refusals, plus every earlier entry); all are killed. The C list is 60 (RETRY ignored, accepted for another nonce, in a room, of any length or version, honoured by the WebSocket-style transport, cookie not stored or not put in the repeat, no immediate repeat, padding, random source, challenges, keepalive logic); all are killed. The browser 503 scenario fails without the retry (the page never gets its ROOM). Re-run after the round: ctest 181 entries green under gcc, clang `-Werror` and ASan+UBSan; `go test -race` x5 and the relay C++ tests x6 under CPU load; both fuzz targets 20-25 s; Chromium; Wine (guest and host); MinGW x86_64 and i686; load test above.
- **For Bit Riot brief 13:** the relay's 15 s session timeout only reclaims resources; drop detection belongs to the game's own peer timeout. A reconnecting peer gets a new context, a new nonce and possibly a new virtual address, while its old session lingers up to 15 s (a UDP member that sent a `LEAVE` frees its slot at once). Also: `getaddrinfo()` inside `vnet_create()` (host names) can block for the resolver's timeout; pass a numeric address, or resolve before the game loop, where that matters.

**Third review fix round**

- **Relay-wide handshake DoS through the global reply budget:** every `RETRY`, `ERROR` and `PATH_CHALLENGE` to an unproven source drew on one 5000/s overall bucket, so a flood of 5000+ forged packets/s (about 3 Mbit/s) from spread-out addresses left real clients without a `RETRY`. Now (a) `ROOM`/`ERROR` sent to a source that has just echoed a valid cookie are not charged to the unauthenticated budgets (`replyProven`; still never larger than the request); (b) `RETRY` (26 bytes) and `PATH_CHALLENGE` (9 bytes) have their own, higher overall allowance of 50000/s (`GlobalVitalReplyPerSec`), while `ERROR`s to unproven sources keep the 5000/s one; the per-source-IP bucket (40/s) applies to both. Outbound volume stays bounded at about 55000 packets/s of at most 26 bytes (about 11 Mbit/s). What remains is documented in `relay-protocol.md` (Security properties, Anti-reflection): forging at more than 50000 packets/s from at least ~1250 source addresses (about 26 Mbit/s of payload) still stops new handshakes and NAT migrations relay-wide while it lasts; established sessions and WebSockets are unaffected, and nothing remains afterwards.
  - Tests: `TestForgedFloodAboveTheGlobalBudgetDoesNotStopRealHandshakes` (6000 forged packets per simulated second, half RETRY-drawing and half bad-version, for 4 seconds: a real client completes its handshake every second; no state from the flood), `TestRepliesToCookieProvenSourcesAreNotChargedToTheUnprovenBudgets` (ROOM and an ERROR with every budget exhausted), `TestErrorsToCookieProvenSourcesAreNotChargedToTheUnprovenBudgets` (`ERROR(RATE)` and malformed-code `ERROR`), `TestPathChallengeSurvivesAnExhaustedOrdinaryReplyBudget`.
- **Nit, cookie test:** `TestHandshakeCookieIsBoundToSourceNonceAndTime` is now a table over `RetryCookieBucket`: refused at exactly two buckets, accepted just under two, accepted one bucket after being issued at the end of a bucket, refused two buckets after that, refused at three. The mutation that accepts a third bucket is killed.
- **Nit, config:** `Config` validation fails unless `2 * RetryCookieBucket < NonceTTL` (a cookie must be dead before the nonce of a finished session is forgotten); `TestConfigRequiresCookiesToExpireBeforeNoncesAreForgotten`.
- `relay/README.md` says that `/stats` `panics` above 0 means restarting the relay soon.
- **Proof, third round:** 8 new Go mutations, all killed (third cookie bucket; RETRY, PATH_CHALLENGE or the whole vital allowance charged to the ordinary bucket; ROOM, `ERROR(RATE)` or `ERROR(ROOM_NOT_FOUND)` to a proven source charged to the unproven budgets; config check removed), 85 in all. Three first survived and the last test (`TestErrors...`, `TestPathChallenge...`) was added for them. Re-run: `go test -race` (x5), ctest 181 entries green under gcc, clang `-Werror` and ASan+UBSan (the IPv6 loopback test skips here: no IPv6 loopback), Chromium browser test.

**Not verified / follow-ups**

- **Docker:** the container image was not built: the sandbox has no Docker daemon (`/var/run/docker.sock` is missing; starting `dockerd` here was refused by the permission system, so it was not tried). The Dockerfile's build command was run directly and gives the same static binary; CI builds and runs the image.
- The CI workflow (new jobs: `relay`, `browser`, `mingw` relay step; Go pinned to 1.24.7 in every job) could not be run here; each step's commands were run locally. `npx playwright install` is only in CI. macOS is unverified, as in 06.
- Browser: tested in Chromium only, over plain `ws://` on loopback. `wss://` behind a proxy is documented, not tested. Background-tab behaviour is brief 12's.
- The relay has no peer join/leave notification, no broadcast address, no host migration and no rejoin: games announce membership themselves (Bit Riot 12/13).
- For **brief 11**: add the transport as another `relay_transport_ops` (control frames on a stream, `DATA` as datagrams; `ops->send` sees the frame type) and set `vnet_status_info.transport` to its name; `RELAY_AUTO` can wrap two transports behind the same core. On the server `session.ws` is a concrete `*wsConn`; make it an interface when adding a third transport. `ERROR`/`ROOM` frames already fit in one datagram.
- For **Bit Riot 12**: show `status.room`; address the host with `vnet_resolve(v, "room-host:PORT")`; poll `vnet_poll()` every frame and watch `vnet_status()` for `FAILED` (`VNET_ECLOSED` after a relay restart or a 15 s UDP silence; WebSocket sessions survive a hidden tab because browsers answer pings); put a session id in game packets (an address can be reissued to a later joiner); room codes are case-insensitive; the default cap is 8 players; the per-source handshake limit (20/s, burst 50) is shared by everyone behind one NAT (`-handshake-rate`); a browser drops datagrams when its WebSocket has more than 64 KB queued.
- Kernel socket buffers hold only about 270 small datagrams (a `recvfrom` backlog beyond that is lost, as with any UDP): the tests pace their sends per frame for this reason.
