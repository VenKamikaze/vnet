# Brief 15: coordinator handoff note (14 Nov 2026)

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

1. **Re-review `148748a`** (Opus, focused):
   - **The raise-on-stall design.** It is a deviation from the reviewer's "set 64 at open".
   - **Loss after the raise.** In saturated sessions the browser silently loses a few datagrams after the raise:
     5 to 7 of 120 in 2 of 5 runs, and 18 of 2000 once.
   - **The new no-idle checks** require zero refusals by vnet but only 90% delivery. Is that acceptable, or is it a
     loosening?
   - **The 3-turn rule has no test.** The mutation that removes it passes. Pin it with a test or remove it.
2. **`MessageChannel` leak (should fix).** `vnet_wt_close` never closes `s.dgPing` or `port1`. In Blink, a port with
   an `onmessage` listener stays alive, together with `s` and the `WebTransport` object. The fix is to close both
   ports on close.
3. **CI on `6e74bf7` failed** only on macOS / clang, in the native test
   `RelayClient.TheWakeCallbackIsNeverCalledByTheNativeRelayClient`. It passed on `71b3258`, and brief 15 hardly
   touches that path, so it may be flaky. Look into it, and check CI on `148748a` (run 43526140207).
4. **Then:**
   - check the final head yourself;
   - set the brief's status in the `README.md` table to "done";
   - delete this file;
   - merge into `claude` with `--no-ff`;
   - tag `v0.5.0`;
   - after the user approves, push `claude` and the tag.
5. **Optional, AC5:** a jfsw-browser WebTransport game with a browser host and two guests at 40 tics a second.
   **Untested:** Firefox and real networks.
