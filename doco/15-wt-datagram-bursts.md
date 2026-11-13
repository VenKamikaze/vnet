# 15: WebTransport drops datagrams sent in a burst

## Goal

A browser client on the WebTransport backend (`VNET_BACKEND_RELAY_WT`, or `RELAY_AUTO` once it has chosen WebTransport) must not drop a datagram because the game sent several in a row. Drops must happen only under real back-pressure from the browser or the network, as vnet's design intends (D7: unreliable, but not lossy for no reason). Memory must stay bounded when the connection really stalls.

This brief was written for a vnet session from the jfsw-browser project (Shadow Warrior in the browser on vnet `v0.4.0`), whose deployment validation found the problem on 13 Nov 2026. That session does not change vnet itself.

## Dependencies

14 (the current WebTransport backend, `v0.4.0`).

## What was seen (in jfsw-browser; check it)

- **The slow case.** Shadow Warrior's netcode is master/slave lockstep at 40 tics a second over vnet. A browser host with two or more guests over WebTransport ran at **about 25 of 40 tics a second**:
  - two browsers and a desktop guest: 24.8 to 26.8 in three games, at load average 2 to 4;
  - three browsers: 26.7.
- **The same games at full speed (40.0):**
  - over the WebSocket backend;
  - two players over WebTransport;
  - a native host with two browser guests over WebTransport.
- **The host's page dropped datagrams in its own send path.** `Module.vnetWT.s[id].drops` reached 951 on the host and 223 on a guest in one 1,200-tic game. The game's protocol resends lost packets, so every machine slowed down.
- **On a busier machine** (load average 7 to 13) it was much worse:
  - 8.9 and 11.1 tics a second, with 1,253 and 1,518 drops on the host;
  - in one game the guest gave up because the host stopped answering for 15 s;
  - the same game over WebSocket at a similar load ran at 27.2 tics a second, with no drops.
- **Not the test's set-up.** Pinning the relay's certificate by hash instead of using Chromium's `--webtransport-developer-mode` made no difference (25.6).
- **Rates involved.** Measured natively, a host sends about 68 datagrams a second in a 2-player game, 130 with 3 players and 188 with 4. A guest sends about 67, all to the host. These are small rates.
- **Not yet checked on a real network**: the measurements are in a container, through a local relay.

## The likely cause (a hypothesis; verify it first)

`src/relay_wt.c`, `vnet_wt_send_datagram` (about line 223):

```js
if (s.dgInFlight >= in_flight_max) { s.drops++; return 1; }   // in_flight_max = JS_MAX_DATAGRAMS_IN_FLIGHT = 16
s.dgInFlight++;
s.dw.write(HEAPU8.slice(p, p + n)).then(function () { s.dgInFlight--; }, function () { s.dgInFlight--; });
```

- **The counter counts writes, not back-pressure.** `dgInFlight` is decremented only when a `write()` promise settles. That happens in a later microtask or task, never during the same synchronous run of C code. So `JS_MAX_DATAGRAMS_IN_FLIGHT` limits the datagrams sent **in one JavaScript task**, not the ones the browser has failed to send.
- **Why a game hits it.** A game that runs one simulation step per animation frame (Emscripten, ASYNCIFY here) sends all of a frame's datagrams in one task: to every peer, plus acks and resends.
- **Why only a browser host.** A browser host sends to several peers per step, and more steps per frame when the frame rate drops. It passes 16 easily. A 2-player host, or a guest (which sends only to the host), stays under it.
- **Why load makes it worse.** Every drop causes a resend, so the bursts get bigger: a feedback loop. That fits the much worse numbers under load.

Check this first. Count the datagrams sent per JavaScript task, and see whether the drops come from bursts within one task, or whether the writes really stay unresolved across tasks, which would be real browser back-pressure (QUIC congestion control or pacing).

## In scope

1. **Find the cause,** with measurements in Chromium: per-task burst sizes, how long writes stay unsettled, and `writer.desiredSize`. Record what you find in the Outcome.
2. **Fix the send path** so that a burst within one task is not dropped, while real back-pressure still drops rather than queuing without limit.
   - **Options to weigh:**
     - the stream's own back-pressure, through `writer.desiredSize` or `writer.ready`, perhaps with `datagrams.outgoingHighWaterMark`;
     - `datagrams.outgoingMaxAge`;
     - a limit on bytes or datagrams queued in the browser, measured where it means something;
     - a much higher cap.

     Say which you chose and why, with what Chromium does today (and Firefox if you can test it: Firefox supports WebTransport, and some players use it).
   - **Keep the design rule:** vnet is unreliable, and a stale datagram is worth less than a new one. So under real back-pressure, dropping is still right. Check that the control-frame path (`JS_MAX_CONTROL_IN_FLIGHT`) cannot have the same burst problem. If it can, fix that too; a stuck control stream ends the session.
3. **Report drops.**
   - If `vnet_stats` (or the equivalent) does not already count the browser-side WebTransport drops, consider counting them, so that a game can see them without reading `Module.vnetWT`.
   - Keep the API compatible (14's API polish).
4. **Release.** After the merge, tag a release (`v0.4.1`, or what the versioning rule says), since consumers pin a tag. The user decides on merging into `main`.

## Out of scope

- The WebSocket and UDP backends, unless the same burst bug is found there. Check `relay_ws.c`'s send path briefly and say so.
- The relay (Go side), unless the investigation shows the relay is involved.
- Any change in jfsw-browser or Bit Riot. They pick up the release themselves.

## Acceptance criteria

1. **Bursts get through.** A browser client that sends a burst of, say, 64 datagrams in one task over WebTransport, to a few peers through the relay, loses none of them on a loopback relay with no impairment. Today it loses everything past the 16th.
2. **Bounded under real back-pressure.** When the connection really stalls (the relay stops reading, or the network is blackholed after the session is up), the client's memory and queue stay bounded, and it drops rather than queuing forever. The session still fails the way it does today when the relay is gone.
3. **No regression:** every existing test passes, native and browser (`tests/browser/wt_browser.test.js`, the relay's Go tests, the CTest suite, CI).
4. **Measured with a game-like load:** a sender that, every 25 ms, sends one datagram to each of 3 or 7 peers, plus a few extra (acks and resends), in one task. It shows no drops at full rate, where today's code drops. Report the numbers before and after.
5. **Optional, if you have jfsw-browser at hand** (see Notes): its WebTransport games with a browser host and two guests run at 40 tics a second.

## Tests

1. **A browser test that fails on today's code:** a burst of more than 16 datagrams in one task over WebTransport, through a real relay, with every datagram counted at the receivers. Add it to `tests/browser/` next to `wt_browser.test.js`.
2. **A back-pressure test:** a stalled connection keeps memory bounded and drops. Use a fake `datagrams.writable` whose writes never settle (like the test's `STRICT_DATAGRAMS` wrapper), a blackholed relay, or both.
3. **Mutation:** restore the old counter, and test 1 fails.

## Notes

- **Branches:** `vnet/15-wt-datagram-bursts` from `claude` (`579a410` when this was written; `main`'s `12c8cec` has the same tree), merged back with `--no-ff` (doco/README.md, "How to work a brief"). The brief itself is on `docs/15-wt-datagram-bursts`, from `claude`.
- **The number 15** follows vnet's last brief (14). Bit Riot's master plan has no 15 yet. Renumber if the user wants.
- **To reproduce in jfsw-browser** (`VenKamikaze/jfsw-browser`, branch `claude`; it vendors vnet as `ext/vnet` at `v0.4.0`):
  - `tests/deploy/check-deploy.sh` (on branch `browser/06-deployment-guide` until it merges) runs a browser host, a browser guest and a desktop guest over WebTransport through a relay with a certificate, and prints the tic rate and the host's drops. It needs root and Apache.
  - Simpler: its `tests/web/netplay.js` has WebTransport scenarios with a local relay (`wt`, a native host). A browser-host variant with two guests shows the slowdown.
  - See its `doco/browser/bugs.md` (the WebTransport slowdown entry, numbered L25 on the deployment branch and to be renumbered when it merges) and `doco/browser/deploy.md` (its "known problem" note).
- **Bit Riot** uses the same backend. Check whether its browser host's per-frame sends can pass 16, for example in a 4-player lockstep game with resends, and say so in the Outcome.

## Outcome

Implemented on `claude/webtransport-datagram-bursts-u1oaqa` (used in place of `vnet/15-wt-datagram-bursts`, the one branch the cloud session could push). CMake version and `VNET_VERSION_*` are 0.4.1; the user tags `v0.4.1` after the merge (the cloud session cannot push tags): `git tag -a v0.4.1 -m "vnet 0.4.1: WebTransport datagram bursts" <merge commit> && git push origin v0.4.1`. Chromium 153.0.8010.12 under Playwright, a loopback relay, no impairment. **Firefox was not tested**: Playwright's Firefox is not installed here (`firefox.launch()` fails, and `playwright install` is not allowed).

### The cause is confirmed

`tests/browser/wt_datagram_probe.js` (a measurement tool, no vnet code in it) talks to a real relay from a Chromium page:

- **No write settles inside the task that made it.** A burst of 16, 64 or 256 `write()` calls in one synchronous run: 0 settled after the microtask checkpoint, and at most 1 by the next task. `writer.desiredSize` goes 0, -15 ... -(N-1): it is just `highWaterMark - unsettled writes`, so it counts the burst exactly as `dgInFlight` did and says nothing about back-pressure. A 16-write counter therefore limits the datagrams sent **in one task**, as the brief guessed.
- **They do settle quickly:** all 16 within 3.6 ms, 64 within 9 ms, 256 within 44 ms.
- **A game-like rhythm leaves nothing behind:** 10 datagrams every 25 ms for 80 steps: 0 unsettled at the start of any later task.
- **Chromium defaults:** `datagrams.outgoingHighWaterMark` 1, `outgoingMaxAge` `null`, `maxDatagramSize` 1024. Both are settable (to 64 and 100), but **`outgoingMaxAge` has no effect in Chromium 153**: with the relay frozen (SIGSTOP) and `outgoingMaxAge = 200`, writes stay pending exactly as without it.
- **Real back-pressure** (relay frozen): the first ~375 of 2000 writes settle (buffers and congestion window), the rest stay pending for good; 1620 unsettled after 5 s and growing, `desiredSize` -1619. **Chromium queues without limit**, so vnet needs a bound of its own.

### The option chosen

A datagram is dropped (`VNET_EAGAIN`, `s.drops`, `vnet_stats.tx_dropped`) when **the oldest unsettled write is older than 250 ms** (`JS_DATAGRAM_STALL_MS`: the connection is stalled, and what is queued is stale; normal writes settle in milliseconds) **or when 512 are queued** (`JS_MAX_DATAGRAMS_IN_FLIGHT`, a memory bound: at most about 560 KB of copies, and 512 settle in about 90 ms, far below the stall age). `s.dgT` holds the issue time of every unsettled write.

Why not the others:

- *`desiredSize` / `ready` / `outgoingHighWaterMark`*: `desiredSize` is `hwm - unsettled`, the same number as the old counter; raising the mark only moves the burst limit.
- *`outgoingMaxAge`*: ignored by Chromium 153, and unknown for Firefox and Safari.
- *A much higher cap alone*: bounds memory but not staleness; a stalled connection would hold up to the cap of stale datagrams and send them all when it clears.
- *Age alone*: no memory bound if a task sends a huge burst; so both.

Firefox (not run): it implements the same `WebTransportDatagramDuplexStream` attributes and write promises, so the stream-level behaviour should be the same (a write stays unsettled until the datagram is queued or sent). If its writes settled only when sent, a 256 burst would take longer than in Chromium but stay far below the 512 cap and the 250 ms age.

**The control path** (`JS_MAX_CONTROL_IN_FLIGHT` = 64) was checked and left alone. It carries only handshake requests, timed keepalives and a close: a few frames, on timers, never a burst, and a client cannot send 64 in one task through any API. Counting unsettled writes there is right, because a control write that stays unsettled means a stuck stream, which ends the session.

**`relay_ws.c`** has no such bug: it limits on `WebSocket.bufferedAmount` (bytes, 64 KiB), which is exactly the browser's queue and is not tied to promises. The burst test passes over WebSocket too (3 x 22 datagrams in one task, 198 of 198). Note the limit is in bytes: a burst of more than about 59 datagrams of the maximum size (1109 bytes) in one task would be dropped there, which is back-pressure by definition, not this bug.

### Drops are reported

`vnet_stats` gained `tx_dropped` at the end (source compatible, 0.4.1): every `vnet_sendto()` that the backend refused with `VNET_EAGAIN` (WebTransport stalled, WebSocket `bufferedAmount` over 64 KiB, a full UDP socket buffer). `Module.vnetWT.s[id].drops` still counts them in the page. The relay also counts only what reaches it; it saw no drops and no rate limiting in the runs.

### Numbers (AC4)

`examples/burst_probe.c` as a browser sender (one step = one task) and native receivers that count distinct datagrams. 150 to 160 ticks, one every 25 ms; "extra" = acks or resends sent in the same step.

| scenario | before: accepted of attempted, lost | after |
|---|---|---|
| burst 3 peers x 22 in one task, 3 times (198) | 45 of 198 (153 dropped); one peer got 45, the others 0 | 198 of 198 |
| one burst of 3 x 85 = 255 | 15 of 255 | 255 of 255 |
| game, 3 peers + 3 extra, one tic per task (6 per task) | 960 of 960 | 960 of 960 |
| game, 7 peers + 3 extra, one tic per task (10 per task) | 1600 of 1600 | 1600 of 1600 |
| game, 3 peers + 3 extra, every 8th step 3 tics in one task (18 per task) | 1157 of 1200 (43 dropped) | 1200 of 1200 |
| game, 7 peers + 3 extra, every 8th step 3 tics in one task (30 per task) | 1716 of 2000 (284 dropped) | 2000 of 2000 |

So a steady 40 tics a second to 3 or 7 peers with a few extras stays under 16 per task and did not drop even before; the drops need more than 16 sends in one task (several tics when a frame falls behind, a resend burst, more peers or extras). Every drop makes the game resend, which makes the next burst bigger: the feedback the brief describes. A first version of the 7-peer scenario ran every step with 3 tics (1200 datagrams a second); the **relay** rate-limits a client at 600 a second sustained (`-client-pps`), so that is not a fair test: the final scenario keeps the average at about 500. The test also checks that the relay rate limited and dropped nothing. Not checked: jfsw-browser itself (AC5), and a real network.

### Back-pressure and the mutation (tests 2 and 3)

- A datagram writer whose writes never settle (the page wraps `WebTransport`): of 2560 sends in 40 bursts of 64, 509 were accepted, the page was given 512 writes, 2051 got `VNET_EAGAIN`, the queue held 512, and the drop counts agree. Before: 15 accepted.
- A relay frozen with SIGSTOP while the sender keeps sending (10 per 25 ms): no drops while it reads; drops start once it stops; the queue stays at about 100 writes (the age rule, not the cap); the session is still up while the relay is only frozen, and **fails when the relay is killed, as today** (`session closed by the relay`, 0.6 s).
- **Mutation 1:** with the old limit (cap of 16, no age rule) 6 checks fail in `wt_burst.test.js` (the three burst checks, the 255 burst and both catch-up game loads). With the whole old code restored the same checks fail (7 with the queue check). **Mutation 2:** no cap and no age rule (unbounded): 8 checks fail (queue size, drops counted, drops once the relay stops reading). Both restored; not committed.

### Bit Riot

Read-only, on its `claude` branch (`81e8dbc`). The host's `LockstepSession::hostSend()` runs on each `pump()` and sends, per present peer, one `TICKS` when there is something new or unacknowledged (resent after 40 ms) and an empty `TICKS` as a keepalive, plus a `LOBBY` broadcast to every peer now and then, plus the catch-up window for each late joiner (`CATCHUP_WINDOW_PACKETS` = 8 datagrams). `MAX_PEERS` is 6, so up to 5 peers. A 4-player browser host (3 peers) sends about 3 to 9 in a pump in steady state, **under 16**, but it can **pass 16** in one task when a frame runs several pumps (hitch, extra ticks), when resends coincide with new ticks and the lobby broadcast, or a late joiner is being caught up (up to 8 more); a 6-player host or a mid-join reaches it more easily. I did not run Bit Riot. With 0.4.1 none of these is dropped by vnet.

### Deviations and follow-ups

- Branch name (see the top). CTest (configured like CI's Linux job, 275 tests, including `relay_go_tests`): all pass, except that this container's kernel has no IPv6 (no `/proc/sys/net/ipv6`, no `::1`), so `UdpFamily.Ipv6LoopbackRoundTrip` is skipped, and with `VNET_REQUIRE_IPV6=1` it and `UdpFamily.DualStackOrFallbackReachesIpv4PeersAndReportsIpv4Senders` fail for that reason alone. CI creates `::1` first.
- New: `examples/burst_probe.c` (browser sender, native receivers), `tests/browser/wt_burst.test.js` (run by `scripts/test-browser.sh`), the measurement tool, and `?arg=` pass-through in `examples/shell.html`.
- Follow-ups: try jfsw-browser at 0.4.1 (AC5); Firefox and Safari by hand (the cap and age rule are browser independent, but their write timing was not measured); a relay-side per-client limit of 600 datagrams a second is worth knowing about for 7-player hosts with resend storms (not a bug).
