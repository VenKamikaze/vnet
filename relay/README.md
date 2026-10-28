# vnet relay

One static Go binary that lets browser and native players share a room and exchange vnet datagrams as if they were on one LAN. Rooms hand out virtual addresses `10.0.0.N` (the creator is always `10.0.0.1`), and datagrams are forwarded **only between members of the same room**: the relay is not a proxy. The wire format is in [`doco/relay-protocol.md`](../doco/relay-protocol.md).

- native clients: UDP (default port 7777) with the `VNET_BACKEND_RELAY_UDP` backend;
- browsers: a WebSocket at `/vnet` (default HTTP port 8080) with `VNET_BACKEND_RELAY_WS` (Emscripten builds).

## Run it

```sh
cd relay && go build -o vnet-relay ./cmd/vnet-relay      # Go 1.24; CGO is not needed
./vnet-relay -udp :7777 -http :8080 -v
```

or with Docker:

```sh
docker build -t vnet-relay relay/
docker run --rm -p 7777:7777/udp -p 8080:8080 vnet-relay -v
```

On start it prints one line, `LISTEN udp=<addr> http=<addr> stats=<addr>` (the bound addresses, `stats=` empty when off; use `-udp 127.0.0.1:0` for a free port), which scripts and tests read.

Every flag can also be set from the environment as `VNET_RELAY_<NAME>` (upper case, `-` becomes `_`: `VNET_RELAY_MAX_MEMBERS=4`); the command line wins.

| Flag | Default | Meaning |
|---|---|---|
| `-udp` | `:7777` | UDP listen address; empty disables native clients |
| `-http` | `:8080` | HTTP listen address; empty disables WebSocket. Serves `/vnet`, `/healthz`, `/stats` |
| `-max-members` | 8 | members per room, creator included (2..254) |
| `-max-rooms` | 1000 | rooms at once |
| `-max-sessions` | 20000 | members at once (also caps open WebSockets) |
| `-max-sessions-per-ip` | 128 | members at once per source address (IPv6: per /64). Everyone behind one NAT shares it, carrier-grade NAT included |
| `-max-rooms-per-ip` | 16 | rooms created and still alive per source address |
| `-max-ws-per-ip` | 128 | open WebSockets per source address |
| `-max-pending-ws-per-ip` | 32 | WebSockets per source address that have not entered a room yet. Browsers refused here (HTTP 503) retry for about 4 s |
| `-ipv6-prefix-factor` | 4 | an IPv6 /48 may hold this many times the per-address caps in total, so many /64s of one subscriber cannot take the whole relay |
| `-ws-queue-bytes` | 65536 | bytes queued per WebSocket client before its datagrams are dropped (worst-case memory is roughly this times the number of stuck clients) |
| `-room-idle` | 30m | close a room that has carried no data for this long |
| `-session-timeout` | 15s | drop a member silent for this long |
| `-keepalive` | 5s | keepalive interval suggested to clients |
| `-client-pps`, `-client-bps` | 600, 600000 | per-client sustained packet and byte rates (bursts are twice that) |
| `-handshake-rate`, `-handshake-burst` | 20, 50 | room create/join attempts per second per source IP. Many players behind one NAT share this |
| `-origins` | `*` | allowed `Origin` hosts for WebSockets, comma separated |
| `-trust-forwarded-for` | off | use the rightmost `X-Forwarded-For` entry as the client IP (only behind a proxy you control) |
| `-stats` | off | serve `/stats` on this address, e.g. `127.0.0.1:8081` |
| `-v` | off | log room creation and closure |

The public listener serves only `/vnet` and `/healthz`. `/stats` (counters as JSON: rooms, members, frames, drops by reason, path validations) is **off by default**: give `-stats` a private address (localhost or an internal interface) to turn it on.

## TLS (`wss://`) in front of the relay

A page served over HTTPS may only open `wss://` connections, so production needs TLS. The relay speaks plain HTTP and expects a reverse proxy to terminate TLS and forward `/vnet` only.

**Caddy** (certificates are automatic):

```
relay.example.org {
    reverse_proxy /vnet 127.0.0.1:8080
}
```

Browsers then connect to `wss://relay.example.org/vnet`. Start the relay with `-http 127.0.0.1:8080 -trust-forwarded-for` so the per-address limits see real client addresses (Caddy sets `X-Forwarded-For` itself, and the relay trusts the rightmost entry, which is the one the proxy wrote).

**nginx:**

```
location /vnet {
    proxy_pass http://127.0.0.1:8080;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-For $remote_addr;   # set, not appended: nothing the client sent gets through
    proxy_read_timeout 1h;
}
```

Native clients do not use TLS or the proxy: open the UDP port (7777 by default) in the firewall. The UDP path is not encrypted (see the security notes in the protocol document).

**Limits that players behind one address share (for game developers, e.g. Bit Riot brief 12):** every cap above is per source address, so a classroom, a LAN party or a carrier-grade NAT with many players on one public address shares one budget: 128 members, 16 rooms, 128 open WebSockets and 32 sockets that have not entered a room yet at any instant, 20 new handshakes per second (burst 50). A browser that is refused retries by itself for about 4 s (the relay answers 503 and the browser backend retries); a native client gets `ERROR(LIMIT)` (`VNET_ELIMIT`) for the caps on members and rooms. Raise the flags if you expect more, and run the relay behind `-trust-forwarded-for` so that browsers behind a proxy are counted by their own addresses.

Other notes:

- One relay process is one failure domain: a restart drops all rooms (clients see `VNET_ECLOSED` within one keepalive interval).
- Capacity: one core handles the 200-client load test (50 rooms of 4, 30 packets per second each) at about 10% CPU and 6 MB of memory over UDP, 23% and 21 MB over WebSockets; see `scripts/loadtest.sh`.
- Run it unprivileged. It needs no files and no outbound connections.

## Development

```sh
cd relay && go vet ./... && go test -race -count=1 ./...
go test -run XXX -fuzz FuzzHandleUDP -fuzztime 30s .      # the fuzz target
scripts/loadtest.sh                                         # from the repo root: 50 rooms x 4 clients x 30 pps
```

The hub is transport-agnostic (`hub.go`); the UDP loop is `server.go`, the WebSocket handler `ws.go`. Tests use a fake clock for timeouts and rate limits (no sleeping) and real sockets for interoperability.
