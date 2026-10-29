# vnet work briefs

vnet is a small C library that gives games a non-blocking "virtual UDP" datagram API, plus a relay server. Its backends:

- **native UDP**
- **in-process loopback**, for tests
- **relay over UDP**, for native clients
- **WebSocket and WebTransport**, for browser (Emscripten) builds

It was planned alongside online play for [Bit Riot](https://github.com/VenKamikaze/BitRiot). Its first consumer is Bit Riot's lockstep netplay. Later consumers could be other games' existing net drivers (jfSW's `mmulti.c`, Odamex).

Each brief (`NN-*.md`) is one implement → review → merge cycle. Brief numbers match Bit Riot's master plan in [`doco/netplay/README.md`](https://github.com/VenKamikaze/BitRiot/blob/claude/doco/netplay/README.md), which shows how the two repos' work interleaves. The plan lives on the `claude` branch.

| # | Brief | Depends on | Status |
|---|---|---|---|
| 06 | [vnet core: API, loopback and native UDP](06-vnet-core.md) | — | done |
| 10 | [Relay and WebSocket transport](10-vnet-relay-websocket.md) | 06 | done |
| 11 | [WebTransport datagrams](11-vnet-webtransport.md) | 10 | done |

**Bit Riot briefs that consume vnet:** 07 (native lockstep, needs 06) and 12 (browser netplay, needs 10). A session doing those needs both repos attached.

Wire format of the relay: [`relay-protocol.md`](relay-protocol.md). Operating it: [`../relay/README.md`](../relay/README.md).

## Settled decisions

Copied from Bit Riot's plan. The D-numbers match. Don't reopen these inside a brief; if one turns out to be wrong, stop and raise it with the user.

- **D3. vnet is its own repository.** It holds the C library, its backends and the relay.
  - **Licence:** MIT.
  - **Visibility:** public.
- **D4. The relay is written in Go.** Use `quic-go/webtransport-go` plus a WebSocket library, built as one static binary.
  - Python is fine for test tools (fake clients, loss simulators).
  - No Java or Spring for the relay.
- **D5. No SDL_net in consumers.** Bit Riot's native netplay goes straight through vnet's native UDP backend. vnet also has an in-process **loopback backend** with simulated delay and loss, for tests.
- **D7. vnet is unreliable datagrams only.** Reliability and ordering belong to the game's protocol.
  - Bit Riot's lockstep protocol repeats every unacknowledged input in each packet, so it works unchanged over lossy UDP and over reliable WebSocket.
  - Don't add a reliability layer to vnet.
- **D9. Browser transport:** WebSocket first, then WebTransport datagrams, keeping WebSocket as the fallback.

## How to work a brief

**Branches.**

- `main` is the default branch.
- `claude` is the working base, as in Bit Riot. Create `vnet/NN-short-name` from `claude`, and merge back with `--no-ff` once reviewed, then push both branches.
- Never commit directly to `claude`.
- Merging `claude` into `main` is the user's call.

**Process** (the user's preferred one):

1. A coordinator session gives the brief to a Sonnet implementer.
2. An Opus reviewer reviews read-only, building in a throwaway worktree.
3. The implementer does a fix round.
4. A re-review follows if the fixes were non-trivial.
5. The coordinator verifies and merges.

**Tests.**

- Every brief adds tests that **fail without the change**.
- Never skip, loosen or quarantine a test.

**When a brief is done:**

- set its status in the table above;
- add an **Outcome** section to the brief recording what landed, any deviations, and follow-ups.

**Scope.** If the scope has to grow a lot, stop and ask.

## Cloud container network notes

These were checked in October 2026 under the *limited* network policy. The user has since switched the environment to full network access for new sessions.

First check whether GitHub archive downloads work:

```bash
curl -sS -o /dev/null -w "%{http_code}\n" -L https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.0.zip
```

A `200` means the limits below don't apply.

**Works under the limited policy:**

- `git clone` from github.com
- storage.googleapis.com (emsdk binaries)
- proxy.golang.org and sum.golang.org (Go modules)
- npm
- apt
- the preinstalled Playwright + Chromium (`require('playwright')`; never run `playwright install`)

**Blocked under the limited policy:** github.com **archive downloads** (`…/archive/*.zip|tar.gz`). As a result:

- **CMake `FetchContent`:** use `GIT_REPOSITORY`, never `URL …zip`, or use a system package such as `libgtest-dev`.
- **Emscripten ports:** git-clone the port sources and set `EMCC_LOCAL_PORTS`. vnet's browser backend shouldn't need any ports.

If anything else is blocked, stop and tell the user which host is needed.
