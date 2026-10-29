# 14 — vnet API polish and browser lifecycle

- **Repo:** `VenKamikaze/vnet`
- **Branch:** `vnet/14-api-polish`, from `claude`
- **Depends on:** 11
- **Status:** done

## Goal

Act on the [design review](design-review.md) of vnet 0.3.0 before Bit Riot's netplay briefs consume the library: fix the one real bug (R1), add the small additive APIs that briefs 07 and 12 need, and write down what a game integrator has to know. Release the result as **0.4.0**.

**Every change is additive.** Existing callers, the `vnet_config`/`vnet_status_info` field order, and relay protocol version 1 must keep working unchanged. New struct fields go at the end.

The design review is the detailed spec for each item: the files, the reasoning and the suggested tests. This brief chooses which items are in scope and settles the choices the review left open. If the review and the code disagree, trust the code, and fix the review in this branch.

## Read first

- [`README.md`](README.md): settled decisions D3–D5, D7 and D9, and how to work a brief.
- [`design-review.md`](design-review.md), in full.
- The Outcome sections of briefs [06](06-vnet-core.md), [10](10-vnet-relay-websocket.md) and [11](11-vnet-webtransport.md).
- [`relay-protocol.md`](relay-protocol.md), `include/vnet.h`, and the sources each item names.
- For how vnet will be used: Bit Riot's `doco/netplay/07-lockstep-netplay.md` and `12-browser-netplay.md` on its `claude` branch.

## In scope

From the review's summary table. The user agreed to all highs and mediums, plus the low items marked here.

1. **R1 (high): the relay client treats a gap between polls as sleep, not as relay silence.**
   - After a poll gap longer than one keepalive interval, reset `last_rx`, send a `KEEPALIVE` at once, and extend the handshake deadline by the gap.
   - Test with the scripted transport and an injected clock: a 60 s jump between two polls leaves the client READY, and it sends a keepalive. Do the same during the handshake.
   - The old behaviour must fail the test.
2. **R2: `vnet_set_impair(v, const vnet_impair*)`.**
   - NULL or all-zero turns impairment off.
   - **Decision:** datagrams already queued for delay stay queued and are released on schedule.
   - Setting a new impairment reseeds the PRNG from its `seed`, so runs stay reproducible.
   - Return `VNET_ENOTSUP` on any backend that has no impairment layer, and document which those are.
3. **R3: an optional wake callback.**
   - Add `vnet_config.wake` and `wake_user`. The browser relay transports (WebSocket, WebTransport) call it when a frame is queued or the transport state changes.
   - The callback may only schedule a poll; document that it must not call back into vnet.
   - Native backends never call it. Loopback doesn't need to either (optional).
   - The browser test checks that the callback fires in a page whose main loop is frozen.
4. **R4: `VNET_BACKEND_RELAY_AUTO` works natively**, where it means `RELAY_UDP`.
   - Add a new `relay_udp` config field (`host:port`), at the end.
   - **Decision:** natively, use `relay_udp` if it is set. Otherwise use `relay`, provided it has no URL scheme. Otherwise return `VNET_EINVAL`, with a clear log line.
   - Browser behaviour is unchanged. Update `examples/relay_peer.c` to drop its `#ifdef`.
5. **R5:** add `relay_rtt_ms` (the latest `KEEPALIVE` round trip; -1 when unknown, 0 on UDP and loopback) and `max_members` (from `ROOM`) to the end of `vnet_status_info`. A smoothed RTT is optional.
6. **R6: a game-loop integration page, `doco/integration.md`,** with the contents the review lists.
   - Add R10's vendoring recipe for builds that don't use CMake, as a section of this page. Leave out the pkg-config file and the amalgamation.
   - Link the page from the top-level README.
7. **R7: documentation and API hygiene.**
   - `VNET_EAGAIN` from `vnet_sendto` can come from any backend; treat it as a drop.
   - Protocol v1 has been frozen since v0.2.0. Say what may still be added to it.
   - Write an API-stability policy: 0.x is additive from 0.3.0 on, 1.0 comes after Bit Riot 07 and 12, and note the `SameMajorVersion` caveat.
   - Add `const char *vnet_version(void)`.
8. **R8: `vnet_wait(v, timeout_ms)`.**
   - UDP and relay-UDP use `poll`/`WSAPoll`. Relay-UDP also returns early at its next keepalive or retransmit deadline.
   - Loopback and the browser backends return `VNET_ENOTSUP`.
   - It returns 1 for ready, 0 for a timeout, and a negative code on error.
   - Switch `examples/` and `tests/smoke.c` from sleep loops to `vnet_wait` where that is natural.
9. **R9 (low, included): `vnet_resolve_host(host_port, family, vnet_addr *out)`.**
   - This exposes the lookup `relay_udp.c` already has. `vnet_create()` still accepts names.
   - `integration.md` documents the recommended pattern: resolve at settings load or on a worker thread.
10. **R12 (low, included): burst loss** in `vnet_impair`, using a two-state Gilbert–Elliott model.
    - The new fields go at the end; zero means off. Extend the `VNET_IMPAIR` parser to accept them.
    - **No bandwidth cap.**
    - Test: given a seed, the loss pattern is deterministic, and the measured burst lengths match the configuration within a tolerance stated in the test.
11. **R13 (low, partly included):**
    - Set `SO_RCVBUF` to 256 KiB, best effort, in `udp.c:make_socket`. Ignore failures; log at debug level. Don't add a config field.
    - Give the WebSocket relay transport a separate control queue, as `relay_wt.c` already has, so a burst of data cannot push out a keepalive answer or an `ERROR(CLOSED)`. Test it with the scripted transport where possible.
    - **Leave out** the `size` field on `vnet_status_info`.
12. **R11 and R14: protocol notes only, no code.** In `relay-protocol.md`:
    - choose and write down how v1 will be extended: new `*_EXT` frame types, or a documented use of the padding bytes. **Recommended:** new frame types, with the padding kept zero. A single extension mechanism must be settled before anything uses the padding.
    - Reserve virtual addresses `10.0.0.254` and `.255` (for a future gateway pseudo-member and broadcast), which no client may be assigned.
    - Record the Odamex packet-size gap (vnet's 1100 bytes against Odamex's 1200) and the per-member rate-limit note, as input for a future gateway brief.
13. **Version 0.4.0.**
    - Bump the CMake project version and `VNET_VERSION_*`, and update the README's pinned-tag section to mention v0.4.0.
    - The user tags `v0.4.0` from a PC, because the cloud session cannot push tags. Give them the exact command in the Outcome.

## Out of scope

- **R11's room control and room secret** (`CLOSE_ROOM`, `KICK`, the secret). Only the extension decision in item 12 is in scope.
- **R14's gateway rate limits and fragmentation helper.**
- **R15**, loopback relay emulation.
- **R10's pkg-config file and amalgamation.**
- A bandwidth cap in the impairment.
- Anything that changes D7: vnet stays unreliable-only.

## Tests

- Each new API and behaviour gets tests that **fail without the change**. Prove it by reverting or mutating the change.
- The existing suites must still pass:
  - the C/C++ tests;
  - the Go relay tests (`go test ./...`, with `-race` where it is already used);
  - the browser (Playwright) tests;
  - the ASan build.
- Build the library with Emscripten as well, since items 3 and 4 touch the browser backends.
- The browser test for R1 is optional if the scripted-transport test covers it, but the R3 wake test is required.

## Acceptance criteria

- R1 is fixed and covered by a test.
- Items 2–11 have landed with tests, and items 6, 7 and 12 are written.
- No existing caller or test needed changing, except to adopt the new APIs where this brief says so.
- The project version is 0.4.0, and the README's pin section is updated.
- `design-review.md` has a short note at the top recording what brief 14 took up and what it left out.

## Outcome

Landed on `vnet/14-api-polish`, version **0.4.0**. Every in-scope item is implemented and tested; nothing was cut. Protocol version 1 and every existing struct, field order and caller are unchanged: the only existing tests touched are listed under Deviations.

**What landed**

1. **R1.** `rc_poll` measures the gap since the previous poll. A gap of more than one keepalive interval (floor 1 s, see below) is taken for the application sleeping: the silence clock restarts, the handshake, primary-transport (AUTO) and datagram-size-probe deadlines move forward by the gap, and the `KEEPALIVE` that is then due goes out in the same poll. Tests (scripted UDP relay, WebSocket-style and WebTransport-style scripted transports, injected clock): a 60 s jump leaves a READY client READY and sends one keepalive; the same while connecting, during the handshake, during the probe and before the AUTO fallback; real silence is still found within three polled intervals after a resume; slow polling below the floor is not a suspension.
2. **R2.** `vnet_set_impair()`. Queued delayed datagrams stay queued and are released on schedule (also after impairment is turned off or replaced); a new setting reseeds the PRNG from `seed`; NULL, all-zero and seed-only turn it off; invalid values change nothing; the call wins over `VNET_IMPAIR`, which only `vnet_create()` reads; the delay queue is allocated on first use with the `impair_queue_len` given to create. Every current backend has an impairment layer, so `VNET_ENOTSUP` is documented but unused.
3. **R3.** `vnet_config.wake` / `wake_user` and `vnet_wake_fn`. The WebSocket transport calls it from `on_open`, `on_message` (when a frame was queued), failure and close; the WebTransport transport from `T.enqueue` and every state change in its JavaScript, through the C function pointer and `getWasmTableEntry` (no exported function, no link flag). Never after the transport is closed. New `examples/wake_probe.c` (browser: an echo host, either with a loop frozen after READY or with no loop at all, driven by wake-ups; native: the guest that pings it, using `RELAY_AUTO` and `vnet_wait`) and `tests/browser/wake_browser.test.js` (WS, WT and AUTO, both modes, `requestAnimationFrame` stubbed out). Native backends never call it (tested).
4. **R4.** `relay_udp` config field; natively `RELAY_AUTO` is `RELAY_UDP` to `relay_udp`, else `relay` if it has no `scheme://`, else `VNET_EINVAL` with a log line that names `relay_udp`. Browser behaviour unchanged. `examples/relay_peer.c` now always uses `RELAY_AUTO` (`--backend` overrides; new `--relay-udp`).
5. **R5.** `vnet_status_info.relay_rtt_ms` (latest `KEEPALIVE` round trip; -1 unknown; 0 on UDP and loopback) and `max_members`. A round trip that spans a gap is not measured. No smoothed RTT.
6. **R6.** `doco/integration.md` (all the review's contents, plus the vendoring recipe of R10 and the API-stability policy); linked from the README.
7. **R7.** `VNET_EAGAIN`/`VNET_EIO` documented as a drop on every backend; protocol v1 declared frozen since v0.2.0 with what may still be added; API-stability policy and the `SameMajorVersion` caveat (header, integration page, README); `vnet_version()` (a test checks it against the macros and the CMake project version).
8. **R8.** `vnet_wait(v, timeout_ms)`: `poll`/`WSAPoll`, UDP and relay-UDP; relay-UDP also returns at its next keepalive, silence or handshake-retransmission deadline and at once if a datagram is already queued; any backend returns early for the next delayed-datagram release. Loopback, WebSocket and WebTransport: `VNET_ENOTSUP`. Negative timeout: `VNET_EINVAL`. `tests/smoke.c` and `examples/relay_peer.c` use it.
9. **R9.** `vnet_resolve_host(host_port, family, out)` in the new `src/resolve.c` (the relay-UDP lookup moved there). Malformed: `VNET_EINVAL`; wrong family: `VNET_EAFNOSUPPORT`; lookup failure: `VNET_EIO`; browsers: `VNET_ENOTSUP`. On Windows it starts and stops Winsock itself.
10. **R12.** Burst loss: `vnet_impair.burst_pct` and `burst_len`, keys `burst=` and `blen=`. See Deviations. No bandwidth cap.
11. **R13.** `SO_RCVBUF` 256 KiB at socket creation, best effort, logged at debug level when refused, no config field. The WebSocket transport's queue moved to `src/relay_inbox.c`: a 256-frame DATA ring and a separate 64-frame control ring; control frames come out first and are never pushed out by data; control overflow fails the transport, as in `relay_wt.c`. No `size` field.
12. **R11/R14, protocol notes only** in `relay-protocol.md`: extension by new `*_EXT` frame types with the padding kept zero and ignored forever; `10.0.0.254` and `.255` reserved; the Odamex packet-size gap, the per-member rate-limit note and the gateway groundwork under "Future extensions"; "Versioning" rewritten (frozen since v0.2.0, what may be added).
13. **0.4.0.** CMake project version, `VNET_VERSION_*`, README pin section (`GIT_TAG v0.4.0`).

**Tests**

- C/C++: `ctest` 270 entries (204 before), 269 pass and 1 skips (the IPv6 test, no `::1` here); also clean under gcc `-Werror`, clang `-Werror` (270, 1 skip) and ASan+UBSan (270, 1 skip). The Go relay is unchanged: `relay_go_tests` passes (it is one of the 270).
- Browser: `scripts/test-browser.sh` with the pinned emsdk 6.0.13 and Chromium: the existing WebSocket and WebTransport suites pass unchanged on `relay_peer` (now `RELAY_AUTO`), and the new wake suite passes (3 transports x frozen/driven).
- MinGW: x86_64 cross build of the library, smoke test, `relay_peer` and `wake_probe` with `-Werror`; `vnet_smoke.exe` (which now exercises `vnet_wait` on `WSAPoll`, `vnet_set_impair` and `vnet_resolve_host`) passes under Wine 9, and `scripts/test-mingw-relay.sh` passes in both directions. i686 not built.
- Emscripten 6.0.13 build of the library, `relay_peer` and `wake_probe` with `-Werror`: clean.
- **Proof that the tests fail without the change** (each mutation applied to a scratch edit, built, the suite run, restored; every mutation below made at least one named test fail): R1 (gap handling off: 9 tests; the t_start, stage_start and probe-deadline extensions one by one; the 1 s floor), R5 (RTT always 0, `ka_pending` never set, `max_members` not stored), R2 (no reseed, off keeps active, off drops the queue, queue length ignored), R12 (wrong mean length, burst on the main PRNG stream, bursts not sticky, no validation, parser key), R8 (poll result ignored, timeout ignored, one socket skipped, impairment due time ignored, relay deadline ignored, queued datagrams ignored, handshake deadline ignored), R13 (`SO_RCVBUF` removed or the wrong size; control and data in one ring; data before control), R9 (no family filter, wrong error code), R4 (relay preferred over `relay_udp`, URLs accepted), R7 (version string, CMake version skew), R3 in the browser (the C hook made a no-op: the frozen and driven tests fail for all transports; the WebTransport JavaScript wake removed from `T.enqueue`: the WebTransport tests fail). One mutation (the "resumed" flag that sent the post-gap keepalive) survived and was removed as redundant: the keepalive falls due anyway in any poll that follows a gap longer than an interval.

**Deviations from the brief and corrections**

- **Existing tests changed** (the old behaviour they encoded is what the brief changes): `RelayClient.SilentRelayEndsTheSessionAfterThreeIntervals`, `RelayClient.DefaultHandshakeTimeoutIsTenSeconds` and `RelayFallback.TheFallbackGetsTheWholeHandshakeTimeoutNotWhatIsLeft` jumped the clock by more than a keepalive in one step, which R1 now (rightly) treats as sleep; they poll in steps instead, asserting the same thresholds. The native `RELAY_AUTO` rows of `RelayAutoArguments` (`"wss://..."` and `"ws://..."` were `VNET_ENOTSUP`, now `VNET_EINVAL`; `"127.0.0.1:7777"` now succeeds and moved to new tests). The `relay_transport_ops` initialisers in three test files gained a trailing `nullptr` for the new optional `wait`.
- **Burst loss is counted in packets, not milliseconds** (`burst_len`, not the review's `burst_len_ms`): a millisecond model needs an exponential draw (libm) or depends on the send rate; the packet model is exactly reproducible. Tolerances are stated in `BurstLoss.BurstLengthsAndLossRateMatchTheConfiguration`.
- **A floor of 1 s on the suspension threshold.** The brief says "longer than one keepalive interval"; `keepalive_ms` may be as low as 100 ms, and a game polling every 200 ms would then switch silence detection off. The threshold is `max(keepalive, 1 s)`; identical to the brief at the default 5 s.
- **`relay_rtt_ms` is KEEPALIVE only**, as the brief says, so the first sample comes one keepalive interval after READY (or at once after a gap). A sample from the handshake round trip would give the lobby a number earlier.
- **Corrections to the review** (also marked in it): `relay_peer.c` had no `#ifdef` for the backend (it chose by URL scheme); "natively the same logic covers sleep/debugger pauses for free" holds only for pauses shorter than the relay's 15 s UDP session timeout, after which the client now learns `VNET_ECLOSED` within a round trip; the WebTransport wake needs no exported function.
- `10.0.0.254` is reserved in the protocol document, but the relay still accepts `-max-members 254` (the brief says protocol notes only, no code). The document says so.
- **CI** (run 40836134763 on `ca6fa23`): all 7 jobs green: gcc, clang, gcc+ASan/UBSan, macOS clang, the Go relay (vet, race, fuzz, Docker), the browser job (Emscripten + Chromium, including the wake test) and MinGW under Wine. Two macOS problems CI found in this branch's first pushes were fixed: a `POLL_IN` macro clash in `udp.c`, and the Linux-only `SO_RCVBUF` seam tests linked on macOS. Not run: MinGW i686.

**Follow-ups**

- For Bit Riot 07: pin `v0.4.0`; read `doco/integration.md`; use `vnet_set_impair()` in the stall/leave/host-loss tests and `burst=`/`blen=` for blackout tests; give each process its own `seed=`; use `vnet_wait()` in `bitriot_headless`; resolve `--join host:port` names with `vnet_resolve_host()` (before the loop or on a worker).
- For Bit Riot 12: one `vnet_config` for both worlds (`relay` URL plus `relay_udp`, backend `RELAY_AUTO`); `cfg.wake` to step a hidden tab from network events (the frozen-loop and no-loop pages in `wake_probe.c` are the working shape); `st.max_members` and `st.relay_rtt_ms` in the lobby; a browser tab paused for a minute stays in its room (WebSocket and WebTransport), whereas a native UDP peer paused for more than 15 s is dropped by the relay and gets `VNET_ECLOSED`.
- Reduce `-max-members` to 253 in the brief that first uses `.254`. Not done: a smoothed RTT, a handshake RTT sample, `size` on `vnet_status_info`, a pkg-config file, an amalgamation, R11's room control, R15.

**Releasing 0.4.0.** The cloud session cannot push tags. **After the coordinator has merged this branch into `claude`** (and `claude` is the commit to release), run on a PC, in a clone with that merge commit checked out:

```sh
git fetch origin
git checkout claude && git pull --ff-only origin claude   # the merge commit
git tag -a v0.4.0 -m "vnet 0.4.0: API polish and browser lifecycle (brief 14)"
git push origin v0.4.0
```

Run it only after the merge: tagging earlier would pin a commit that is not the released one.
