# Integrating vnet into a game loop

For the person wiring vnet into a game for the first time (Bit Riot's native lockstep and browser netplay, or a port of an existing net driver). The reference for each call is [`include/vnet.h`](../include/vnet.h); the relay's wire format is [`relay-protocol.md`](relay-protocol.md). This page is what is scattered between them: the order of calls, the states, the traps.

vnet gives you **unreliable, unordered datagrams of at most `VNET_MAX_DATAGRAM` (1100) bytes**. Everything below follows from three rules: nothing blocks (with two named exceptions), nothing runs unless you call `vnet_poll()`, and no datagram is ever guaranteed to arrive.

## The per-frame sequence

Once per game frame, in this order:

```c
vnet_poll(v);                                      /* 1. pump: I/O, impaired datagrams, keepalives */

vnet_status_info st;
int state = vnet_status(v, &st);                   /* 2. where are we? (see the state machine) */

if (state == VNET_STATUS_READY) {
    uint8_t buf[VNET_MAX_DATAGRAM];
    vnet_addr from;
    for (;;) {                                     /* 3. drain until EAGAIN */
        int n = vnet_recvfrom(v, sock, buf, sizeof buf, &from);
        if (n >= 0) handle_packet(&from, buf, (size_t)n);
        else if (n == VNET_EAGAIN || n == VNET_EIO) break;   /* nothing more / transient: next frame */
        /* VNET_EMSGSIZE: that datagram was consumed (a bug in your buffer size); carry on */
    }
}

game_logic();                                      /* 4. */
/* 5. sends: */
int rc = vnet_sendto(v, sock, pkt, len, &peer);
/* rc >= 0: accepted. VNET_EAGAIN / VNET_EIO: it was dropped; carry on. */
```

Points that have bitten other projects:

- **Poll in every state, including menus, loading screens and "waiting for players".** A relay context sends its keepalives from `vnet_poll()`; the relay drops a UDP member that has been silent for 15 s, and a lockstep peer's own timeout (about 10 s) is shorter than that. Impaired datagrams are released by `vnet_poll()` too.
- **Drain the socket every frame.** A receive queue that fills drops datagrams, like a real network: `rx_queue_len` (loopback and relay backends, default 128 per socket) and the kernel's buffer (UDP; vnet asks for 256 KiB, which holds several hundred small datagrams on a stock Linux, up to the system's `rmem_max`).
- **`VNET_EAGAIN` and `VNET_EIO` from `vnet_sendto()` are a dropped packet, on every backend**: the OS send buffer is full, the network is unreachable for a moment, the WebSocket has too much buffered, or too many WebTransport writes are in flight. A protocol that repeats what is unacknowledged (D7, and Bit Riot's lockstep) needs no special handling; never tear down a session for it. `VNET_EMSGSIZE` and `VNET_EINVAL` are bugs in the caller.
- A negative code you do not know is an error, but treat `VNET_EAGAIN` and `VNET_EIO` as transient (future versions may add codes).
- `vnet_recvfrom()` with a buffer smaller than the datagram returns `VNET_EMSGSIZE` and **consumes** the datagram. Always receive into a `VNET_MAX_DATAGRAM` buffer.
- A context and everything on it must be used from one thread at a time.

## The status state machine

Every backend answers `vnet_status()`; only relay backends ever leave `READY`'s neighbourhood. UDP and loopback are `READY` from the start, so one driver works on all of them.

| State | Meaning | `vnet_open` | `vnet_sendto` | `vnet_recvfrom` | `vnet_local_addr` |
|---|---|---|---|---|---|
| `CONNECTING` | relay: opening the transport, entering a room | works (ports are local) | `VNET_ENOTCONN`, nothing is queued | `VNET_EAGAIN` | `VNET_ENOTCONN` |
| `READY` | usable. Relay: in a room, `st.room` and `st.self` (10.0.0.N) are set | works | works | works | works |
| `FAILED` | final; `st.error` says why | works | `VNET_ENOTCONN` | `VNET_EAGAIN` | `VNET_ENOTCONN` |

`CONNECTING` becomes `READY` or `FAILED` and never goes back. `FAILED` is final.

Failure reasons: `VNET_ENOROOM` (no such room code), `VNET_EROOMFULL`, `VNET_ELIMIT` (a server or rate limit), `VNET_ETIMEDOUT` (no answer in `relay_timeout_ms`, or the relay went silent while you were polling), `VNET_ECONNREFUSED` (cannot reach the relay; for a WebSocket also a relay that is down), `VNET_ECLOSED` (the relay ended your session: it timed out, restarted, or the room closed), `VNET_EPROTO` (version mismatch or a misbehaving relay), `VNET_EMSGSIZE` (WebTransport cannot carry a full-size datagram). `st.transport` names what is in use ("udp", "loopback", "relay-udp", "relay-ws", "relay-wt").

### What to do on `FAILED`

Destroy the context and make a new one: `vnet_destroy(v)`, `vnet_create()`, `vnet_open()` for every port again, and re-resolve `room-host:PORT` (a new context usually gets a new address). A reconnecting peer is a **new** context with a new virtual address; old datagrams may still reach the old address's next owner, so identify peers by a token of the game's own, not by address, and put a session id in your packets. Show the reason (`vnet_strerror(st.error)`) and let the player retry. Do not retry in a tight loop: `VNET_ELIMIT` includes the relay's handshake rate limit.

A relay restart ends every room, so a match cannot be resumed through it.

### Showing the connection

While `CONNECTING` show "connecting"; give up yourself after a time that suits the UI (`relay_timeout_ms`, default 10 s, bounds the handshake). With `VNET_BACKEND_RELAY_AUTO` in a browser the WebTransport attempt can add up to `wt_timeout_ms` (3 s) before the WebSocket one starts: show `st.transport` once `READY`.

Lobby numbers from the status: `st.max_members` (the room's limit, so the lobby can refuse a seventh player with the right message), `st.relay_rtt_ms` (the latest `KEEPALIVE` round trip to the relay; -1 until the first answer, which comes one keepalive interval after `READY`, so start it early with a smaller `keepalive_ms` if the lobby shows it; 0 on UDP and loopback). The relay RTT is one leg: a peer-to-peer RTT through the relay is about the sum of the two peers' legs, and your game's own ping/pong measures it exactly. Use the relay figure only as a floor before any peer has answered.

## Main-loop skeletons

### SDL (native)

```c
while (running) {
    SDL_Event e; while (SDL_PollEvent(&e)) { /* ... */ }
    net_frame(&game);              /* the per-frame sequence above */
    update(&game);
    render(&game);
    /* vsync or SDL_Delay as usual; vnet adds no sleep of its own */
}
```

### A headless peer or dedicated server

Instead of `nanosleep(1 ms)`, sleep until something can have arrived:

```c
for (;;) {
    vnet_wait(v, 5);               /* UDP and relay-UDP; returns 1 early if a datagram may be waiting */
    net_frame(&game);              /* still poll and drain: wait receives nothing */
    tick_if_due(&game);
}
```

`vnet_wait(v, ms)` returns 1 (something may be ready; can be spurious), 0 (timeout, or a deadline of the context is due: an impaired datagram's release, a relay keepalive or handshake retransmission) or a negative code. **Poll after it either way.** It is the one call that blocks, by name; loopback and the browser backends return `VNET_ENOTSUP` (use `VNET_ENOTSUP` as "fall back to your own sleep"). It assumes the context's clock runs in real time.

### Emscripten (browser)

```c
static void frame(void *arg) { net_frame(arg); update(arg); render(arg); }
int main(void) {
    /* ... vnet_create() with VNET_BACKEND_RELAY_AUTO ... */
    emscripten_set_main_loop_arg(frame, &game, 0, 1);   /* requestAnimationFrame */
}
```

Nothing blocks in a browser: the connection is made by events between frames, and `vnet_poll()` moves what they queued into your sockets. Allocation: vnet itself allocates nothing in steady state, but Emscripten's WebSocket glue, vnet's WebTransport glue and the browser allocate for every message.

## Hidden browser tabs

A tab in the background stops `requestAnimationFrame` altogether and runs timers at about 1 Hz. Two facts make that survivable:

1. **The session survives.** The relay keeps a WebSocket or WebTransport session alive for as long as the connection answers its pings, and browsers answer those by themselves. vnet does not read a pause as a dead relay: if more than one keepalive interval passes between two `vnet_poll()` calls it restarts its silence and handshake clocks and sends a `KEEPALIVE` at once. A page that comes back after a minute is still in its room. (Natively the same logic covers a debugger or a sleeping laptop, but over UDP the relay itself drops a member that was silent for 15 s, so a longer pause ends in `VNET_ECLOSED` as soon as the first `KEEPALIVE` is answered.)
2. **Network events are not throttled, so poll from them, as well as from a timer.** Set `vnet_config.wake`; the WebSocket and WebTransport transports call it from the browser event that queues a frame or changes the transport's state:

```c
static void on_wake(void *user) {
    (void)user;
    /* Do NOT call vnet from here. Only arrange for a poll: */
    emscripten_async_call(poll_and_step, NULL, 0);    /* or set a flag, or postMessage */
}
cfg.wake = on_wake;
```

The callback is called from browser events only (never from inside a vnet call), at any time between `vnet_create()` and `vnet_destroy()` (never after); it must not call any vnet function and must not block, and it may be called many times for one poll (coalesce with a flag). Native backends never call it.

**The wake does not replace a timer.** Frames and state changes call it; vnet's own timers do not, because they are not events: a failed WebSocket connect is retried (with backoff) only inside `vnet_poll()`, the WebTransport-to-WebSocket fallback deadline, the handshake timeout and the keepalives likewise, and a connection that never delivers a frame never wakes you. A page that polls from wake-ups alone stays `CONNECTING` forever behind a relay that is slow to refuse. So keep a **timer-driven poll** (`emscripten_async_call` re-arming itself, `setTimeout`, about once a second is plenty) running alongside the wake, and let the wake poll *sooner*. Mind the throttling: a hidden tab's timers run at about 1 Hz, and after about five minutes hidden Chrome's intensive throttling can run them as rarely as once a minute (Bit Riot brief 12 has the details). That is enough for vnet (a gap of more than a keepalive interval is treated as sleep and the session survives, point 1), and it is why the wake matters: the game itself keeps stepping from the network events, which are not throttled.

In a WebSocket page, control frames overtake queued datagrams (they have a queue of their own so that a burst of datagrams cannot push them out): an `ERROR(CLOSED)` can be seen before datagrams that arrived earlier, which are then of no use because the session has ended. Under D7 nothing is owed to a lost datagram.

`examples/wake_probe.c` and `tests/browser/wake_browser.test.js` are a working page with no main loop at all, a 1 Hz safety timer and a wake that schedules each poll (the exchange itself is driven by the wake-ups; the timer is there for vnet's own timers), and a page whose loop is frozen.

Keeping a lockstep game stepping in a hidden tab (what to simulate when) is Bit Riot brief 12's business; this is the hook it needs.

## Setup details

- **Ports.** `vnet_open(v, 0)` picks an ephemeral port; `vnet_local_addr(v, s, &a)` tells you which. UDP sockets are bound to all local addresses, so tell peers `127.0.0.1:port` or your real address (the wildcard is what `vnet_local_addr` shows). Test harnesses should open port 0 and publish the result instead of fixing numbers.
- **One backend constant for every build.** Use `VNET_BACKEND_RELAY_AUTO` and fill both `relay` (the `wss://host/vnet` URL, used by browser builds) and `relay_udp` (`host:port`, used by native builds); no `#ifdef __EMSCRIPTEN__` is needed.
- **DNS.** `vnet_create()` with a relay *host name* resolves it with `getaddrinfo()` inside the call, which can freeze a frame for the resolver's timeout. Recommended: resolve once, at settings-load time or on a worker thread, with `vnet_resolve_host("relay.example.org:7777", VNET_FAMILY_ANY, &addr)`, turn the result into a numeric string with `vnet_addr_to_string()` and pass *that* as `relay_udp`. A numeric address never blocks. (`vnet_resolve()` itself is numeric-only and never blocks; the other blocking call is `vnet_wait()`.) A game connecting directly (`--join host:port`) can use `vnet_resolve_host()` the same way instead of its own lookup.
- **The clock.** `vnet_time(v)` is the context's clock in ms: the system monotonic clock, `vnet_config.clock` if you supplied one, or the loopback network's virtual clock. Do RTT and timeout arithmetic with it so the same code runs on virtual time in tests.
- **Direct UDP across the internet** has no hole punching or STUN: one side needs a forwarded port. The relay is the answer when neither has one.

## Testing

- **Loopback** (`VNET_BACKEND_LOOPBACK`): any number of peers in one thread, virtual time, exact repeatability. Drive it with `vnet_loopback_network_step(net, ms)`; a datagram becomes receivable when its *destination* polls.
- **Impairment** on any backend: `vnet_config.impair`, or `VNET_IMPAIR="lat=80,jit=20,loss=5,dup=1,reorder=3,burst=2,blen=5,seed=7"` in the environment (it replaces the configured value unless `ignore_env`). **Give every process its own `seed=`**: the default seed is 0, so otherwise all peers lose the same packets. A multi-process test should set it per process (`seed=<player number>`). It applies to what a context *sends*, never to handshakes or keepalives.
- **Change conditions mid-test** with `vnet_set_impair(v, &imp)`: 500 ms of `loss_pct = 100` and then `vnet_set_impair(v, NULL)`. Datagrams already delayed stay queued and are released on schedule. Call it again with the same `seed` to replay a pattern. **Burst loss** (`burst_pct`, `burst_len`; `burst=`, `blen=`) is a two-state model counted in packets: the thing a lockstep input delay is actually chosen against, which uniform loss at 5% or 20% does not show.
- A context is cheap: a test that wants a fresh network creates new contexts rather than reusing one after `FAILED`.

## Vendoring without CMake

For a build system that cannot use `vnet::vnet` (a GNU make project such as jfbuild/jfSW, say). vnet is 13 C files and one header with no dependencies:

- Add **all** of `src/*.c` to your build. Include paths: `-Ivnet/include -Ivnet/src`. Language: C11. (The browser-only files, `relay_ws.c` and `relay_wt.c`, compile to `VNET_ENOTSUP` stubs natively, and `relay_udp.c` and `udp.c` to stubs under Emscripten, so the list does not change per platform.)
- POSIX: define `_POSIX_C_SOURCE=200809L` for those files (`_DARWIN_C_SOURCE` on macOS). Windows: link `ws2_32` and define `_WIN32_WINNT=0x0601` and `WIN32_LEAN_AND_MEAN` for them. Emscripten: link `-lwebsocket.js` (no ports are needed).
- Your own sources include `vnet.h` only (valid C and C++).
- `cmake/banned.h` is a developer-build guard for vnet's own development; do not copy it.
- Pin the version you copied (`vnet_version()` at run time says which one is linked), and re-copy `src/` and `include/` together.

A `pkg-config` file and a single-file amalgamation are not provided.

## API stability

- From **0.3.0** on, 0.x releases are **additive only**: new functions, error codes, backends and config/status/impairment fields. New fields go at the **end** of `vnet_config`, `vnet_impair` and `vnet_status_info`, so code written for an older version, with its structs zeroed or `vnet_config_init`ed, keeps its meaning **when rebuilt**. Nothing is removed, renamed or reinterpreted. This is *source* compatibility, not binary: vnet is a static library whose structs grow (an old object file would hand a new `vnet_create()` a config that is too short, and a `vnet_status()` a struct that is too small), so rebuild your code against the header of the vnet you link, and never mix a new library with objects built against an old header. Relay protocol version 1 is frozen (see [Versioning](relay-protocol.md#versioning)).
- **1.0** comes after Bit Riot's native and browser netplay (briefs 07 and 12) have consumed the API.
- **`SameMajorVersion` caveat.** The installed CMake package declares compatibility `SameMajorVersion`, which makes `find_package(vnet 0.4)` accept any 0.x. That is more generous than semver's 0.x rule (any minor may break) and than this policy (additive), and it cannot tell you a release is *older* in a way that matters to you. **Pin an exact release tag** (`GIT_TAG v0.4.0`) and bump it on purpose.
- A shared-library build, if one is ever offered, would add a `size` field to `vnet_status_info` first; as a static library it needs none.
- **Releases**: 0.1.0 core and native UDP, 0.2.0 relay and WebSocket, 0.3.0 WebTransport, 0.4.0 this API polish.
