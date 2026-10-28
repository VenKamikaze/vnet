# vnet relay protocol, version 1

The relay puts clients into **rooms** and forwards datagrams between the members of a room. This document is the wire format. The server is `relay/` (Go), the client is `src/relay_client.c` (C), and the browser transport is `src/relay_ws.c`. Keep the three in step; the constants are in `relay/protocol.go` and `src/relay_proto.h`.

- [Overview](#overview)
- [Frames](#frames)
- [Handshake and idempotent retries](#handshake-and-idempotent-retries)
- [Rooms and virtual addresses](#rooms-and-virtual-addresses)
- [Sessions, keepalive and timeouts](#sessions-keepalive-and-timeouts)
- [Limits](#limits)
- [Security properties](#security-properties)
- [Errors](#errors)
- [Client guidance](#client-guidance)
- [Versioning](#versioning)

## Overview

- One protocol, two transports. Both carry **whole frames**; a frame never spans datagrams or WebSocket messages.
  - **UDP:** one frame per datagram, to the relay's UDP port. Unreliable, so the client retries the handshake, and every frame after the handshake carries the session **token**.
  - **WebSocket:** one frame per **binary** message on `/vnet`. Reliable and ordered, so no retries, no padding and no token (the connection is the identity). Text messages are not part of the protocol.
- All integers are **big-endian**. A frame is a 1-byte type followed by its fields.
- The relay hands each member a virtual IPv4 address `10.0.0.N` and forwards `DATA` between addresses **of the same room**. Nothing else is ever a destination: the relay is not a proxy.
- vnet carries unreliable datagrams only (D7). The relay does not retransmit, order or acknowledge anything. Over WebSocket, TCP happens to deliver in order; a game must not rely on that.

## Frames

`token` is 16 bytes, `nonce` 8 bytes, `code` 6 ASCII bytes, `vaddr` 4 bytes (`10 0 0 N`). `ver` is the protocol version, currently **1**.

### Client to relay

| Type | Name | UDP layout | WebSocket layout |
|---|---|---|---|
| `0x01` | `CREATE_ROOM` | `01 ver nonce[8]` + zero padding to **at least 64 bytes** | `01 ver nonce[8]` |
| `0x02` | `JOIN_ROOM` | `02 ver nonce[8] code[6]` + zero padding to **at least 64 bytes** | `02 ver nonce[8] code[6]` |
| `0x03` | `LEAVE` | `03 token[16]` | `03` (the relay then closes the connection) |
| `0x04` | `KEEPALIVE` | `04 token[16]` | `04` |
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
| `0x10` | `DATA` | `10 src_vaddr[4] dst_port[2] src_port[2] payload` |

- `ROOM` answers `CREATE_ROOM` and `JOIN_ROOM`. `nonce` echoes the request; `vaddr` is the member's address; `token` is the session token (WebSocket clients may ignore it); `code` is the room code; `max_members` is the room's member cap; `keepalive_s` is the interval the relay suggests for `KEEPALIVE` frames.
- `ERROR` carries the nonce of the request it answers. Errors that are not about a request (`NO_SESSION`, `CLOSED`, `TIMEOUT` on an established session) carry the session's nonce, or all zeros if the relay does not know the session.
- `DATA` has the same layout in both directions, with the address field meaning **destination** when a client sends it and **source** when the relay delivers it. The relay fills in the source from the sender's membership; a client cannot choose it. `src_port` and `dst_port` are opaque to the relay: they are the game's vnet ports, carried so that the receiving vnet context can hand the datagram to the right local socket.
- Clients **ignore frame types they do not know** (forward compatibility).

## Handshake and idempotent retries

1. The client picks a random 8-byte **nonce** once and sends `CREATE_ROOM` (to make a room) or `JOIN_ROOM` (to enter one).
2. The relay answers `ROOM` or `ERROR`.
3. Over UDP, either datagram can be lost, so the client **repeats the identical request, nonce included**, until it gets an answer. The relay treats the nonce as an idempotency key:
   - a repeat of a request that already succeeded returns the same `ROOM` (same token, same address) and creates nothing, so a retry never makes a second room or a second member;
   - a repeat from a **different source address** is the same session, and moves it there (a NAT mapping changed);
   - a nonce that was used for a *different* request (CREATE vs JOIN, or another code) gets `ERROR(NONCE_REUSED)`;
   - when a session ends, its nonce is remembered for 60 s (`NonceTTL`) so that a late, reordered duplicate of the handshake gets `ERROR(CLOSED)` instead of resurrecting a room.
4. The vnet client sends the first request at once, then retries after 250 ms, 500 ms more, then every 1000 ms, and gives up (`VNET_ETIMEDOUT`) after `relay_timeout_ms` (default 10 s).
5. Over WebSocket the handshake is sent once, when the connection opens. Repeating the same request on the same connection returns the same `ROOM`; a different request on a connection that already has a session gets `ERROR(ALREADY_IN_ROOM)`; the same nonce on another connection gets `ERROR(NONCE_REUSED)`.
6. A WebSocket connection must send `CREATE_ROOM` or `JOIN_ROOM` within 10 s (`HandshakeTimeout`) and as its first frame; anything else gets `ERROR(MALFORMED)` and the connection is closed.

`ERROR` answers over WebSocket leave the connection open (except as above), so a client may try another code. Failed attempts count against the per-source handshake limit.

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

- A **session** is one member of one room. Over UDP it is identified by its **token**, not by its source address: every authenticated frame is accepted from any address, and the relay then sends to the address the last such frame came from. A NAT rebinding therefore does not break a session. Over WebSocket the session lives as long as the connection.
- `KEEPALIVE`: the client sends one about every 5 s (`keepalive_s` from `ROOM`); the relay answers with a one-byte `KEEPALIVE`. This keeps NAT mappings open, keeps the session alive, and lets the client notice a dead relay.
- **Relay side:** a session with no authenticated frame for 15 s (`SessionTimeout`; WebSocket pongs count) is dropped, and a UDP member is told with `ERROR(TIMEOUT)`. The relay pings WebSocket clients every 5 s; browsers answer pings by themselves, even in a background tab whose timers are throttled, so a hidden page keeps its session as long as the connection is up.
- **Client side:** the vnet client declares the session lost (`VNET_ETIMEDOUT`) after three keepalive intervals without any frame from the relay, and `VNET_ECLOSED` on `ERROR(NO_SESSION | CLOSED | TIMEOUT)` or when the WebSocket closes.
- `ERROR(NO_SESSION)` answers an authenticated-type frame (`KEEPALIVE`, `LEAVE`, `DATA`) whose token is unknown (expired, never issued, the relay restarted, or a WebSocket session's token used over UDP).
- `LEAVE` is best effort over UDP (a lost `LEAVE` is just the 15 s timeout).

## Limits

All configurable; the defaults are in `relay/config.go`, the flags in `relay/cmd/vnet-relay`.

| Limit | Default | Effect when exceeded |
|---|---|---|
| members per room | 8 | `ERROR(ROOM_FULL)` |
| rooms | 1000 | `ERROR(LIMIT)` on `CREATE_ROOM` |
| members (and open WebSockets) | 20000 | `ERROR(LIMIT)`; WebSocket upgrade refused with HTTP 503 |
| payload | 1100 bytes | frame dropped |
| per-client packets | 600/s, burst 1200 | frame dropped, counted (`rate_limited`) |
| per-client bytes (whole frames) | 600000/s, burst 1200000 | frame dropped |
| handshakes per source IP | 20/s, burst 50 | `ERROR(RATE)` if a reply is allowed, else silence |
| replies to unauthenticated packets, per source IP | 20/s, burst 50 | no reply |
| replies to unauthenticated packets, overall | 5000/s | no reply |
| WebSocket send queue | 128 frames | frames to that client dropped (`ws_queue_drops`) |
| WebSocket message size | 1125 bytes | connection closed |

Limited frames are *dropped*, not queued: vnet is unreliable, and queueing behind a slow link only adds latency.

## Security properties

The relay is a router for the members of a room, not a proxy and not a gateway to the internet.

- **No forwarding to non-members.** A `DATA` frame is delivered only to a current member of the sender's room, found by the destination's last address byte *inside the sender's room*. There is no code path from a frame to an arbitrary socket address.
- **Anti-reflection (UDP).** An attacker can spoof a victim's source address. So:
  - `CREATE_ROOM`/`JOIN_ROOM` datagrams must be **at least 64 bytes**; shorter ones get no reply and create no state. Every reply to an unauthenticated packet is at most 38 bytes, so the relay **never answers an unauthenticated packet with a larger one**. (`replyUnauth` also checks `reply <= request` itself.)
  - Packets that are not valid frames get no reply at all.
  - Replies to unauthenticated sources are rate-limited per source IP and overall (table above); a spoofing flood is mostly answered with silence.
  - Replies to *authenticated* frames (the one-byte `KEEPALIVE`) are smaller than their request, and need a valid token.
- **Tokens.** 128 random bits from `crypto/rand`, one per session. They travel in clear text over UDP, like every UDP game protocol without encryption: anyone who can read a client's traffic can impersonate it. Use `wss://` for browsers; encryption of the UDP path is out of scope.
- **Relay-to-client frames carry no token.** The client accepts them only from the relay's address. An attacker who can forge the relay's source address *and* guess the client's port could inject `DATA` or end a session; this is inherent to unauthenticated UDP.
- **Brute forcing codes.** 30-bit codes with at most 1000 live rooms, plus the per-IP handshake limit, makes guessing impractical but not impossible for a patient attacker with many addresses. Rooms are meant for casual games, not secrets.
- **WebSocket.** `Origin` can be restricted (`-origins`); by default any origin may connect, because the relay has no ambient credentials to protect. Behind a reverse proxy, `-trust-forwarded-for` makes per-IP limits use the real client address (only enable it if the relay is reachable only through the proxy, and the proxy sets the header itself).

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

- generate a fresh random nonce per attempt and keep it across retries;
- pad UDP handshakes to 64 bytes;
- accept `ROOM`/`ERROR` only if the nonce matches during the handshake, and frames only from the relay's address;
- send `KEEPALIVE` at the suggested interval and treat silence for three intervals as a lost session;
- drop `DATA` for ports it has not opened, and payloads over 1100 bytes;
- on `WebSocket` backpressure (`bufferedAmount` large), drop new datagrams instead of queueing stale ones.

## Versioning

- The version byte is in `CREATE_ROOM`/`JOIN_ROOM` and echoed in `ROOM`/`ERROR`. This relay speaks **version 1** only; any other value gets `ERROR(BAD_VERSION)` carrying the relay's version.
- Changes that keep old clients working (new optional frame types, new `ERROR` reasons, more trailing bytes in `ROOM`) stay at version 1: clients ignore frame types they do not know, and treat unknown error reasons as protocol errors. Anything else bumps the version. `VNET_MAX_DATAGRAM` (1100) is part of the ABI of every game and will not be raised.
- Not in version 1, and left to later briefs: peer join/leave notifications, broadcast addresses, host migration, rejoin after a drop (Bit Riot brief 13), encryption, a WebTransport transport (vnet brief 11, which carries the same frames: control frames on a stream, `DATA` as datagrams).
