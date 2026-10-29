# 11 — vnet WebTransport datagrams

- **Repo:** `VenKamikaze/vnet`
- **Branch:** `vnet/11-webtransport`, from `claude`
- **Depends on:** 10
- **Status:** done

## Goal

Give browser clients an **unreliable datagram** path to the relay using WebTransport, the closest thing browsers have to UDP. If WebTransport is unavailable or fails, fall back automatically to the WebSocket path from brief 10.

Lockstep Bit Riot works either way. This transport is mainly for client/server games such as Odamex, where retransmit stalls hurt, and for smoother lockstep play under packet loss.

## Background

The claims in the original brief were re-checked in October 2026 (sources and what could not be verified are in the Outcome). **Corrected here:**

- WebTransport reached Baseline **"newly available"** on **2026-03-24** (Safari 26.4 on macOS and iOS completed the set). It is *not* "widely available": that status comes 30 months after the newly-available date, so about September 2028. Chrome 97 (January 2022), Edge 97/98, Firefox 114 (June 2023) and Safari 26.4 (March 2026) are the first versions with the API.
- `serverCertificateHashes` arrived later than the API in two engines: **Chrome/Edge 100, Firefox 125, Safari 26.4**. The rules (W3C WebTransport, section "Server Authentication using Certificate Hashes", and MDN): an X.509v3 certificate, valid now, with a validity period **of less than two weeks** (the brief's "≤14 days" is one second too generous; the spec says "limited to two weeks" and MDN "less than 2 weeks", so vnet issues 13 days), whose key is ECDSA P-256 at minimum and **never RSA**; the only hash is SHA-256.
- Whether Safari support depends on the OS version rather than the Safari version could not be verified. The browser-compat data lists Safari 26.4 (macOS) and Safari on iOS 26.4; on iOS Safari's version is the OS version.
- Safari can also run WebTransport over **HTTP/2** when QUIC is unavailable (WebKit's Safari 26.4 notes). That path has no datagrams (`WebTransport.reliability` is `"reliable-only"`). The relay serves HTTP/3 only; a client that ends up on a reliable-only session treats it as a failure and (in `RELAY_AUTO`) falls back to the WebSocket.
- **`datagrams.maxDatagramSize` is a floor, not the limit.** Chromium 153 reports **1024**, less than the 1109 bytes of a full vnet `DATA` frame (9-byte header + `VNET_MAX_DATAGRAM`), but sends and receives datagrams of at least 1109 bytes. The brief's "check it at runtime and fail clearly if it is smaller" would have made WebTransport unusable in Chrome, so vnet checks what really fits (see Outcome).
- WebTransport needs an HTTP/3 server with TLS. `quic-go/webtransport-go` is the mature Go choice; the newest releases need Go 1.25/1.26, the relay stays on Go 1.24 and pins v0.10.0 (quic-go v0.59.0).
- Engines differ on some server and certificate details. Keep the WebSocket fallback.

## In scope

1. **Relay**
   - Add a WebTransport endpoint (HTTP/3, UDP 443 or a configurable port) using `webtransport-go`.
   - Carry the brief 10 framing:
     - control messages over a reliable bidirectional stream;
     - `DATA` over WebTransport datagrams.
   - Members on any transport (WS, WT, UDP) share rooms transparently.
2. **Certificates**
   - **Production:** a real certificate. This is a deployment note; ACME through the relay or a provided cert/key.
   - **Development:** a `--dev-cert` mode that generates a short-lived ECDSA certificate and prints its SHA-256 hash for `serverCertificateHashes`.
3. **Browser backend**
   - Add `VNET_BACKEND_RELAY_AUTO`: try WebTransport, and fall back to WebSocket after a timeout or error.
   - Report which transport was chosen via `vnet_status()` or `vnet_transport_name()`.
   - Keep the same C ring-buffer approach as 10.
   - Datagram size: `VNET_MAX_DATAGRAM` (1100) plus vnet framing must fit the browser's datagram size limit. Check `datagrams.maxDatagramSize` at runtime and fail clearly if it's smaller.
4. **Docs:** a transport comparison table in vnet's README: semantics, browser support, hosting requirements.

## Out of scope

- A native WebTransport client. Native stays on `RELAY_UDP`.
- Congestion-control tuning.

## Tests

- **Go tests:** a WebTransport session joins a room, and exchanges datagrams with a UDP member and with a WS member.
- **Playwright Chromium:** the test page connects to a local relay in dev-cert mode, using `serverCertificateHashes` passed via the page URL. It shows `transport == webtransport`, exchanges datagrams with a native client, and under a deliberately broken WT endpoint falls back to WS. Check Chromium's flags for local QUIC if needed.
- **Loss behaviour (report only):**
  - With impairment at the relay at 10% loss, the WT path shows lost datagrams arriving without stalls.
  - The WebSocket path can't be tested the same way. Application-level drops at the relay show up as loss, not as TCP head-of-line stalls. Real stalls need kernel-level loss (`tc netem`, which needs NET_ADMIN and is probably unavailable).
  - Measure the WS stall behaviour only if `tc netem` works. Otherwise say so, and record whatever numbers you have in the Outcome.
- **Datagram size:** check `datagrams.maxDatagramSize` in Chromium against `VNET_MAX_DATAGRAM` (1100) plus framing.

## Acceptance criteria

- Browsers that support WebTransport use it.
- Others fall back to WebSocket without code changes in the game.
- Dev-cert mode works for local testing.
- Production deployment is documented.

## Outcome

Landed on `vnet/11-webtransport`: relay (`8d2851a`), client and browser tests (`5484da6`), then the review-ready test, tool, CI and documentation commits that follow it. Everything in scope is implemented and tested; nothing was cut. WebTransport is part of protocol **version 1** (no new frame types); the carriage is in [`relay-protocol.md`](relay-protocol.md#webtransport), operation and **production deployment** in [`../relay/README.md`](../relay/README.md#webtransport-http3-datagrams), and the transport comparison table in the [top-level README](../README.md#transports-compared).

**What is there**

- *Relay (Go).* `wt.go` (the transport), `certs.go` (`-dev-cert`, certificate reloading), `conn.go` (the interface the hub sees a stream transport through). The hub's `session.ws *wsConn` became `session.conn clientConn`, as the brief 10 notes asked; WebSocket and WebTransport now share the handshake code (`connHandshake`), the per-address accounting (`acquireConn` / `releaseConn`) and the global cap. New flags: `-wt`, `-wt-cert`, `-wt-key`, `-dev-cert`, `-dev-cert-names`, `-max-wt-per-ip`, `-max-pending-wt-per-ip`, `-wt-queue-bytes`. The start-up line gained `wt=<addr> wt_cert_sha256=<hex>`; `/stats` gained `webtransports`, `wt_refused`, `wt_datagram_drops`, `wt_ctl_overflow`, `wt_early_datagrams`, `wt_bad_streams`. Dependencies: `quic-go/webtransport-go` **v0.10.0** with quic-go **v0.59.0**, the newest releases that build with Go 1.24 (v0.11 and later need Go 1.25 or 1.26). Moving the toolchain up and these pins with it is a follow-up; nothing in the code depends on the old versions.
- *Client (C).* `src/relay_wt.c` (WebTransport transport, `EM_JS`, no extra library or flag), `src/relay_auto.c` (URL choice for `RELAY_AUTO`), and in `relay_client.c`: a fallback to a second transport, a datagram-size probe, and `relay_transport_ops.max_data_frame`. `examples/relay_peer.c` takes `--backend ws|wt|auto --relay-wt --cert-hash --wt-timeout --maxlen --verbose` (the browser page takes them as query parameters).
- *Tests and tools.* `tests/browser/harness.js` (shared by both browser tests), `wt_browser.test.js`, `transport_loss.js` (+ `scripts/measure-transport-loss.sh`); `vnet-loadtest` can drive WebTransport clients (`-wt-every`).

**API additions** (all additive; version 0.3.0)

- `VNET_BACKEND_RELAY_WT` (4) and `VNET_BACKEND_RELAY_AUTO` (5). Native builds return `VNET_ENOTSUP` for both (the brief: native stays on `RELAY_UDP`).
- `vnet_config` (new fields at the end of the struct): `relay_wt` (the WebTransport URL for AUTO; derived from a `wss://` `relay` when NULL), `relay_cert_sha256` (64 hex digits: a development certificate, passed to the browser as `serverCertificateHashes`; NULL in production), `wt_timeout_ms` (AUTO: how long WebTransport gets, default 3000 ms; the WebSocket attempt then gets `relay_timeout_ms` in full).
- `vnet_status().transport` is `"relay-wt"` or `"relay-ws"`. With AUTO it names the transport being tried while CONNECTING (`relay-wt`, then `relay-ws` after a fallback) and is final at READY. FAILED may now carry `VNET_EMSGSIZE`. No new status calls.
- `RELAY_AUTO` for a game that used `RELAY_WS`: change the backend constant; `relay = "wss://host/vnet"` is kept and the WebTransport URL (`https://host/vnet`, same port and path) is derived.

**WebTransport carriage** (full text in `relay-protocol.md`)

- The client opens a session by `CONNECT` to `https://host:port/vnet`, then one bidirectional **control stream**. Every control frame (`CREATE_ROOM`, `JOIN_ROOM`, `LEAVE`, `KEEPALIVE`; `ROOM`, `ERROR`, `KEEPALIVE`) is `length[2] frame` (length 1 to 64; anything else closes the session). The frames are exactly the WebSocket ones: no token, no padding, no cookie.
- `DATA` is one frame per **datagram** in the WebSocket layout (at most 1109 bytes). A control frame in a datagram, and `DATA` on the stream, are ignored (counted as malformed); nothing about a session can be done from a datagram.
- One session and one control stream per QUIC connection; any other stream is reset on arrival (16 of them close the session).

**Decisions and deviations**

- **The datagram-size check is a probe, not a comparison.** The brief said to check `datagrams.maxDatagramSize` at runtime and fail clearly if it is smaller than `VNET_MAX_DATAGRAM` plus framing. **Chromium 153 reports 1024**, less than 1109, and still carries 1109-byte datagrams in both directions, so a comparison would have disabled WebTransport in the one browser that matters. The W3C text does allow a browser to drop datagrams above `maxDatagramSize`, so ignoring the number is not safe either. So when the reported size is below 1109 (or unknown), the client, after `ROOM`, sends itself a 1100-byte `DATA` datagram (port 0, which no socket has) through the relay, repeats it every 250 ms and reports `READY` only when it comes back; after 1.5 s it fails with `VNET_EMSGSIZE` ("datagram too large") or, under `RELAY_AUTO`, falls back to the WebSocket. This needs nothing from the relay (a member's datagram to its own address is ordinary routing). It was tested with a browser that really drops datagrams over 1024 (the page wraps `WebTransport` to do what the spec permits): WebTransport is rejected, the page ends up on `relay-ws` and the exchange of 1100-byte datagrams works. Without the probe that same browser gets `READY` and loses every datagram over 1024 bytes (the mutation run below shows it).
- **Fallback happens only before the room is entered.** Triggers: the transport fails (QUIC cannot connect, bad certificate hash, no `WebTransport` object, session closed), `wt_timeout_ms` passes (a firewall that drops QUIC), the session has no datagrams (`reliability` is `"reliable-only"`, Safari's HTTP/2 mode) or the probe fails. A relay *answer* (`ROOM_NOT_FOUND`, `ROOM_FULL`, `LIMIT`, `RATE`, bad version) is final: the WebSocket would say the same. After `READY` a lost session is `VNET_ECLOSED`, never a silent switch (the game holds a room and an address). The first transport is closed before the second is opened, and the handshake restarts with a **new nonce** (the relay answers the old one with `NONCE_REUSED`).
- **Liveness is the QUIC connection's.** The relay sets QUIC's keep-alive to the ping interval (5 s) and the idle timeout to `session-timeout` (15 s). A WebTransport member is alive while its connection is, whether or not the page sends frames, as with WebSocket pings (a hidden tab's timers are throttled; the browser's QUIC stack still answers). The sweeper's frame-based timeout does not apply to them. A vanished client is gone within about 15 s (tested with a client whose socket is cut). Not-yet-in-a-room connections still die after `HandshakeTimeout`.
- **`RELAY_WT` exists as well as `RELAY_AUTO`**: the explicit backend is what tests (and games that want to know) use; it never falls back.
- **QUIC Retry is always on** (`VerifySourceAddress` returns true): one extra round trip on every connection setup, in exchange for the brief 10 properties (see below).
- **CI bug found and fixed:** the relay job's container step did `curl .../stats`, which brief 10 made opt-in (`-stats`), so that step could not pass; it now checks the `LISTEN` line (and WebTransport start-up). The CI workflow still could not be run here.
- Not done: a Go/native WebTransport client in the C library (out of scope), ACME in the relay (a deployment note instead: certificate files from certbot/Caddy are re-read when they change).

**Security properties of brief 10, and how they hold for WebTransport**

| Property | Enforced by | Tested by |
|---|---|---|
| Route only within a room | the same `forward`: destination looked up *inside the sender's room*; a datagram's source address is filled in by the relay | `TestWTRoutesDataAsDatagramsOnlyWithinTheRoom`, `TestWebTransportRoutesOnlyWithinTheRoomAcrossTransports` (two rooms with the same addresses, three transports, probes of 11 addresses and another subnet, exact `unroutable`/`forwarded` counts) |
| Tokens are credentials | a WebTransport (or WebSocket) session's token is **not accepted over UDP** (`udpAuthenticated`: `s.conn != nil` is an unknown token), and nothing on a WebTransport connection needs a token: the encrypted QUIC connection is the identity | `TestWTTokenIsNoCredentialOverUDP`, `TestWebTransportTokenIsNoCredentialOverUDP` (KEEPALIVE, LEAVE and DATA with the token, from another address: `NO_SESSION`, nothing delivered, no path challenge) |
| Nonces are credentials, `getentropy` | unchanged client code (`vnet__random`); a fallback draws a **fresh** nonce through the same call, and the relay answers a repeat of a nonce only on its own connection | `TestWTHandshakeIsIdempotentAndPerConnection` (repeat on the same connection: same ROOM; the same nonce on another: `NONCE_REUSED`), `RelayFallback.AFailedTransportIsReplacedAndTheHandshakeStartsOverWithANewNonce` |
| Per-address caps, /64 and /48 keying | `wtConnContext` runs in quic-go's admission hook on every validated connection, before any handshake state is built: `acquireConn(kindWT, ipKey(addr))` (per address, pending, global `max-sessions` shared with WebSocket); released exactly once when the connection's context ends, with or without a session | `TestWTPerAddressCaps`, `...KeyIPv6ByPrefix` (two hosts of a /64 share, 40 /64s of a /48 get 3x), `...ShareTheGlobalConnectionCap`, `TestWTReleaseGivesBackOnlyWhatWasHeld`, and over real QUIC `TestWebTransportConnectionsWithoutASessionAreCappedPerAddress` (connections that never send a request) and `...SessionsArePerAddressCapped...` |
| No amplification; nothing to an unproven address | every connection goes through QUIC **Retry** first: an Initial from an unvalidated source gets one small Retry and no state; everything afterwards is addressed to the validated QUIC path (quic-go's path manager validates a migrated path before switching to it, by reading its source, not tested here) | `TestWebTransportFirstAnswerToAnUnvalidatedAddressIsASmallRetry`: the first answer to a real QUIC Initial is a Retry of 132 bytes against a 1280-byte Initial |
| Concurrency safety (brief 10 B1) | control-stream writes are serialised (`wmu`) because both the control writer and `reject()` write; datagrams are sent by **one** goroutine (quic-go's `SendDatagram` **blocks** when its 32-datagram queue is full, so nothing that holds the hub lock may call it: `deliver` only enqueues); the two reader goroutines of a session share its rate buckets under a lock; every goroutine recovers panics | `TestWTControlStreamWritesAreSerialised` (writer parked inside `Write`, then a bad frame makes the reader write too: the fake stream fails on overlap; it fails without the lock), `TestWTSlowDatagramConsumer...` (a stuck `SendDatagram`: the sender and the hub are not held up, memory stays bounded), `TestWTConcurrentControlAndDataFromOneMember` (`-race`), `TestWTRecoveredPanicLeavesTheHubUsable` |
| Bounded queues | datagrams: 128 frames **and** 64 KiB per client (drop); control stream: 32 frames (a client that lets it overflow is closed, reliable data is not silently dropped); QUIC flow-control windows at most 128 KiB per connection; 4 bidirectional and 8 unidirectional streams | `TestWTDatagramQueueIsBoundedInBytes`, `...ConsumerLosesDatagrams...`, `TestWTControlQueueOverflowClosesTheSession`, `TestWTSurplusStreamsAreResetAndAbuseIsClosed` |
| Deferred unlocks | every new lock use is `defer`red; the recovered-panic test hook covers the WebTransport datagram path | `TestWTRecoveredPanicLeavesTheHubUsable` (panic injected under the read lock from the datagram reader) |
| Per-IP caps on concurrent sessions, including not-yet-authenticated ones | `-max-wt-per-ip` (128) and `-max-pending-wt-per-ip` (32) count QUIC connections from the moment of admission, session or not; pending ones are closed after `HandshakeTimeout` by the sweeper (and by a read deadline) | `TestWTHandshakeTimeoutClosesAndReleases`, `TestWebTransportHandshakeTimeoutFreesAPendingConnection` |
| Malformed control | length 0 or over 64 and truncated/garbled frames close the session with `ERROR(MALFORMED)` and create no state; unknown frames in a room are ignored; control frames in datagrams do nothing | `TestWTMalformedControlStreamsAreRefusedWithoutState` (12 cases), `TestWTControlFramesInDatagramsDoNothing`, `FuzzHandleWT` (20-25 s: ~100000 executions per run, no finding) |

Left as they are, and documented in `relay-protocol.md`: QUIC's own stateless reset and version negotiation packets (RFC 9000 requires them to be smaller than their trigger; that was not checked in quic-go) and Initial packets from a *real* address at a high rate (each draws one Retry) are bounded by quic-go's queues, not by this relay; the relay-wide forged-flood remarks of brief 10 apply to the UDP port as before.

**Tests**

- Go: **134** top-level tests and fuzz targets (was 82); 52 of them new for WebTransport, certificates and the refactor, plus `FuzzHandleWT`. `go vet`, `gofmt` clean; `go test -race -count=5` passes (60 s); fuzz targets `FuzzHandleUDP`, `FuzzHandleWS`, `FuzzHandleWT` 25 s each, no finding. Two layers: `wt_test.go` runs the relay against a fake WebTransport session (blocked `SendDatagram`, a control stream stuck in `Write`, overlapping writes, hostile bytes, no sleeping: the fake clock plus bounded waits), `wt_e2e_test.go` uses real QUIC clients (the webtransport-go dialer, accepting the development certificate by hash only) with UDP and WebSocket members in the same rooms.
- C (GoogleTest): **202** tests (was 179); `ctest` has **204** entries (was 181; the extra two are the plain-C smoke test and the Go tests), all pass, 1 skip (the IPv6 loopback test: this container has no IPv6), identically under gcc, clang `-Werror` and ASan+UBSan. New: `RelayFallback` (11), `RelayProbe` (9), `RelayAutoArguments` (3), all with scripted transports.
- Browser (Chromium 153 under Playwright 1.63): `relay_browser.test.js` 20 checks (unchanged behaviour, now on the shared harness) and `wt_browser.test.js` **61 checks**, three runs in a row all green: auto picks WebTransport (and opens no WebSocket); 150 numbered, content-checked datagrams each way against a native client with payloads up to 1100 bytes; WebTransport browser with a native host and as host; WebTransport to WebSocket browsers; derived `https://` URL; and the **fallback** in six forms (nothing listening, a relay without WebTransport, a firewall that drops QUIC, wrong certificate hash, no `WebTransport` object, a browser that drops datagrams over `maxDatagramSize`), each ending on `relay-ws` with a complete exchange; `RELAY_WT` failing with its reason where AUTO falls back; relay-side checks that nothing was malformed, rate limited or leaked.
- Windows: MinGW-w64 builds `libvnet.a` (with the new files) and the smoke test with `-Werror` for x86_64 (runs under Wine 9: ok) and i686 (builds); `relay_peer.exe` under Wine exchanges 200 datagrams each way with a Linux peer through the Go relay, as guest and as host.
- Docker was not built (no daemon); the build command was run directly. CI was not run.

**Proof that the key tests fail when broken** (scratch copies; broken, run, restored)

- Go, 37 mutations: **36 killed**, 1 redundant. Killed: QUIC Retry off; WebTransport token accepted over UDP; control-stream writes unserialised; datagram enqueue blocking (the test hangs, then fails); datagram queue unbounded in bytes; control overflow dropping instead of closing; the liveness exemption removed; rate buckets unlocked (the `-race` run reports the race); control frames in datagrams dispatched; `DATA` on the stream forwarded; control length limit raised or zero allowed; per-address WebTransport cap missing; pending slot never freed; release not exactly-once; release forgotten; IPv6 not keyed by /64; WebTransport not counted against the global cap; origin check removed; many sessions per connection; surplus streams not reset; handshake timeout not swept; early datagrams uncounted; payload limit off by one; too-large datagram fatal; panic not recovered; session not removed when the connection dies; dev cert valid 15 days / P-384 / hash of the wrong bytes; certificate reload on every handshake / never; QUIC keep-alive off; QUIC idle timeout left at its default; connection never released; wrong length prefix. The one that survives (CheckOrigin inside webtransport-go set to allow all) is redundant by design: the relay checks the origin itself first. Three survived the first run and led to a new test or a corrected mutation (the byte bound of the datagram queue, exact release, a stream-refusal mutation that did not compile); the length-limit test had been strengthened (a valid CREATE_ROOM one byte too long) after the same check on paper.
- C, 29 mutations: **28 killed**; the survivor (AUTO ignoring `relay_wt`) cannot be observed natively and is killed by the browser test. Killed: probe accepting any `DATA`, ignoring the sender, content or length; threshold off by one; no probe at all; probe failure not falling back, wrong error; probe never repeated, never ending; READY during the probe; no LEAVE during the probe; fallback keeping the old nonce, not restoring the keepalive, not restarting the handshake clock, on a relay error, after READY (timeout and failure), reusable, without timeout, leaking the first transport; ENOTSUP unmapped; URL derivation accepting `ws://`; hash length/digit checks; `http://` accepted; a malformed hash falling back. (The first run of this list was invalidated by stale object files and redone from a clean build.)
- Browser, 3 mutations, all killed by `wt_browser.test.js`: AUTO ignoring `relay_wt` (12 checks fail), **datagram-size probe skipped** (the browser that drops datagrams over 1024 gets `READY` on `relay-wt`, and the exchange of 1100-byte datagrams times out: 7 checks fail), no WebSocket fallback (the fallback scenarios fail).

**Loss behaviour** (report only; Chromium 153, 100 datagrams/s of 200 bytes for 10 s, relay on loopback; `scripts/measure-transport-loss.sh`)

`tc netem` is **not available**: `tc` is not installed, the process lacks `CAP_NET_ADMIN`, and the kernel is built without `CONFIG_NET_SCH_NETEM` (checked in a user+network namespace too). So WebSocket stalls under real packet loss **could not be measured**. Instead WebTransport ran with a user-space UDP proxy that drops 10% of the QUIC packets in both directions (real loss in the network path); WebSocket ran with the sender dropping 10% itself (application-level loss, as the brief said, which shows loss, not head-of-line stalls).

| Run | Delivered | One-way latency of delivered datagrams p50 / p99 / max | Gap between delivered datagrams p50 / p99 / max | Round trip of a KEEPALIVE on the reliable path p50 / p99 / max |
|---|---|---|---|---|
| WebTransport, no loss | 1000 of 1000 | 0.8 / 1.4 / 2.1 ms | 8.6 / 13.1 / 13.5 ms | 0.8 / 1.5 / 1.7 ms |
| **WebTransport, 10% of QUIC packets lost** | **900 of 1000 (10.0% lost)** | **1.0 / 1.8 / 19.2 ms** | **8.7 / 21.5 / 42.1 ms** | 1.3 / **86.7** / 94.1 ms |
| WebSocket, no loss | 1000 of 1000 | 0.6 / 1.1 / 5.3 ms | 8.6 / 13.0 / 15.8 ms | 0.6 / 1.2 / 1.2 ms |
| WebSocket, 10% dropped by the sender | 914 of 914 sent (8.6% lost) | 0.6 / 3.9 / 5.7 ms | 8.7 / 21.6 / 34.2 ms | 0.6 / 1.6 / 4.8 ms |

(113 of 1215 QUIC packets toward the browser and 123 of 1141 toward the relay were dropped.) Lost datagrams are simply gone, 10.0% as configured, with no duplicates and no reordering; the datagrams that did arrive were not delayed (p99 1.8 ms, against 1.4 ms without loss), and the gap between deliveries only grows by the number of consecutive losses (the maximum of 42 ms is four lost packets in a row). The **reliable** path (the control stream) is what QUIC repairs by retransmission: its round trip shows the stalls (p99 86.7 ms, max 94 ms) while the datagrams beside it do not. That is the property the brief wants from WebTransport; a lost TCP segment would hold up every later WebSocket message by a retransmission timeout. The last row is only what an application-level drop (vnet's impairment layer does the same) looks like: a drop is a drop.

**Datagram size found in Chromium**: `datagrams.maxDatagramSize` = **1024** (Chromium 153.0.8010.12), the same at `ready`, after a few seconds and after traffic. Writes above it are not dropped: a probe server received browser datagrams of 1025 to at least 1200 bytes, and a full vnet frame (9 + 1100 = 1109 bytes) makes the round trip browser, relay, browser in every run of the browser test (the vnet log line "a 1100 byte datagram fits over relay-wt").

**Chromium flags needed: none.** A default `chromium.launch()` under Playwright (headless, version 153.0.8010.12) connects to `https://127.0.0.1:PORT/vnet` with `serverCertificateHashes` for a certificate from `-dev-cert` (ECDSA P-256, 13 days, SAN `localhost`, `127.0.0.1`, `::1`), with no `--origin-to-force-quic-on`, no `--ignore-certificate-errors`, and no `--enable-quic`. The page must be a secure context (the test serves it from `http://127.0.0.1`, which is).

**Load sanity** (one relay process, 50 rooms x 4 clients x 30 packets/s, 100-byte payloads, 30 s, load generator on the same 4-core machine; `scripts/loadtest.sh`)

| Clients | Packets into relay | Lost | Latency p50 / p99 | Relay CPU | Relay RSS |
|---|---|---|---|---|---|
| all UDP (as in brief 10) | 5987/s | 0 | 108 us / 334 us | 9.3% of one core | 9.1 MB |
| all WebTransport | 5986/s | 0 | 378 us / 6.5 ms | 55.8% | 59.7 MB (peak 73 MB) |
| a third WebTransport, a third WebSocket, a third UDP | 5982/s | 0 | 228 us / 863 us | 45.7% | 31.7 MB |

Each WebTransport datagram is one encrypted QUIC packet and one `write` system call, so it costs several times what a UDP or WebSocket packet does; about 55% of a core for 200 clients is the honest number to size a host by (the load generator's own QUIC clients share the machine).

**Browser support facts verified** (October 2026)

- Verified from the MDN browser-compat-data 8.1.7 (the data behind MDN's tables) and `web-features`: `WebTransport` Chrome 97, Edge 97, Firefox 114, Safari 26.4, Safari on iOS 26.4; `serverCertificateHashes` Chrome 100, Edge 100, Firefox 125, Safari 26.4, iOS 26.4; Baseline **"low" (newly available) since 2026-03-24**, not widely available. caniuse agrees on the versions (Edge 98). MDN's WebTransport page: "Since March 2026, this feature works across the latest devices and browser versions". Certificate rules: W3C WebTransport, "Server Authentication using Certificate Hashes" (two weeks), MDN `WebTransport()` constructor (X.509v3, valid now, less than 2 weeks, ECDSA secp256r1 at minimum, no RSA, SHA-256 only, dedicated connections only). WebKit's Safari 26.4 post confirms WebTransport shipped in 26.4, runs over HTTP/3 and falls back to HTTP/2 when QUIC is unavailable.
- **Could not be verified**: that Safari's support depends on the OS version rather than the Safari version (the data lists "Safari 26.4"; on iOS they coincide; for macOS it is unknown); that `serverCertificateHashes` behaves identically in Safari and Firefox (the data says it exists; only Chromium was run); the W3C text's exact public-key algorithm list (the fetched part of the spec did not include it; MDN's wording is used).

**Not verified / follow-ups**

- Only Chromium was run. Firefox and Safari (and their `serverCertificateHashes` handling) are untested; in `RELAY_AUTO` a failure there is a fallback, not an error. macOS and the GitHub Actions workflow are unverified, as before. The relay's Docker image was not built.
- Production behaviour behind real networks: a CA-issued certificate, `wss://` plus WebTransport on one host name, a proxy in front, the UDP buffer sizes quic-go asks for, and carriers that block UDP/443 were reasoned about and documented, not tested. quic-go's migration handling (new source address after a NAT rebinding) was read, not exercised.
- The relay cannot tell a hidden tab that stopped running its keepalive from a live one: like WebSocket pings, QUIC acknowledgements keep the member. Whether `vnet_poll()` runs in a background tab is brief 12's problem; the relay side is fine. The client's three-silent-keepalive-intervals check still applies to a throttled page.
- `webtransport-go` v0.10.0 implements an older draft than the newest releases; it interoperates with Chromium 153 (tested), and `ApplicationProtocols` (`WT-Available-Protocols`) are not used. Update both pins when the Go toolchain moves to 1.25 or later.
- For **Bit Riot brief 12**: use `VNET_BACKEND_RELAY_AUTO` with the same `wss://` URL as for WebSocket; show `vnet_status().transport`; allow up to about 3 s more "connecting" when WebTransport is blocked; WebTransport datagrams can be lost or reordered, so the lockstep protocol's repeat-every-input behaviour is now actually exercised; the per-address caps of WebSocket and WebTransport are separate, so a classroom behind one NAT can hold up to 128 + 128 connections.

**Review fix round**

- **A global cap of its own for WebTransport (`-max-wt`, default 4000).** The review measured about 200 KB and 13 goroutines of relay memory per WebTransport member, and `-max-sessions` (20000, shared with WebSockets) with per-address caps alone would let about 157 IPv4 addresses (128 each), or 40 IPv6 /48s, hold 4 to 6 GB for ever. Measured here: 1000 mostly idle clients (1 packet/s each) took 157 MB resident in all (about 150 KB each); 200 clients at 30 packets/s took 60 MB (about 250 KB each). So 250 to 300 KB per connection is budgeted, and the default of 4000 is about 1.2 GB. WebSockets are not affected by it; a client refused by it falls back to the WebSocket. Tests: `TestWTHasAGlobalCapOfItsOwn` (different addresses, so no per-address cap is involved; WebSocket unaffected; exactly-once release), `TestWebTransportGlobalCapRefusesConnectionsFromAnywhere` (real QUIC). Sizing guidance and a memory table are in `relay/README.md` ("Sizing WebTransport").
- **CI container step.** It now waits (50 tries at 0.2 s) for the `LISTEN` line and matches `wt=[^ ]*:4433 wt_cert_sha256=<64 hex>` so that it accepts `wt=[::]:4433` and `wt=0.0.0.0:4433`; the line is printed after the HTTP listener is up (the dev certificate is made in between). The regex was checked against a real line from the binary; the workflow itself still could not be run.
- **Control frames are never dropped by a datagram burst.** The JavaScript inbox in `relay_wt.c` has two queues: datagrams (256, then dropped, as before) and control frames (256, served first). A control queue that overflows fails the session visibly (the game sees `FAILED`/`VNET_ECLOSED`) instead of losing a `ROOM`, `ERROR` or `KEEPALIVE` quietly. The queue logic is one function (`T.enqueue`) so that the browser test can drive it: `wt_browser.test.js` pushes 1000 datagrams and one control frame in a single synchronous step (256 datagrams kept, 744 dropped, the control frame kept), and 300 control frames into a stuck page (the session fails). Mutation: control frames sharing the datagram queue is caught by both checks.
- **Dev-cert mode.** The start-up log says "DEVELOPMENT certificate (local trials only, NOT FOR PRODUCTION)", that the hash is new at every start and when it expires; a warning is logged if `-wt` is bound to a non-loopback address (`DevCertWarning`, tested); `relay/README.md` documents the hash changing at every start and the 13-day life.
- **Certificate expiry** (`certs.go`): `CertReloader` (and the dev certificate, through `NewStaticCert`) logs a `WARNING` when less than 7 days remain and an `ERROR` once the certificate has expired, at most once an hour; a renewed certificate resets it. Tests `TestCertificateExpiryIsReported`, `TestExpiryIsTrackedAcrossReloads`.
- **Production recipe for the Docker image** (`relay/README.md`, item 7): `privkey.pem` is `0600 root` and the image runs as uid 65534, and `live/` holds symlinks; the documented workable setup is a certbot deploy hook that installs group-readable copies in `/etc/vnet-relay/certs`, which is what the container mounts (the relay re-reads them when they change).
- **Probe notes.** `relay-protocol.md` now has the argument for why one successful probe is enough (QUIC's 1200-byte minimum and the roughly 1140 bytes a full frame needs in one packet, so only the endpoints' own limits can differ, and the probe measured those), and says that under very heavy loss a spurious `VNET_EMSGSIZE` is possible with `RELAY_WT` (it is what `RELAY_AUTO` falls back on); also noted in `vnet.h`. The "limits that players behind one address share" paragraph lists the WebTransport caps.
- **Results.** Go 139 top-level tests and fuzz targets (was 134), `-race -count=3` and a 15 s `FuzzHandleWT` clean; ctest 204 entries pass (1 IPv6 skip); browser: `relay_browser.test.js` 20 checks and `wt_browser.test.js` 63 checks green. Mutations of the new tests, each killed: global cap missing, never released, applied to WebSockets; expiry never reported, not rate limited, expiry treated as a warning, not tracked across reloads; dev-cert warning inverted; JavaScript control queue shared with datagrams (browser level).
