# Brief 15: coordinator handoff note (14 Nov 2026, updated by the second coordinator session)

> **Temporary file. Delete it (`git rm doco/15-HANDOFF.md`) once brief 15 is merged into `claude`.** It
> carries the coordinator's state from one session to the next and is not part of vnet's docs.

## Picking this up

You coordinate brief 15 ([`15-wt-datagram-bursts.md`](15-wt-datagram-bursts.md)). Use the process in
[`README.md`](README.md), "How to work a brief":

- a Sonnet implementer;
- a read-only Opus reviewer in a throwaway worktree;
- fix rounds, with a re-review when a fix is not trivial;
- your own check, then the merge.

The brief's **Outcome** section is the implementers' full account, with all the numbers. This note holds what the
Outcome does not: the state of the review, the user's decisions, and the open points.

**Branch.** All the work is on **`vnet/15-wt-datagram-bursts`**, at head `148748a`. It was branched from
`claude` at `579a410`.

- The user approved that branch name.
- A cloud session's own `claude/...` branch name cannot exist in this repo, because a branch called `claude`
  exists.
- Push with `git push origin HEAD:refs/heads/vnet/15-wt-datagram-bursts`.

**The user's decisions:**

- The version is **0.5.0**. It is a minor bump, because the new field `vnet_stats.tx_dropped` adds to the API, and
  every earlier API addition took a minor bump. The code and docs already say 0.5.0.
- Ask the user before pushing to `claude` and before pushing the `v0.5.0` tag.

**Token budget.** The user says sessions are short of tokens. Ask the user before you spawn any subagent.

**Environment notes for a cloud container:**

- Run `scripts/setup-emsdk.sh`, which installs into `.emsdk`.
- Go is at `/usr/local/go/bin`.
- Use the preinstalled Playwright and Chromium through `require('playwright')`. Never run `playwright install`.
- The container has no IPv6, so `UdpFamily.Ipv6LoopbackRoundTrip` is skipped. That is expected.
- The browser suite is `scripts/test-browser.sh <dir>`.

## Commits so far

| commit | what |
|---|---|
| `dd873bf` | the brief |
| `244fa67` | `vnet_stats.tx_dropped` |
| `99c7572` | tests and tools: `examples/burst_probe.c`, `tests/browser/wt_burst.test.js`, `tests/browser/wt_datagram_probe.js` |
| `d0f45f1` | fix round 1: a cap of 512 and a 250 ms stall rule |
| `71b3258` | docs (the version said 0.4.1 here) |
| `dbdb16a` | fix round 2: the stall rule waits for 3 event-loop turns (counted with a `MessageChannel`), the tests pin both bounds, hang guards, `timeout-minutes: 40` on the CI browser job |
| `6e74bf7` | version 0.5.0, the `tx_dropped` docs and a test |
| `148748a` | fix round 3: raise `outgoingHighWaterMark` on the first stall, and tests for a main thread that is never idle |

## The design now (`src/relay_wt.c`, `vnet_wt_send_datagram`)

**The cause, confirmed in Chromium 153.** A `write()` never settles inside the task that made it. The old cap of
16 therefore limited the datagrams sent in one task, not back-pressure.

**What `148748a` does.** A datagram is refused (`VNET_EAGAIN`, counted in `s.drops` and `vnet_stats.tx_dropped`)
in either of two cases:

- **The cap:** 512 writes are unsettled.
- **The stall rule:** all three of these hold for the oldest unsettled write:
  - it is more than 250 ms old;
  - that age is counted from the first event-loop turn after the write was made;
  - at least 3 turns, counted with a `MessageChannel`, have passed since then.

**The queue-depth raise.** The browser's queue depth, `datagrams.outgoingHighWaterMark`, is 1 by default. The first
time the stall rule would refuse a datagram, the code raises it to 64 instead, feature-detected. The rule then judges
again, counting from the raise.

**Why the depth matters.** With a depth of 1, a main thread that is never idle lets out only about one datagram
per task. The review measured it: the datagrams arrive exactly when their writes settle. A Shadow Warrior-like game
at about 9 frames a second with no idle time had 396 of 480 datagrams refused on `6e74bf7`.

**Why the depth is not set to 64 at open.** Any depth above 1 sometimes loses blocks of a burst made in one task,
and nothing reports it. Chromium 153 has no `getStats()`. The cause is unknown.

**What is not changed:**

- The control path (cap 64) has no bursts and is left alone.
- `relay_ws.c` has the same limit, but measured in bytes: about 64 KiB per task. It is recorded as a follow-up and
  not changed.

## Review history

1. **Review 1 (Opus):** merge after fixes. Its 6 findings were fixed in `dbdb16a` and `6e74bf7`.
2. **Re-review (Opus), stopped early:** a blocker. The no-idle drops are real queueing caused by a depth of 1, and
   `dbdb16a` had hidden them by adding a 400 ms idle gap (`--gap 700`) to the test. Fix round 3 (`148748a`)
   restored the no-idle test and added the raise above.
3. **`148748a` has not been reviewed yet.**

## Open points

1. **Re-review `148748a`** (Opus, focused). **Started in the second session, then paused** at the user's request
   to save tokens: it was told to finish its burst-test run and stop. Its interim report, if any, is under "Re-review
   interim" below. Its throwaway worktree was `/tmp/claude-0/.../scratchpad/review-148748a` (session-local; gone in a
   new container). A new session should start a fresh focused re-review, after asking the user. The points to cover:
   - **The raise-on-stall design.** It is a deviation from the reviewer's "set 64 at open".
   - **Loss after the raise.** In saturated sessions the browser silently loses a few datagrams after the raise:
     5 to 7 of 120 in 2 of 5 runs, and 18 of 2000 once.
   - **The new no-idle checks** require zero refusals by vnet but only 90% delivery. Is that acceptable, or is it a
     loosening?
   - **The 3-turn rule has no test.** The mutation that removes it passes. Pin it with a test or remove it.
2. **`MessageChannel` leak (should fix).** `vnet_wt_close` never closes `s.dgPing` or `port1`. In Blink, a port with
   an `onmessage` listener stays alive, together with `s` and the `WebTransport` object. The fix is to close both
   ports on close.
3. **CI: settled (14 Nov, second coordinator session).**
   - **CI on `148748a` is green.** Run 43526140207 passed on all 7 jobs.
   - **The macOS failure on `6e74bf7` is unrelated to brief 15.** The test,
     `RelayClient.TheWakeCallbackIsNeverCalledByTheNativeRelayClient`, failed in its set-up:
     `tests/test_relay_client.cpp:49`, where `Enter()` → `PollUntilState(READY)` returned false after 5 s of real
     time. The client never saw the fake relay's room frame over loopback UDP, and the test's clock is virtual, so
     nothing is resent. The same test passed on all three Linux jobs in that run.
   - **Likely cause:** a UDP datagram lost now and then on the macOS runner. Brief 15 does not touch the
     relay-udp handshake.
   - **What to do with it:** record it as a follow-up in the Outcome (a flaky native test). Do not fix it in
     brief 15.
4. **Then:**
   - check the final head yourself;
   - set the brief's status in the `README.md` table to "done";
   - delete this file;
   - merge into `claude` with `--no-ff`;
   - tag `v0.5.0`;
   - after the user approves, push `claude` and the tag.
5. **Docs to fix before the merge.** The brief's Outcome starts with "Implemented on
   `claude/webtransport-datagram-bursts-u1oaqa`", and `doco/README.md` (the State line) names the same branch. The
   work is on `vnet/15-wt-datagram-bursts`, so correct both.
6. **Optional, AC5:** a jfsw-browser WebTransport game with a browser host and two guests at 40 tics a second.
   **Untested:** Firefox and real networks.

## Re-review interim

This is the paused Opus re-review of `148748a`. Its verdict: **block, or merge after fixes.**

**Runs:**
- The full browser suite passed: 166 ok, 0 failed (relay_browser 20, wt_browser 63, wake 36, wt_burst 47).
- Native CTest passed **276/276**, with the IPv6 test skipped. The Outcome still says 275.
- Two repeat burst runs were cut short by the pause.

**Findings:**

1. **Blocker: the "browser loses a few after the raise" is the relay, not the browser.**
   - **The limit.** quic-go v0.59.0, `http3/state_tracking_stream.go:12`: `streamDatagramQueueLen = 32`. At `:141`,
     `enqueueDatagram` silently drops a datagram when 32 are already queued. The coordinator confirmed both lines in
     the module cache.
   - **Why the relay hits it.** `relay/wt.go:361` `datagramReader` reads and forwards one datagram at a time, so a
     flood of more than 32 overflows the queue.
   - **Measured.** With `outgoingHighWaterMark` 64 set at open, a 255-datagram burst was lossy in 5 of 10 runs. The
     relay's own drop counter roughly matched the loss.
   - **With the queue patched to 1024:** 0 lost in 20 runs.
   - **What this makes wrong:**
     - The Outcome says "cause not found" and "the browser loses a few". Both are wrong.
     - The 90% checks only tolerate this relay bug.
     - A browser that does not pace one datagram at a time could lose any burst above 32. Firefox is the untested
       case.
   - **Fix:** in the relay, drain datagrams in a tight loop into a deeper queue of its own, or patch or replace
     quic-go's queue length. Count those drops in the relay's stats. Then make the no-idle checks strict again.
   - **Scope:** the brief allows relay changes "if the investigation shows the relay is involved". It now does, but
     **confirm the scope with the user.**
2. **Should-fix: once the relay is fixed, raise-on-stall has no advantage.**
   - The reviewer ran `wt_burst` with a mutant that sets the mark to 64 at open, against the fixed relay. All 47
     checks passed with 0 lost.
   - The stall rule never fired in any check, and the bounds held: 512 queued against a writer that never settles,
     21 queued with the relay frozen, and the session failed when the relay was killed.
   - **Fix:** set the mark at open, and remove `hwmUp`, `dgRaiseK` and `dgRaiseT`.
3. **Should-fix: the no-idle checks pin less than they claim** (`tests/browser/wt_burst.test.js:118`).
   - With `outgoingHighWaterMark` hidden, the 300 ms case and the 110 ms hog case still pass. Only the 112 ms frames
     case pins the raise.
   - `noneRefused` drops failed or missing receivers from the expected count, and never checks `x.ok`.
4. **Should-fix: the 3-turn rule can be pinned** (`src/relay_wt.c:133`).
   - A one-turn mutant fired the stall rule in 4 of 10 hog runs. The real code fired it in 0 of 14.
   - **Proposed check:** assert `hwmUp === false` on the strict idle and hog cases, and run the hog cases with the
     attribute hidden too.
   - The check is probabilistic (it catches the mutant about 40% of the time), so it needs repeats. If finding 2 is
     adopted, re-think this.
5. **Should-fix: the `MessageChannel` leak.**
   - `vnet_wt_close` never closes `s.dgPing`. From Blink's `HasPendingActivity`, a port that is started and entangled
     and has `onmessage` set keeps `s`, `s.wt`, the writers and the queues alive. This is not measured.
   - Nothing else outlives a close: there are no timers, and the promise chains end with the WebTransport.
   - **Fix:** `try { s.dgPing && s.dgPing.close(); } catch (e) {}`.
6. **Nit: docs that no longer match.**
   - `outgoingMaxAge` 250 does not reduce the loss.
   - A browser without the attribute gets a one-time grace, so it does not "refuse as before".
   - The re-judge counts from the raise call, not from the first turn after it.
   - The run counts disagree: the code says 6 and the Outcome 11; the test says 20 and the Outcome 5.
   - `doco/integration.md:38` mentions neither the raise nor the loss.

**Not checked yet:**
- more repeat runs of the stock code;
- measuring the leak (`--expose-gc` with a `FinalizationRegistry`);
- the relay fix with a frozen relay, combined with the current raise-on-stall code;
- a full review of the Outcome text.

**Suggested next step:** a Sonnet fix round, once the user agrees to the relay scope. It would:
1. fix the relay's datagram drain, and count the drops;
2. set the mark at open, and remove the raise;
3. make the no-idle checks strict, and fix `noneRefused`;
4. decide on the turn rule: pin it or remove it;
5. close `dgPing` on close;
6. correct the docs.

Then a short Opus re-review.
