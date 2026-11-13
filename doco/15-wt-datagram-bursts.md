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

(To be written by the implementer.)
