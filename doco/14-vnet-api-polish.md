# 14 — vnet API polish and browser lifecycle

- **Repo:** `VenKamikaze/vnet`
- **Branch:** `vnet/14-api-polish`, from `claude`
- **Depends on:** 11
- **Status:** todo

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

*(fill in when done)*
