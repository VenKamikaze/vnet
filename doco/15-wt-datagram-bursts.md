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
4. **Release.** After the merge, tag a release (`v0.5.0`: the user chose a minor bump, as every earlier release that added an API field took one), since consumers pin a tag. The user decides on merging into `main`.

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

Implemented on `claude/webtransport-datagram-bursts-u1oaqa` (used in place of `vnet/15-wt-datagram-bursts`, the one branch the cloud session could push). CMake version and `VNET_VERSION_*` are **0.5.0** (the user chose a minor bump: adding `vnet_stats.tx_dropped` is an API addition, and every earlier addition took a minor); the user tags `v0.5.0` after the merge (the cloud session cannot push tags): `git tag -a v0.5.0 -m "vnet 0.5.0: WebTransport datagram bursts" <merge commit> && git push origin v0.5.0`. A review round followed the first implementation; its findings are folded into the sections below. Chromium 153.0.8010.12 under Playwright, a loopback relay, no impairment. **Firefox was not tested**: Playwright's Firefox is not installed here (`firefox.launch()` fails, and `playwright install` is not allowed).

### The cause is confirmed

`tests/browser/wt_datagram_probe.js` (a measurement tool, no vnet code in it) talks to a real relay from a Chromium page:

- **No write settles inside the task that made it.** A burst of 16, 64 or 256 `write()` calls in one synchronous run: 0 settled after the microtask checkpoint, and at most 1 by the next task. `writer.desiredSize` goes 0, -15 ... -(N-1): it is just `highWaterMark - unsettled writes`, so it counts the burst exactly as `dgInFlight` did and says nothing about back-pressure. A 16-write counter therefore limits the datagrams sent **in one task**, as the brief guessed.
- **They do settle quickly:** all 16 within 3.6 ms, 64 within 9 ms, 256 within 44 ms.
- **A game-like rhythm leaves nothing behind:** 10 datagrams every 25 ms for 80 steps: 0 unsettled at the start of any later task.
- **Chromium defaults:** `datagrams.outgoingHighWaterMark` 1, `outgoingMaxAge` `null`, `maxDatagramSize` 1024. Both are settable (to 64 and 100), but **`outgoingMaxAge` has no effect in Chromium 153**: with the relay frozen (SIGSTOP) and `outgoingMaxAge = 200`, writes stay pending exactly as without it.
- **Real back-pressure** (relay frozen): the first ~375 of 2000 writes settle (buffers and congestion window), the rest stay pending for good; 1620 unsettled after 5 s and growing, `desiredSize` -1619. **Chromium queues without limit**, so vnet needs a bound of its own.

### The option chosen

A datagram is dropped (`VNET_EAGAIN`, `s.drops`, `vnet_stats.tx_dropped`) when **512 are queued** (`JS_MAX_DATAGRAMS_IN_FLIGHT`, a memory bound: at most about 560 KB of copies) **or when the oldest unsettled write is stale**: it is older than 250 ms (`JS_DATAGRAM_STALL_MS`; normal writes settle in milliseconds), **counted from the first turn of the event loop after it was made**, **and** the event loop has turned at least 3 times since (`JS_DATAGRAM_STALL_TURNS`). Turns are counted by a `MessageChannel` ping posted from the send path, one outstanding at a time (so nothing runs when nothing is sent); a `setTimeout` would be throttled in a hidden tab, a message is not. (Fix round 3 added a first step to the stall rule: the first stall raises Chromium's own queue depth instead of refusing; see below.)

**Why the turn rule (found in review).** The first version had the age alone, measured from the `write()` call. Chromium's datagram sink takes one write at a time, so after a long task the queue drains one write per main-thread round trip, and the game's next timer task can run before the settle tasks. Age then includes time the page itself could not settle anything. Measured in Chromium 153 against a loopback relay: send 10, busy-wait 300 ms in the same task, send 10 (3 rounds): 30 of 60 accepted; a game load (3 peers + 3 extras every 25 ms) while another page task hogs the main thread 300 ms every 700 ms or 400 ms every 1000 ms: 6 and 12 drops, where 0.4.0 had none. A drop-free rule has to ignore time spent in a task that is still running or that just ended. Options weighed:

- *Age from issue time, as before* (rejected): the regression above.
- *Age from the first turn after issue, plus 3 turns* (chosen): the same-task case has no turn at all, so nothing is dropped; a hog between game steps gives the settle tasks the chance to run before 3 more turns accumulate; and the age is still kept per head write, so under a frozen relay (Chromium still settles about one write per 0.5 s) a head that is already old when it becomes head is stale at once, and the queue stays at about 100.
- *Time since the head changed* (rejected, measured): each settle under the frozen relay restarts the clock, and the queue grew to 486.
- *A `setTimeout` turn counter* (rejected): throttled in hidden tabs.

**Hidden tabs.** Playwright's headless Chromium keeps `document.visibilityState` at `visible` however many pages are opened, so a hidden tab could not be made; what was run is a sender with a 1 Hz game loop (the cadence of a throttled tab) sending 18 datagrams in a step: 108 of 108. By reasoning: message events are not timer-throttled, so turns are counted; in a throttled tab the game itself sends about once a second, so a real stall is noticed at the third send (3 s) instead of 250 ms, and the cap bounds what is queued meanwhile. Brief 14's wake callback (`cfg.wake`) is unaffected: it runs from the page's own promise handlers, not this path.

**What the rule does not cover.** Chromium itself buffers about 375 more already-settled datagrams under a stall (SIGSTOP of the relay): bounded by Chromium, but stale in a way this rule cannot see. (The first version of this section said a page that is saturated for a second or more "is failing to keep up". The third fix round showed that wrong: see the next section.)

### Fix round 3: the browser's own queue is one datagram deep (`outgoingHighWaterMark`)

**Finding (re-review, then re-measured here).** Chromium's datagram sink keeps `datagrams.outgoingHighWaterMark` writes (default **1**) in its own queue, and each write settles only after a round trip of the page's main thread. On a page whose main thread is busy all the time, about **one datagram leaves per task** and the rest wait *in the browser*: measured with a receiver that prints arrival times, arrival time equals settle time (settle minus arrival: median 0.1 ms; call to arrival: median 1.2 s). So the 250 ms stall rule was not misreading late callbacks: the datagrams were really late, because of the default queue depth. A game that never idles (a frame of 112 ms, 6 datagrams before and 6 after the work, about 9 frames a second, the jfsw-like case of the brief) had **396 of 480 refused** on 0.5.0 while the connection was fine; with `outgoingHighWaterMark` 64 none. Raising the mark to 24 or more clears that case (frame of 12 datagrams: 16 refused 132 to 144, 20 refused 36 to 60, 24 and above none).

**What a deeper queue costs.** The mark cannot simply be set to 64 at session open, which is what the re-review prototyped. More than one datagram handed to the connection at a time loses blocks of a burst: bursts of 3 x 22 = 66 in one task (3 rounds): 0 lossy runs of 30 at the default, 3 of 30 at 32, 4 of 30 at 64 (72 datagrams lost in the worst 4); bursts of 3 x 85 = 255: 0 of 40 at the default, 5 of 40 at 16, 7 of 40 at 32 (1x120 in a task: 0 of 20 up to 16, about 1 in 4 at 64). The suite's own checks failed 3 runs of 6 at 64 (the first, 66-datagram check) and 1 of 6 at 32 (the 255 one). The cause was **not found**: vnet reports no refusals, the relay's counters show no drops and no rate limiting, the kernel's UDP counters (`RcvbufErrors`, `InErrors`) stay 0, and a relay that reads into a 4096-deep channel loses as much; the missing datagrams are blocks of consecutive ones, in any round. (Chromium 153 has no `WebTransport.getStats()`, so the browser's own counters could not be read.) The likely reason the default of 1 never loses is that it paces the datagrams one per round trip of the page, where a deeper queue hands over a flood at once.

**Design chosen: raise the mark only after the first stall.** The queue stays at the default. The first time the stall rule would refuse a datagram in a session, vnet sets `outgoingHighWaterMark` to 64 instead (feature-detected, in a `try`, only if it is lower; a browser without the attribute keeps the default and refuses as before), and the rule judges again from that moment (turns and time are counted from the raise, so the backlog gets the chance to drain). A session whose main thread is never saturated never changes it, so every burst scenario behaves as before (and loses nothing, as measured); a session that does saturate it stops being throttled to one datagram per task. After the raise the rule is unchanged, so a real stall (writer that never settles, frozen relay) is still refused and bounded: the cap of 512 and the 250 ms rule, as before. Cost: in a saturated session the datagrams leave in floods of 10 to 20 at a time and the browser then loses a few (with a strict delivery check the 300 ms no-idle case lost 5 and 7 of 120 in 2 of 5 runs, and the 7-peer game load up to 18 of 2000 once; the checks therefore require no refusal by vnet and 90 percent delivery).

**The turn rule is kept, but no test distinguishes it any more.** With the raised queue a false stall costs one raise, not a drop, so the 3-turn requirement matters less: removing it (age only, from the first turn) passed the suite twice. A one-turn rule lost 10 datagrams to the stall rule once in 4 runs of the 300 ms-no-idle case, with three turns 0 in 11. It was left at 3 for margin (it is cheap: the counting is needed anyway for the age from the first turn), and this is recorded as open: no mutation of it fails a check now.

**Tests.** `wt_burst.test.js` runs the same-task busy case again with no idle (`--gap 300` for a 300 ms busy: the fix-round change to `--gap 700` is reverted), a jfsw-like game (6 datagrams, 112 ms of work, 6 more, 40 frames, no idle), and a hog of 110 ms with 1 ms idle (99 percent busy), all under a rule that vnet refuses nothing (eagain 0, `tx_dropped` 0, accepted = attempted, the page's own drops 0) and at least 90 percent arrive. They fail on `6e74bf7` (60 of 120 accepted, 66 of 480, and 6 of 960 refused) and pass here. The other checks keep requiring every datagram.

Why not the others:

- *`desiredSize` / `ready`*: `desiredSize` is `hwm - unsettled`, the same number as the old counter. *`outgoingHighWaterMark`* is not a replacement for the stall rule (it only moves the burst limit) but is used with it: see fix round 3.
- *`outgoingMaxAge`*: ignored by Chromium 153, and unknown for Firefox and Safari.
- *A much higher cap alone*: bounds memory but not staleness; a stalled connection would hold up to the cap of stale datagrams and send them all when it clears.
- *Age alone*: no memory bound if a task sends a huge burst; so both.

Firefox (not run, no browser here): `outgoingHighWaterMark` is part of the same interface (`WebTransportDatagramDuplexStream`); the code reads it in a `try` and only writes it when it is a number lower than 64, so a browser that lacks it, throws on it or ignores it keeps today's behaviour (refusals under a saturated page, as 0.5.0). Whether Firefox has the one-datagram-per-task limit at all is unknown. It implements the same `WebTransportDatagramDuplexStream` attributes and write promises, so the stream-level behaviour should be the same (a write stays unsettled until the datagram is queued or sent). If its writes settled only when sent, a 256 burst would take longer than in Chromium but stay far below the 512 cap and the 250 ms age.

**The control path** (`JS_MAX_CONTROL_IN_FLIGHT` = 64) was checked and left alone. It carries only handshake requests, timed keepalives and a close: a few frames, on timers, never a burst, and a client cannot send 64 in one task through any API. Counting unsettled writes there is right, because a control write that stays unsettled means a stuck stream, which ends the session.

**`relay_ws.c` has the same limit, measured in bytes (corrected in review).** It limits on `WebSocket.bufferedAmount` (64 KiB), and `bufferedAmount` cannot shrink inside the task that sends either: the browser drains it later. So one task that sends more than about 64 KiB is cut: 3 x 25 datagrams of 1100 bytes in one task over WebSocket: 60 of 75 accepted; 3 x 20 x 1100 bytes and 3 x 200 x 100 bytes lose nothing (the burst test here, 3 x 22 x 100 bytes, passes). It is the same bug, but only above about 59 maximum-size datagrams in one task, which small game packets do not reach; not changed here (the brief fixes WebSocket only if the same bug is found, and a byte limit that tolerates a burst needs its own design: an age/turn rule like the WebTransport one, or a higher byte cap). **Follow-up.**

### Drops are reported

`vnet_stats` gained `tx_dropped` at the end (source compatible, 0.5.0): every datagram the backend refused with `VNET_EAGAIN`, at `vnet_sendto()` or, with impairment, when `vnet_poll()` released a delayed datagram (`vnet_sendto()` had returned success then). Sources: UDP `EWOULDBLOCK`/`EAGAIN`/`ENOBUFS`, relay-udp (the inner UDP socket), relay-ws `bufferedAmount` over 64 KiB, relay-wt cap or stall rule; never loopback; relay control frames are not counted. A unit test covers the impaired path (`RelayWsStyle.ImpairedDatagramRefusedWhenReleasedCountsAsDropped`); a UDP test is not cheap (it needs a full kernel send buffer) and was skipped. `Module.vnetWT.s[id].drops` still counts them in the page. The relay also counts only what reaches it; it saw no drops and no rate limiting in the runs.

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
| 10 datagrams, 300 ms busy-wait in the same task, 10 more, 3 rounds (60) | 47 of 60 (13 dropped) | 60 of 60 |
| game, 3 peers + 3 extra, another task hogs the main thread 300 ms every 700 ms (960) | 960 of 960 | 960 of 960 (an age-only rule: 6 dropped) |
| the same, 400 ms every 1000 ms (960) | 960 of 960 | 960 of 960 (an age-only rule: 12 dropped) |
| **no idle:** 10 datagrams, 300 ms busy-wait in the task, 10 more, a new step every 300 ms, 6 rounds (120) | 0.5.0: 60 of 120 | 120 of 120 (a few lost by the browser in 2 of 5 runs of a strict build, at most 7) |
| **no idle:** a game of 9 frames/s, 6 datagrams, 112 ms of work, 6 more, 40 frames (480) | 0.5.0: 84 of 480 (396 refused; arrival of the rest 1.7 s late); 66 of 480 in the test | 480 of 480 |
| **no idle:** game, 3 peers + 3 extra, another task hogs 110 ms with 1 ms idle (960) | 0.5.0: 954 of 960 (6 refused; 1 peer + 5 extra: head age 334 ms) | 960 of 960 |

So a steady 40 tics a second to 3 or 7 peers with a few extras stays under 16 per task and did not drop even before; the drops need more than 16 sends in one task (several tics when a frame falls behind, a resend burst, more peers or extras). Every drop makes the game resend, which makes the next burst bigger: the feedback the brief describes. A first version of the 7-peer scenario ran every step with 3 tics (1200 datagrams a second); the **relay** rate-limits a client at 600 a second sustained (`-client-pps`), so that is not a fair test: the final scenario keeps the average at about 500. The test also checks that the relay rate limited and dropped nothing. All "after" numbers were re-measured with the final code (`wt_burst.test.js`); the "before" ones are from 0.4.0 (`relay_wt.c` as of `579a410`) and, in the last two rows, from the first-round age-only rule. Not checked: jfsw-browser itself (AC5), and a real network.

### Back-pressure and the mutation (tests 2 and 3)

- **The cap alone (a writer whose writes never settle; the page wraps `WebTransport`):** one burst of 600 datagrams in a single task, which the age rule cannot touch. Exactly 512 writes were made and are queued (511 data datagrams and one HELLO answer), 89 got `VNET_EAGAIN`, the drop counts agree. Before: 15 accepted.
- **The age rule alone (a relay frozen with SIGSTOP while the sender keeps sending, 10 per 25 ms):** no drops while it reads; drops start once it stops; the queue stays at about 107 writes, far below the cap (the test requires fewer than 300); the session is still up while the relay is only frozen, and **fails when the relay is killed, as today** (`session closed by the relay`, 0.6 s).
- **The long task:** 300 ms of busy-waiting in the task, and a hog task of 300 and 400 ms between game steps: zero drops in all three.
- **Robustness:** `wt_burst.test.js` has a 10-minute watchdog that kills every child process and exits 1, every `page.evaluate`, `ctx.close()` and `browser.close()` has a 15 s timeout, children (relays, receivers) are killed in `finally`, and the CI `browser` job has `timeout-minutes: 40`.

**Mutations** (applied to `src/relay_wt.c`, run against the final `wt_burst.test.js`, restored, never committed):

| mutation | checks failed | which |
|---|---|---|
| old limit: cap 16, no age rule | 12 | the burst checks, the 255 burst, both catch-up loads, the no-idle cases, the 110 ms hog, the cap checks (16 queued) |
| the whole 0.4.0 file | 14 | the same, and the queue checks (no `dgT`) |
| cap only (512, no age rule) | 1 | the frozen relay's queue is 512, not below 300 |
| age only (no cap) | 3 | the 600 burst (617 queued, none refused) |
| neither (unbounded) | 7 | the 600 burst, no drops on the frozen relay, the queue |
| the raise of the browser's queue removed (`6e74bf7`) | 5 | the 300 ms no-idle case (60 of 120), the 112 ms frames (66 of 480), the 110 ms hog (6 refused) |
| the turn rule removed (age from the first turn only) | 0 | **passes**: see fix round 3; kept for margin, not pinned by a test |
| an infinite loop in the page's send path (hang guard) | ERROR | `page.evaluate` timed out after 15 s; the whole run ended after 72 s with exit 1 (the 10-minute watchdog was not needed) |

The burst test then ran 7 times on the final code (6 alone, once in the full browser suite): 0 failures. An earlier build with a strict delivery check on the no-idle cases failed 2 of 5 runs on it, which is why those checks are tolerant.

### Bit Riot

Read-only, on its `claude` branch (`81e8dbc`). The host's `LockstepSession::hostSend()` runs on each `pump()` and sends, per present peer, one `TICKS` when there is something new or unacknowledged (resent after 40 ms) and an empty `TICKS` as a keepalive, plus a `LOBBY` broadcast to every peer now and then, plus the catch-up window for each late joiner (`CATCHUP_WINDOW_PACKETS` = 8 datagrams). `MAX_PEERS` is 6, so up to 5 peers. A 4-player browser host (3 peers) sends about 3 to 9 in a pump in steady state, **under 16**, but it can **pass 16** in one task when a frame runs several pumps (hitch, extra ticks), when resends coincide with new ticks and the lobby broadcast, or a late joiner is being caught up (up to 8 more); a 6-player host or a mid-join reaches it more easily. I did not run Bit Riot. With 0.5.0 none of these is dropped by vnet.

### Deviations and follow-ups

- Branch name (see the top). CTest (configured like CI's Linux job, 275 tests, including `relay_go_tests`): all pass, except that this container's kernel has no IPv6 (no `/proc/sys/net/ipv6`, no `::1`), so `UdpFamily.Ipv6LoopbackRoundTrip` is skipped, and with `VNET_REQUIRE_IPV6=1` it and `UdpFamily.DualStackOrFallbackReachesIpv4PeersAndReportsIpv4Senders` fail for that reason alone. CI creates `::1` first.
- New: `examples/burst_probe.c` (browser sender, native receivers), `tests/browser/wt_burst.test.js` (run by `scripts/test-browser.sh`), the measurement tool, and `?arg=` pass-through in `examples/shell.html`.
- Follow-ups: try jfsw-browser at 0.5.0 (AC5); **WebSocket's byte limit in one task** (above); Firefox and Safari by hand (the cap and age rule are browser independent, but their write timing was not measured); a relay-side per-client limit of 600 datagrams a second is worth knowing about for 7-player hosts with resend storms (not a bug).
