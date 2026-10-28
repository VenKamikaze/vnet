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
- Tokens are 16 bytes; frames from the relay carry none, so the client trusts only the relay's source address (documented limits in the protocol document). A repeated request with the same nonce from a different source address is the same session (this is also how a NAT rebinding during the handshake works); the nonce of an ended session is remembered 60 s so that a late duplicate cannot resurrect a room.
- WebSocket liveness uses the relay's pings (browsers answer them even when the page's timers are throttled) as well as frames. Silent UDP members are dropped after 15 s (`-session-timeout`); the client treats three silent keepalive intervals (15 s) as a lost session. Note for Bit Riot brief 13: this is 15 s, not the "~5 s" assumed there.
- The relay answers `KEEPALIVE` with a one-byte `KEEPALIVE`, so a client can tell a dead relay from a quiet room.
- A browser queues incoming frames in a C ring (via the Emscripten callbacks), not in JS, and no `--js-library` was needed.
- The room is not closed when the host leaves (there is no host migration); a room also dies after 30 min without `DATA`.

**Tests**

- Go: 55 tests (+ `FuzzHandleUDP`, run for 25 s with no finding). Most drive the hub synchronously with a recording hook and a fake clock (no sleeping, "no reply" is an assertion, not a wait); the rest use real UDP and WebSocket sockets (interop of both transports, handshake timeout, pongs, origin check, caps, 12 rooms of mixed transports under the race detector). Clean under `-race`, 20 repeats, also under CPU load.
- C (GoogleTest): `RelayClient` 40 tests against a scripted fake relay on a UDP socket with an injected clock; `RelayWsStyle` 10 tests drive the same core through a WebSocket-style transport (no token, unpadded, unretried handshake, async connect); `RelayIntegration` 9 tests against the real relay as a child process (two native clients, content/order/sender addresses, impairment in front incl. exact loss accounting and latency, room isolation, unknown/full rooms, address reuse after LEAVE, lost replies cause retries but never a second room or member, NAT rebinding through a forwarder). `ctest`: 168 entries (166 GoogleTest + smoke + Go), all pass, 1 skip (the IPv6 test of brief 06). Same result under gcc, clang `-Werror` and ASan+UBSan; 20 repeats of the relay tests under CPU load pass.
- Browser: `tests/browser/relay_browser.test.js`, Playwright 1.63 / Chromium: native host with browser guest, browser host with native guest, two browsers, 150 datagrams each way, content and order asserted; unknown room and unreachable relay show up as FAILED statuses in the page; the relay's `/stats` shows no malformed frames, no rate limiting and no leftover sessions. About 3 s.
- Windows: `relay_peer.exe` (MinGW-w64, Winsock) under Wine 9 against the Go relay, as guest and as host with a native Linux peer; the brief-06 smoke test still passes; i686 builds.
- Mutation checks (broken, run, restored, in scratch copies): 46 Go and 46 C mutations (routing across rooms, broadcast, source spoofing, identity by address, NAT rebinding, nonce idempotency, tombstones, padding rule, reply-size guard, reply and handshake rate limits, room/member/session caps, per-client rates, payload, WebSocket caps/queue/pongs/origin, timeouts, alphabet, host address, retry nonce, retransmission, timeouts, keepalives, source-address check, error mapping, ENOTCONN before the room, framing differences between UDP and WebSocket style ...). All were killed. The first round left one survivor (a redundant port check in the relay address parser); a test was added and it now dies. One mutation (receive buffer size check removed) is killed by a crash, not an assertion.

**Load sanity** (one relay process, 50 rooms x 4 clients x 30 pps, 100-byte payloads, 30 s, load generator on the same 4-core machine; `scripts/loadtest.sh`):

| Clients | Packets into relay | Lost | Latency p50 / p99 | Relay CPU | Relay RSS |
|---|---|---|---|---|---|
| all UDP | 5986/s | 0 | 133 us / 405 us | 10.4% of one core | 10.2 MB |
| all WebSocket | 5986/s | 0 | 195 us / 602 us | 27.5% | 20.2 MB |
| half UDP, half WebSocket | 5986/s | 0 | 173 us / 508 us | 24.4% | 15.4 MB |
| all UDP, each tick to 3 peers | 17960/s | 0 | 154 us / 944 us | 15.2% | 11.2 MB |

**Not verified / follow-ups**

- **Docker:** the container image was not built: the sandbox has no Docker daemon (`/var/run/docker.sock` is missing; starting `dockerd` here was refused by the permission system, so it was not tried). The Dockerfile's build command was run directly and gives the same static binary; CI builds and runs the image.
- The CI workflow (new jobs: `relay`, `browser`, `mingw` relay step; Go pinned to 1.24.7 in every job) could not be run here; each step's commands were run locally. `npx playwright install` is only in CI. macOS is unverified, as in 06.
- Browser: tested in Chromium only, over plain `ws://` on loopback. `wss://` behind a proxy is documented, not tested. Background-tab behaviour is brief 12's.
- The relay has no peer join/leave notification, no broadcast address, no host migration and no rejoin: games announce membership themselves (Bit Riot 12/13).
- For **brief 11**: add the transport as another `relay_transport_ops` (control frames on a stream, `DATA` as datagrams; `ops->send` sees the frame type) and set `vnet_status_info.transport` to its name; `RELAY_AUTO` can wrap two transports behind the same core. On the server `session.ws` is a concrete `*wsConn`; make it an interface when adding a third transport. `ERROR`/`ROOM` frames already fit in one datagram.
- For **Bit Riot 12**: show `status.room`; address the host with `vnet_resolve(v, "room-host:PORT")`; poll `vnet_poll()` every frame and watch `vnet_status()` for `FAILED` (`VNET_ECLOSED` after a relay restart or a 15 s UDP silence; WebSocket sessions survive a hidden tab because browsers answer pings); put a session id in game packets (an address can be reissued to a later joiner); room codes are case-insensitive; the default cap is 8 players; the per-source handshake limit (20/s, burst 50) is shared by everyone behind one NAT (`-handshake-rate`); a browser drops datagrams when its WebSocket has more than 64 KB queued.
- Kernel socket buffers hold only about 270 small datagrams (a `recvfrom` backlog beyond that is lost, as with any UDP): the tests pace their sends per frame for this reason.
