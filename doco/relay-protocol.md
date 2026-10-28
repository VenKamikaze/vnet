# vnet relay protocol, version 1

The relay puts clients into **rooms** and forwards datagrams between the members of a room. This document is the wire format. The server is `relay/` (Go), the client is `src/relay_client.c` (C), and the browser transports are `src/relay_ws.c` (WebSocket) and `src/relay_wt.c` (WebTransport). Keep them in step; the constants are in `relay/protocol.go` and `src/relay_proto.h`.

- [Overview](#overview)
- [Frames](#frames)
- [Handshake and idempotent retries](#handshake-and-idempotent-retries)
- [WebTransport](#webtransport)
- [Rooms and virtual addresses](#rooms-and-virtual-addresses)
- [Sessions, keepalive and timeouts](#sessions-keepalive-and-timeouts)
- [Limits](#limits)
- [Security properties](#security-properties)
- [Errors](#errors)
- [Client guidance](#client-guidance)
- [Versioning](#versioning)

## Overview

- One protocol, three transports. All carry **whole frames**; a frame never spans datagrams or WebSocket messages.
  - **UDP:** one frame per datagram, to the relay's UDP port. Unreliable, so the client retries the handshake, and every frame after the handshake carries the session **token**.
  - **WebSocket:** one frame per **binary** message on `/vnet`. Reliable and ordered, so no retries, no padding and no token (the connection is the identity). Text messages are not part of the protocol.
  - **WebTransport** (HTTP/3 over QUIC, `CONNECT` to `/vnet`): control frames on one reliable bidirectional stream, each with a 2-byte length; `DATA` as unreliable QUIC datagrams. No retries, no padding and no token (the encrypted QUIC connection is the identity). See [WebTransport](#webtransport).
- All integers are **big-endian**. A frame is a 1-byte type followed by its fields.
- The relay hands each member a virtual IPv4 address `10.0.0.N` and forwards `DATA` between addresses **of the same room**. Nothing else is ever a destination: the relay is not a proxy.
- vnet carries unreliable datagrams only (D7). The relay does not retransmit, order or acknowledge anything. Over WebSocket, TCP happens to deliver in order; a game must not rely on that.

## Frames

`token` is 16 bytes, `nonce` 8 bytes, `code` 6 ASCII bytes, `vaddr` 4 bytes (`10 0 0 N`). `ver` is the protocol version, currently **1**.

### Client to relay

The WebTransport layouts are the WebSocket ones (no token); `CREATE_ROOM`, `JOIN_ROOM`, `LEAVE` and `KEEPALIVE` go on the control stream and `DATA` in a datagram.

| Type | Name | UDP layout | WebSocket / WebTransport layout |
|---|---|---|---|
| `0x01` | `CREATE_ROOM` | `01 ver nonce[8] cookie[16]` + zero padding to **at least 64 bytes** | `01 ver nonce[8]` |
| `0x02` | `JOIN_ROOM` | `02 ver nonce[8] code[6] cookie[16]` + zero padding to **at least 64 bytes** | `02 ver nonce[8] code[6]` |
| `0x03` | `LEAVE` | `03 token[16]` | `03` (the relay then closes the connection) |
| `0x04` | `KEEPALIVE` | `04 token[16]` | `04` |
| `0x06` | `PATH_RESPONSE` | `06 token[16] cookie[8]` | (not used) |
| `0x10` | `DATA` | `10 token[16] dst_vaddr[4] dst_port[2] src_port[2] payload` | `10 dst_vaddr[4] dst_port[2] src_port[2] payload` |

- `payload` is 0 to **1100 bytes** (`VNET_MAX_DATAGRAM`). The largest frame in either direction is therefore 1125 bytes (UDP `DATA`). Longer payloads are dropped, never truncated.
- Padding is zero bytes after the fields and is ignored. A UDP `CREATE_ROOM`/`JOIN_ROOM` shorter than 64 bytes is **dropped without a reply** (see [Security properties](#security-properties)). Trailing bytes beyond the fields are padding, not an error.
- Room codes are matched case-insensitively (the relay upper-cases them).

### Relay to client

| Type | Name | Layout |
|---|---|---|
| `0x81` | `ROOM` | `81 ver nonce[8] vaddr[4] token[16] code[6] max_members keepalive_s` (38 bytes) |
| `0x82` | `ERROR` | `82 ver nonce[8] reason` (11 bytes) |
| `0x04` | `KEEPALIVE` | `04` (the answer to a `KEEPALIVE`) |
| `0x05` | `PATH_CHALLENGE` | `05 cookie[8]` (UDP only; see [Where the relay sends](#where-the-relay-sends-address-validation)) |
| `0x07` | `RETRY` | `07 ver nonce[8] cookie[16]` (UDP only, 26 bytes; the stateless retry, see [Handshake](#handshake-and-idempotent-retries)) |
| `0x10` | `DATA` | `10 src_vaddr[4] dst_port[2] src_port[2] payload` |

- `ROOM` answers `CREATE_ROOM` and `JOIN_ROOM`. `nonce` echoes the request; `vaddr` is the member's address; `token` is the session token (WebSocket clients may ignore it); `code` is the room code; `max_members` is the room's member cap; `keepalive_s` is the interval the relay suggests for `KEEPALIVE` frames.
- `RETRY` answers a UDP `CREATE_ROOM`/`JOIN_ROOM` that carries no valid cookie. The relay keeps **no state** for it.
- `ERROR` carries the nonce of the request it answers. Errors that are not about a request (`NO_SESSION`, `CLOSED`, `TIMEOUT` on an established session) carry the session's nonce, or all zeros if the relay does not know the session.
- `DATA` has the same layout in both directions, with the address field meaning **destination** when a client sends it and **source** when the relay delivers it. The relay fills in the source from the sender's membership; a client cannot choose it. `src_port` and `dst_port` are opaque to the relay: they are the game's vnet ports, carried so that the receiving vnet context can hand the datagram to the right local socket.
- Clients **ignore frame types they do not know** (forward compatibility).

## Handshake and idempotent retries

The source address of a UDP datagram can be forged, so over UDP the relay creates nothing for a source until that source has shown it can receive at its address. This is a stateless retry, as QUIC does it:

1. The client picks a random 8-byte **nonce** once and sends `CREATE_ROOM` (to make a room) or `JOIN_ROOM` (to enter one) with an all-zero `cookie`.
2. **UDP only:** the relay answers `RETRY(nonce, cookie)` and forgets the request: no session, no room, no budget used, nothing stored. The cookie is an HMAC (key: a random secret that never leaves the relay and changes at each start) over the source address and port, the nonce and a 16-second time bucket; the current and the previous bucket are accepted. `RETRY` is 26 bytes, always smaller than the (at least 64-byte) request, and counts against the sender's reply budget like any answer to an unproven source.
3. The client repeats **the same request, nonce included, with the cookie filled in**. Only now does the relay apply its limits and create state; the session's address is the source of this request, which has just been proved. A cookie cannot be moved to another address, port or nonce, and expires; one that is missing, wrong or expired just draws a fresh `RETRY`. (The client keeps repeating until it gets an answer, so a lost `RETRY`, request or reply costs one retransmission.)
4. The relay answers `ROOM` or `ERROR`. Over UDP either datagram can be lost, so the client **repeats the identical request (with the cookie)** until it gets an answer. The nonce is an idempotency key:
   - a repeat of a request that already succeeded, **from the session's own address**, returns the same `ROOM` (same token, same address) and creates nothing, so a retry never makes a second room or a second member;
   - a repeat from **any other address** gets `ERROR(NONCE_REUSED)`, whatever its cookie: the ROOM, and with it the token, goes only to the address that was proved when the session was created (a client whose ROOM was lost *and* whose NAT mapping changed in the same moment has to start again with a new nonce);
   - a nonce that was used for a *different* request (CREATE vs JOIN, or another code) gets `ERROR(NONCE_REUSED)`;
   - when a session ends, its nonce is remembered for 60 s (`NonceTTL`) so that a late, reordered duplicate of the handshake gets `ERROR(CLOSED)` instead of resurrecting a room.
5. The vnet client sends the first request at once, answers a `RETRY` immediately, retransmits after 250 ms, 500 ms more, then every 1000 ms, and gives up (`VNET_ETIMEDOUT`) after `relay_timeout_ms` (default 10 s).
6. **Over WebSocket** the TCP handshake already proved the address: there is no cookie field and no `RETRY`; the handshake is sent once, when the connection opens. Repeating the same request on the same connection returns the same `ROOM`; a different request on a connection that already has a session gets `ERROR(ALREADY_IN_ROOM)`; the same nonce on another connection gets `ERROR(NONCE_REUSED)`.
7. A WebSocket connection must send `CREATE_ROOM` or `JOIN_ROOM` within 10 s (`HandshakeTimeout`) and as its first frame; anything else gets `ERROR(MALFORMED)` and the connection is closed. A browser cannot tell why a WebSocket failed to open (relay down, or HTTP 503 because this address has too many sockets pending right now), so the vnet browser backend retries a failed open five times over about 4 s before reporting `VNET_ECONNREFUSED`.

`ERROR` answers over WebSocket leave the connection open (except as above), so a client may try another code. Failed attempts count against the per-source handshake limit.

## WebTransport

WebTransport gives a browser what WebSocket cannot: **unreliable datagrams** that are not held up behind a lost packet. The relay serves it over HTTP/3 on a UDP port of its own (`-wt`), with a TLS certificate (production) or a development certificate that the page pins by hash. A session is the HTTP/3 extended `CONNECT` request for `https://host:port/vnet` (path `/vnet`, as for WebSocket).

**Carriage of the frames**

- **Control stream.** The client opens **one** bidirectional stream and sends `CREATE_ROOM` or `JOIN_ROOM` on it as its first bytes; the relay answers on the same stream. Every control frame (client to relay: `CREATE_ROOM`, `JOIN_ROOM`, `LEAVE`, `KEEPALIVE`; relay to client: `ROOM`, `ERROR`, `KEEPALIVE`) is written as
  `length[2] frame[length]`, big endian. `length` is 1 to **64**; anything else (zero, or more than 64) is a framing error that cannot be recovered on a byte stream: the relay answers `ERROR(MALFORMED)` and closes the session. A stream that ends or is reset ends the session. The frames themselves are exactly the WebSocket ones (no token, no padding, no cookie; the QUIC handshake, with Retry, has already proved the address).
- **Datagrams.** `DATA` is one frame per QUIC datagram, in the WebSocket layout (`10 dst_vaddr[4] dst_port[2] src_port[2] payload` to the relay; `10 src_vaddr[4] dst_port[2] src_port[2] payload` from it). A datagram is at most 9 + 1100 = **1109 bytes**. A datagram that is not `DATA` (a control frame, an empty one, an unknown type) is counted as malformed and ignored: **a handshake in a datagram does nothing**. `DATA` on the control stream is likewise ignored (in a room) or a protocol error (before one). Datagrams that arrive before the session has entered a room are dropped (`wt_early_datagrams`).
- **Surplus streams.** The relay reads no stream other than the control stream. A second bidirectional stream and any unidirectional stream the client opens (other than HTTP/3's own) are reset at once; more than 16 of them close the session. QUIC stream limits are 4 bidirectional (the `CONNECT` stream, the control stream, margin) and 8 unidirectional.
- **One session per QUIC connection.** A second `CONNECT` on the same connection gets HTTP 429.
- **Handshake** as for WebSocket: idempotent by nonce (a repeat on the same connection returns the same `ROOM`; the same nonce on another connection gets `ERROR(NONCE_REUSED)`; a different request in a room gets `ERROR(ALREADY_IN_ROOM)`). The first control frame must arrive within `HandshakeTimeout` (10 s) of the connection being accepted, or the connection is closed.
- **Token.** `ROOM` carries a token as on every transport, but a WebTransport session is identified by its connection: **the token is no credential**. `DATA`, `KEEPALIVE`, `LEAVE` or a path response bearing a WebTransport (or WebSocket) session's token over UDP gets `ERROR(NO_SESSION)`.
- **Liveness.** The relay sends QUIC keep-alives (`PingInterval`, 5 s) and sets QUIC's idle timeout to `SessionTimeout` (15 s): a client whose QUIC stack stops answering (closed laptop, pulled cable) loses its connection, and with it its membership, within about that long. While the connection is up the member is alive, whether or not the page sends anything, as with WebSocket pings; a hidden browser tab whose timers are throttled keeps its session. The sweeper's frame-based session timeout therefore does not apply to WebTransport members. The client's `KEEPALIVE` is still answered with `KEEPALIVE`, and the client treats three silent keepalive intervals as a lost session.
- **Writes.** The relay writes the control stream from one goroutine (and from `reject`, under the same lock) and sends datagrams from another, from a queue bounded in frames (128) and bytes (64 KiB): a client that cannot keep up loses datagrams (`wt_datagram_drops`); one whose control stream backs up by more than 32 frames is closed (`wt_ctl_overflow`). A datagram larger than the connection can carry is dropped and counted, not fatal.

**Browsers do not promise 1109 bytes.** `datagrams.maxDatagramSize` is a conservative bound: Chromium 153 reports **1024** while delivering datagrams of at least 1109 bytes in both directions. The vnet client therefore treats a reported size below 1109 as "unknown" and **probes** (see [Client guidance](#client-guidance)); it does not trust the number and does not refuse on it.

**Certificates.** Production: a certificate for the relay's host name, from a public CA (`-wt-cert`, `-wt-key`). Development: `-dev-cert` makes a 13-day ECDSA P-256 certificate and prints its SHA-256; the page passes it as `serverCertificateHashes`. Browsers accept a certificate by hash only if it is X.509v3, valid now, has a validity period of **less than two weeks**, and does not use an RSA key.

## Rooms and virtual addresses

- A room is created by `CREATE_ROOM` and lives until its last member is gone, or until it has carried no `DATA` for `RoomIdleTimeout` (default 30 min; keepalives do not count).
- The room code is 6 symbols from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (A-Z without `I` and `O`, plus `2`-`9`: no `0/O` or `1/I` look-alikes), drawn from a CSPRNG. 30 bits, so codes are not secret: anyone with a code can join while the room has space. Rooms are not authenticated (out of scope).
- **The creator is always `10.0.0.1`.** Games may treat "host = `10.0.0.1`" as a convention (Bit Riot brief 12 does). vnet resolves `"room-host:PORT"` to `10.0.0.1:PORT`.
- Joiners get the **lowest free address from `10.0.0.2` up to `10.0.0.<max_members>`**, so addresses never exceed the member cap. An address is stable for the member's whole lifetime in the room; a member that leaves frees its address for the next joiner.
- `10.0.0.1` is **never reissued** while the room exists, even after the creator has left (the room stays open for the others, with one slot fewer). There is no host migration; rejoining as the host is not possible.
- A late datagram meant for a member that has left can reach whoever later gets that address. Games that care should put their own session id in their packets.
- A member may send to its own address; the relay delivers it back. A destination that is not a current member of the sender's room (an address that is free, outside `10.0.0.0/24`, or a member of another room) is dropped and counted (`unroutable`).
- Member cap: 8 by default, `2`-`254` configurable (`-max-members`). One cap for the whole relay.

## Sessions, keepalive and timeouts

- A **session** is one member of one room. Over UDP it is identified by its **token**, not by its source address: a frame bearing the token is accepted from any address, so a NAT rebinding does not break a session. *Where the relay sends* is a separate matter, see below. Over WebSocket and WebTransport the session lives as long as the connection.
- `KEEPALIVE`: the client sends one about every 5 s (`keepalive_s` from `ROOM`); the relay answers with a one-byte `KEEPALIVE`. This keeps NAT mappings open, keeps the session alive, and lets the client notice a dead relay. A client's own interval must be at most a third of the relay's session timeout (vnet accepts 100 to 5000 ms and clamps the relay's suggestion to 5 s).
- **Relay side:** a session with no authenticated frame for 15 s (`SessionTimeout`; WebSocket pongs count) is dropped, and a UDP member is told with `ERROR(TIMEOUT)`. A WebTransport member is dropped when its QUIC connection is (see [WebTransport](#webtransport)). The relay pings WebSocket clients every 5 s; browsers answer pings by themselves, even in a background tab whose timers are throttled, so a hidden page keeps its session as long as the connection is up.
- **Client side:** the vnet client declares the session lost (`VNET_ETIMEDOUT`) after three keepalive intervals without any frame from the relay, and `VNET_ECLOSED` on `ERROR(NO_SESSION | CLOSED | TIMEOUT)` or when the WebSocket or WebTransport session closes.
- `ERROR(NO_SESSION)` answers an authenticated-type frame (`KEEPALIVE`, `LEAVE`, `DATA`) whose token is unknown (expired, never issued, the relay restarted, or a WebSocket session's token used over UDP).
- `LEAVE` is best effort over UDP (a lost `LEAVE` is just the 15 s timeout).

### Where the relay sends (address validation)

A UDP session's address is **proved when the session is created** (the handshake cookie, above) and afterwards moves only on proof:

1. A frame bearing the token from a source other than the session's address (a NAT rebinding, or a forgery) is processed (a token is the right to send), but does **not** move the session. The relay answers it with `PATH_CHALLENGE(cookie)` to that source: 9 bytes, always smaller than the frame that triggered it, at most one per session per second, and counted against the per-source reply limits.
2. The client answers every `PATH_CHALLENGE` it gets from the relay with `PATH_RESPONSE(token, cookie)`, from the same socket (so from the new address). The relay moves the session to the source of a `PATH_RESPONSE` only if the cookie is the one it sent to *that* source, within 10 s, once. Anything else is dropped and counted.
3. The `ROOM` reply, and with it the token, is only ever sent to the address that echoed a handshake cookie. A request with a forged source never receives a cookie, so it never creates a session and never learns a token; and a second request with the same nonce from a real address learns nothing (point 4 of the handshake).

An attacker who has stolen a token can send as that member, and can cause one 9-byte challenge per second at a spoofed address; it cannot make the relay's traffic go to an address that does not answer.

## Limits

All configurable; the defaults are in `relay/config.go`, the flags in `relay/cmd/vnet-relay`.

| Limit | Default | Effect when exceeded |
|---|---|---|
| members per room | 8 | `ERROR(ROOM_FULL)` |
| rooms | 1000 | `ERROR(LIMIT)` on `CREATE_ROOM` |
| members (and open WebSockets) | 20000 | `ERROR(LIMIT)`; WebSocket upgrade refused with HTTP 503 |
| members per source address | 128 | `ERROR(LIMIT)` |
| live rooms created per source address | 16 | `ERROR(LIMIT)` on `CREATE_ROOM` |
| the same, for an IPv6 /48 (all /64s in it together) | 4 times the per-address caps | as above |
| open WebSockets per source address / not yet in a room | 128 / 32 | HTTP 503 on the upgrade (the browser backend retries) |
| payload | 1100 bytes | frame dropped |
| per-client packets | 600/s, burst 1200 | frame dropped, counted (`rate_limited`) |
| per-client bytes (whole frames) | 600000/s, burst 1200000 | frame dropped |
| handshakes per source IP (counted only once a valid cookie has proved the source) | 20/s, burst 50 | `ERROR(RATE)` if a reply is allowed, else silence |
| replies to unauthenticated packets (`RETRY`, `ERROR`, `PATH_CHALLENGE`), per source IP | 40/s, burst 100 | no reply |
| `RETRY` and `PATH_CHALLENGE` replies to unauthenticated packets, overall | 50000/s | no reply |
| `ERROR` replies (bad version, unknown token) to unauthenticated packets, overall | 5000/s | no reply |
| `ROOM` and `ERROR` replies to a source that has just echoed a valid cookie | not charged to the budgets above | (still never larger than the request) |
| WebSocket send queue | 128 frames and 64 KiB | frames to that client dropped (`ws_queue_drops`) |
| WebSocket write | 5 s | the connection is closed |
| WebSocket message size | 1125 bytes | connection closed |
| QUIC connections per source address (a session or not) / not yet in a room | 128 / 32 | the connection is refused (QUIC `CONNECTION_REFUSED`); the browser backend falls back to WebSocket |
| WebTransport datagram queue | 128 datagrams and 64 KiB | datagrams to that client dropped (`wt_datagram_drops`) |
| WebTransport control stream: frame length / queued answers / write | 64 bytes / 32 frames / 5 s | the session is closed |
| WebTransport surplus streams | 16 | the session is closed |

Connections of every kind (WebSocket and WebTransport) count together against `max-sessions`, and the per-address numbers above are per source address as seen by the relay (IPv6 by /64, with the /48 factor).

Limited frames are *dropped*, not queued: vnet is unreliable, and queueing behind a slow link only adds latency.

## Security properties

The relay is a router for the members of a room, not a proxy and not a gateway to the internet.

- **No forwarding to non-members.** A `DATA` frame is delivered only to a current member of the sender's room, found by the destination's last address byte *inside the sender's room*.
- **Where the relay sends, precisely.** Every datagram the relay sends is addressed either (a) to the *address of a session*, which is the source of a handshake that echoed a cookie (so a host that received our `RETRY`), or a source that later answered a `PATH_CHALLENGE`; or (b) to the *source of the packet that triggered a reply*, in which case the reply is a `RETRY`, `ERROR`, `ROOM`, `PATH_CHALLENGE` or one-byte `KEEPALIVE`, never larger than the packet it answers (at most 38 bytes against at least 64 for a handshake), and for unproven sources rate-limited per source IP and overall. There is no way to make the relay send a *data* datagram, or a token, to an address that has not proved it receives. A forged source can draw small replies (b) at its victim, within those rate limits; it cannot draw state or data.
- **Anti-reflection (UDP).** An attacker can spoof a victim's source address. So:
  - `CREATE_ROOM`/`JOIN_ROOM` datagrams must be **at least 64 bytes**; shorter ones get no reply and create no state. Every reply to an unauthenticated packet is at most 38 bytes, so the relay **never answers an unauthenticated packet with a larger one**. (`replyUnauth` also checks `reply <= request` itself.)
  - A handshake without a valid cookie draws only a 26-byte `RETRY`, and no state.
  - Packets that are not valid frames get no reply at all.
  - Replies to unauthenticated sources are rate-limited per source IP and overall (table above); a spoofing flood is mostly answered with silence. There are two overall allowances, so that junk of one kind cannot starve the other: `RETRY` and `PATH_CHALLENGE` (what a real client needs to start a handshake or to survive a NAT rebinding) share a high one (50000/s); `ERROR`s to unproven sources (wrong version, unknown token) have the ordinary one (5000/s). A `ROOM` or `ERROR` sent to a source that has just echoed a valid cookie is not charged to either: the source has proved it receives there, so it is not a reflection target. Total unauthenticated output is therefore bounded at roughly 55000 packets per second of at most 26 bytes each, about 11 Mbit/s.
  - **What a flood can still do.** *Per victim address:* someone who can forge a victim's address at more than a few dozen packets per second uses up that address's reply budget and keeps the victim from starting a handshake while the flood lasts. *Relay-wide:* to deny new handshakes and NAT migrations to **everyone**, an attacker must push the `RETRY`/`PATH_CHALLENGE` allowance over 50000/s. Because each source IP is limited to 40 replies a second first, that takes forged packets from at least ~1250 distinct source addresses (easy to forge from a network that does not filter spoofed sources), each of at least 64 bytes: more than 50000 packets/s, about 26 Mbit/s of UDP payload (about 37 Mbit/s on the wire). While it lasts, clients that are not yet in a room cannot get a `RETRY` and keep retransmitting until their handshake times out, and clients whose address changed cannot validate the new one. It does **not** affect sessions that already exist: `DATA`, `KEEPALIVE`, `LEAVE` and WebSocket traffic need no unauthenticated reply, and a client that already holds a cookie still gets its `ROOM`. No state remains afterwards. A flood of the other kind (wrong-version or unknown-token packets over 5000/s, about 3 Mbit/s) only silences those `ERROR`s, which affects no handshake. The allowances are `Config.GlobalVitalReplyPerSec`/`Burst` and `GlobalReplyPerSec`/`Burst`; the only complete defence against a sustained flood of this size is network-level (ingress filtering, a firewall or a provider scrubbing service in front of the UDP port), and WebSocket clients are unaffected by it.
  - Replies to *authenticated* frames (the one-byte `KEEPALIVE`) are smaller than their request, and need a valid token.
- **WebTransport.** Every QUIC connection first goes through QUIC's **Retry**: an Initial packet from a source that has not echoed a Retry token gets one small Retry packet (132 bytes against a client Initial of 1280 in the test) and the relay keeps no state, so a forged source address cannot draw a handshake, a certificate, state or data (`TestWebTransportFirstAnswerToAnUnvalidatedAddressIsASmallRetry`). The per-address and relay-wide connection caps are applied when the validated Initial is accepted, before the TLS handshake is built, and given back exactly once when the connection ends, whether or not it ever got a session. Everything the relay sends on a connection goes to the address that completed the QUIC handshake (QUIC's own path validation covers migration). The cost is one extra round trip on connection setup. Not covered by this document: QUIC's own stateless reset and version negotiation packets (RFC 9000 requires them to be smaller than their trigger; not checked in quic-go), its limits on the number of handshakes in flight, and Initial packets from a *real* address at a high rate (each draws one Retry); these are bounded by quic-go's queues, and the network-level defences named under Anti-reflection apply.
- **Tokens.** 128 random bits from `crypto/rand`, one per session. They travel in clear text over UDP, like every UDP game protocol without encryption: anyone who can read a client's traffic can impersonate it. A token is only ever sent to the address that proved, with a cookie, that it receives there, so an attacker who merely forges a source address never learns one. Use `wss://` for browsers; encryption of the UDP path is out of scope.
- **The nonce.** A repeat of a request's nonce is answered with that session's `ROOM`, token included, but only to the session's own address, so a guessed nonce is not enough to learn a token. Clients should still draw the nonce from a cryptographically secure source (vnet uses `getentropy()`, or `rand_s` on Windows, and **fails to create the context** rather than fall back to something weaker): a predictable nonce lets someone else claim it first.
- **Capacity per source.** One address (IPv6: one /64) can hold at most 128 members, 16 live rooms (counted until the room dies, not until its creator leaves), 128 open WebSockets and 32 WebSockets that have not entered a room yet; the handshake and reply rates are per the same key. An IPv6 /48 (a whole site; a /56 or /64 from the same provider block sits inside one) is limited to 4 times those numbers in total, so a subscriber with many /64s cannot take the relay's room budget (`-max-rooms`) by itself. Because sessions and rooms exist only for sources that echoed a cookie, forged handshakes use none of these budgets. Everyone behind one NAT shares them, including carrier-grade NAT: 128 members is the cap for all players behind one public address (flags below).
- **Relay-to-client frames carry no token.** The client accepts them only from the relay's address. An attacker who can forge the relay's source address *and* guess the client's port could inject `DATA` or end a session; this is inherent to unauthenticated UDP.
- **Brute forcing codes.** 30-bit codes with at most 1000 live rooms, plus the per-IP handshake limit, makes guessing impractical but not impossible for a patient attacker with many addresses. Rooms are meant for casual games, not secrets.
- **WebSocket and WebTransport.** `Origin` can be restricted (`-origins`, which applies to both); by default any origin may connect, because the relay has no ambient credentials to protect. Behind a reverse proxy, `-trust-forwarded-for` makes per-IP limits use the real client address, taken from the **rightmost** `X-Forwarded-For` entry (the one the proxy added; earlier entries are written by the client). Only enable it if the relay is reachable only through the proxy.

## Errors

| Reason | Name | When | vnet error (`vnet_status().error`) |
|---|---|---|---|
| 1 | `BAD_VERSION` | the request's `ver` is not supported; the reply's `ver` is the relay's | `VNET_EPROTO` |
| 2 | `ROOM_NOT_FOUND` | `JOIN_ROOM` for a code that is not a live room (or is malformed) | `VNET_ENOROOM` |
| 3 | `ROOM_FULL` | no free address in the room | `VNET_EROOMFULL` |
| 4 | `LIMIT` | room or session cap reached | `VNET_ELIMIT` |
| 5 | `RATE` | too many handshakes from this source | `VNET_ELIMIT` |
| 6 | `MALFORMED` | WebSocket: unusable first frame | `VNET_EPROTO` |
| 7 | `NO_SESSION` | unknown token | `VNET_ECLOSED` |
| 8 | `CLOSED` | the room or session ended; or a late duplicate handshake | `VNET_ECLOSED` |
| 9 | `NONCE_REUSED` | the nonce belongs to a different request | `VNET_EPROTO` |
| 10 | `TIMEOUT` | the session was dropped for inactivity | `VNET_ECLOSED` |
| 11 | `ALREADY_IN_ROOM` | WebSocket connection already has a session | `VNET_EPROTO` |

Other failures seen by a client: `VNET_ETIMEDOUT` (no answer, or the relay went silent), `VNET_ECONNREFUSED` (the WebSocket could not be opened, or the relay's host name did not resolve).

## Client guidance

The vnet backends implement this; another client should:

- generate a fresh nonce per attempt from a **cryptographically secure** source (see [Security properties](#security-properties)) and keep it across retries;
- over UDP, answer a `RETRY` (matching nonce, from the relay's address) at once by repeating the request with the cookie filled in, keep the cookie for later retransmissions, and replace it when a newer `RETRY` arrives;
- answer every `PATH_CHALLENGE` with a `PATH_RESPONSE` from the same socket;
- pad UDP handshakes to 64 bytes;
- over WebTransport: open one bidirectional stream, send every control frame on it with its 2-byte length, send `DATA` only as datagrams, and read control frames with a reassembling parser (a frame may arrive in pieces, several in one chunk); a length of 0 or over 64 is a protocol error;
- over WebTransport, if the browser's `datagrams.maxDatagramSize` is below 1109, **probe before declaring the session usable**: after `ROOM`, send a full-size `DATA` frame (1100 payload bytes) to your own address with both ports 0, repeat it every 250 ms, and treat the session as usable only when it comes back from yourself with the same bytes; give up after 1.5 s (vnet: `VNET_EMSGSIZE`, or fall back to WebSocket). The relay needs no support for this: delivering a member's datagram to its own address is ordinary routing, and no socket has port 0;
- accept `ROOM`/`ERROR` only if the nonce matches during the handshake, and frames only from the relay's address;
- send `KEEPALIVE` at the suggested interval and treat silence for three intervals as a lost session;
- drop `DATA` for ports it has not opened, and payloads over 1100 bytes;
- on `WebSocket` backpressure (`bufferedAmount` large), drop new datagrams instead of queueing stale ones.

## Versioning

- The version byte is in `CREATE_ROOM`/`JOIN_ROOM` and echoed in `ROOM`/`ERROR`. This relay speaks **version 1** only; any other value gets `ERROR(BAD_VERSION)` carrying the relay's version.
- `RETRY` (with the cookie field of UDP `CREATE_ROOM`/`JOIN_ROOM`) and `PATH_CHALLENGE`/`PATH_RESPONSE` are part of version 1 (it has not been released).
- Changes that keep old clients working (new optional frame types, new `ERROR` reasons, more trailing bytes in `ROOM`) stay at version 1: clients ignore frame types they do not know, and treat unknown error reasons as protocol errors. Anything else bumps the version. `VNET_MAX_DATAGRAM` (1100) is part of the ABI of every game and will not be raised.
- The WebTransport carriage (control stream with 2-byte lengths, `DATA` in datagrams) is part of version 1; it adds no frame types.
- Not in version 1, and left to later briefs: peer join/leave notifications, broadcast addresses, host migration, rejoin after a drop (Bit Riot brief 13), encryption of the UDP path.
