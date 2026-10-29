# vnet relay

One static Go binary that lets browser and native players share a room and exchange vnet datagrams as if they were on one LAN. Rooms hand out virtual addresses `10.0.0.N` (the creator is always `10.0.0.1`), and datagrams are forwarded **only between members of the same room**: the relay is not a proxy. The wire format is in [`doco/relay-protocol.md`](../doco/relay-protocol.md).

- native clients: UDP (default port 7777) with the `VNET_BACKEND_RELAY_UDP` backend;
- browsers: a WebSocket at `/vnet` (default HTTP port 8080) with `VNET_BACKEND_RELAY_WS` (Emscripten builds);
- browsers, optionally: WebTransport datagrams at `/vnet` over HTTP/3 (UDP, off unless `-wt` is given) with `VNET_BACKEND_RELAY_WT`, or `VNET_BACKEND_RELAY_AUTO`, which tries WebTransport and falls back to the WebSocket. See [WebTransport](#webtransport-http3-datagrams).

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

On start it prints one line, `LISTEN udp=<addr> http=<addr> stats=<addr> wt=<addr> wt_cert_sha256=<hex>` (the bound addresses, `stats=` and `wt=` empty when off; `wt_cert_sha256=` only with `-dev-cert`; use `-udp 127.0.0.1:0` for a free port), which scripts and tests read.

Every flag can also be set from the environment as `VNET_RELAY_<NAME>` (upper case, `-` becomes `_`: `VNET_RELAY_MAX_MEMBERS=4`); the command line wins.

| Flag | Default | Meaning |
|---|---|---|
| `-udp` | `:7777` | UDP listen address; empty disables native clients |
| `-http` | `:8080` | HTTP listen address; empty disables WebSocket. Serves `/vnet`, `/healthz`, `/stats` |
| `-max-members` | 8 | members per room, creator included (2..254) |
| `-max-rooms` | 1000 | rooms at once |
| `-max-sessions` | 20000 | members at once (also caps open WebSockets and WebTransport connections, together) |
| `-max-wt` | 4000 | WebTransport (QUIC) connections at once, with or without a session, from anywhere. Each costs roughly 150 to 250 KB of memory, far more than a WebSocket, so this cap is separate from (and below) `-max-sessions`; see [sizing](#sizing-webtransport) |
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
| `-origins` | `*` | allowed `Origin` hosts for WebSockets and WebTransport sessions, comma separated |
| `-wt` | off | UDP listen address for WebTransport (HTTP/3), e.g. `:4433` or `:443`. Needs `-wt-cert` and `-wt-key`, or `-dev-cert` |
| `-wt-cert`, `-wt-key` | | the TLS certificate chain and key (PEM) for `-wt`; re-read when the files change (checked at most once a minute, on a handshake), so a renewal needs no restart. A pair that fails to load is ignored and the old one is kept |
| `-dev-cert` | off | **development only**: generate a 13-day ECDSA P-256 certificate and print its SHA-256 (the page passes it as `serverCertificateHashes`). The hash changes at every start and the certificate expires after 13 days (the relay warns from the sixth day and logs an error once it has expired). A warning is logged if `-wt` is not a loopback address |
| `-dev-cert-names` | | extra host names or IP addresses for the `-dev-cert` certificate (`localhost`, `127.0.0.1` and `::1` are always there) |
| `-max-wt-per-ip` | 128 | QUIC connections per source address, with or without a session. Counted from the moment the Initial packet is accepted; shared by everyone behind one NAT |
| `-max-pending-wt-per-ip` | 32 | of those, the ones that have not entered a room yet (including connections that never send an HTTP/3 request) |
| `-wt-queue-bytes` | 65536 | bytes of datagrams queued per WebTransport client before its datagrams are dropped (and at most 128 datagrams) |
| `-trust-forwarded-for` | off | use the rightmost `X-Forwarded-For` entry as the client IP (only behind a proxy you control) |
| `-stats` | off | serve `/stats` on this address, e.g. `127.0.0.1:8081` |
| `-v` | off | log room creation and closure |

The public HTTP listener serves only `/vnet` and `/healthz`; the WebTransport listener serves only `CONNECT /vnet`. `/stats` (counters as JSON: rooms, members, frames, drops by reason, path validations) is **off by default**: give `-stats` a private address (localhost or an internal interface) to turn it on.

If `/stats` shows `panics` > 0, restart the relay soon: a recovered panic is not guaranteed to have left its state consistent.

## WebTransport (HTTP/3 datagrams)

WebTransport lets a browser send **unreliable datagrams** to the relay, so one lost packet does not hold up the ones behind it (as a lost TCP segment does for a WebSocket). It is the better transport for client/server games and for lockstep play on lossy links, but it needs more from the host than a WebSocket does; see the comparison table in the [top-level README](../README.md#transports-compared). The browser backend `VNET_BACKEND_RELAY_AUTO` uses it where it works and falls back to the WebSocket where it does not, so **keep the WebSocket endpoint running**.

### Development: a certificate pinned by hash

```sh
./vnet-relay -udp :7777 -http :8080 -wt 127.0.0.1:4433 -dev-cert
# 2026/10/28 23:10:39 WebTransport DEVELOPMENT certificate (local trials only, NOT FOR PRODUCTION): SHA-256 f5da9d11..., valid 312h0m0s from now, a new one at every start; ...
# LISTEN udp=[::]:7777 http=[::]:8080 stats= wt=127.0.0.1:4433 wt_cert_sha256=f5da9d11...
```

The page gives that hash to the browser (`serverCertificateHashes`); vnet takes it in `vnet_config.relay_cert_sha256`:

```c
cfg.backend = VNET_BACKEND_RELAY_AUTO;
cfg.relay = "ws://127.0.0.1:8080/vnet";            // the WebSocket fallback
cfg.relay_wt = "https://127.0.0.1:4433/vnet";      // the WebTransport URL (derived from a wss:// relay)
cfg.relay_cert_sha256 = "f5da9d11...";             // dev only; NULL in production
```

Chromium needs **no flags** for this (checked with Chromium 153 under Playwright: `new WebTransport("https://127.0.0.1:PORT/vnet", {serverCertificateHashes: [...]})` connects to a loopback relay in a default launch, headless included). The browser accepts a certificate by hash only if it is X.509v3, valid now, **valid for less than two weeks**, and not RSA; `-dev-cert` makes a 13-day ECDSA P-256 certificate, so a relay left running for more than about two weeks needs a restart. **The certificate, and so its hash, is new at every start**: a page built with yesterday's hash cannot connect after a restart (it falls back to the WebSocket under `RELAY_AUTO`); read the hash from the `LISTEN` line or the log each time, as `scripts` and the browser tests do. The relay logs a warning from the sixth day on and an error once the certificate has expired, and a warning at start-up if `-wt` is not bound to a loopback address. Never use `-dev-cert` for players. The page must be a secure context (`https://` or `http://localhost`).

### Production

1. **A real certificate** for the relay's host name, from any public CA: Let's Encrypt via certbot, Caddy's or another ACME client's certificate files, or your own. Give the relay `-wt-cert fullchain.pem -wt-key privkey.pem`; it re-reads the files when they change, so renewals need no restart (and no `serverCertificateHashes`; leave `relay_cert_sha256` NULL). The relay does not do ACME itself: it would have to answer challenges on TCP 80 or 443, which on a typical host belong to the reverse proxy.
2. **Open a UDP port** in the firewall and the cloud security group: `-wt :443` if the host has no other HTTP/3 server on UDP 443 (binding 443 needs `CAP_NET_BIND_SERVICE`, or run the relay on `:4433` and use that port in the URL), and make sure it is **UDP**, not TCP. Corporate networks and some mobile carriers block outgoing UDP/443; those clients fall back to the WebSocket by themselves after `wt_timeout_ms` (3 s).
3. **Same host name, two transports.** With the WebSocket behind Caddy/nginx on TCP 443 (above), `wss://relay.example.org/vnet` and a relay serving WebTransport on UDP 443 (or the port given in `relay_wt`) share a name; `VNET_BACKEND_RELAY_AUTO` with `relay = "wss://relay.example.org/vnet"` derives `https://relay.example.org/vnet` for WebTransport. A different port needs `relay_wt = "https://relay.example.org:4433/vnet"`. A general-purpose reverse proxy cannot be assumed to forward WebTransport (HTTP/3 extended `CONNECT` plus datagrams): this relay terminates QUIC itself, and that is the configuration that was tested.
4. **UDP socket buffers.** quic-go asks the kernel for 7 MiB buffers and logs a warning if it does not get them. On Linux: `sysctl -w net.core.rmem_max=7500000 net.core.wmem_max=7500000` (persist it in `/etc/sysctl.d/`). Without it the relay still works, with less headroom under bursts.
5. **Per-address limits and NAT.** The relay sees the QUIC connection's source address (there is no `X-Forwarded-For` for UDP), so `-max-wt-per-ip` and `-max-pending-wt-per-ip` count everyone behind one NAT together, exactly like the WebSocket ones. A client over a limit is refused at the QUIC level and falls back to the WebSocket. `-max-wt` bounds all WebTransport connections together, whoever they come from (see [sizing](#sizing-webtransport)).
6. **Origins.** `-origins game.example.org` restricts both WebSocket and WebTransport sessions to pages from that host. The default (`*`) allows any page, as for WebSockets: the relay holds no ambient credentials.
7. **Docker**: add `-p 4433:4433/udp` and mount the certificate directory read-only (the image is `scratch`; it contains no certificates, and it runs as uid 65534):

   ```sh
   docker run --rm -p 7777:7777/udp -p 8080:8080 -p 4433:4433/udp \
       -v /etc/vnet-relay/certs:/certs:ro vnet-relay \
       -wt :4433 -wt-cert /certs/fullchain.pem -wt-key /certs/privkey.pem
   ```

   **File permissions.** Let's Encrypt's `privkey.pem` is `0600 root`, which uid 65534 cannot read, and `live/` holds symlinks into `archive/`, which a single-directory mount does not carry. So do not mount `/etc/letsencrypt/live/...` directly; give the relay a copy it can read, refreshed by a certbot deploy hook (certbot runs it after every renewal; the relay notices the new files within a minute, no restart):

   ```sh
   #!/bin/sh
   # /etc/letsencrypt/renewal-hooks/deploy/vnet-relay.sh   (chmod +x)
   set -eu
   d=/etc/vnet-relay/certs
   install -d -m 750 -o root -g 65534 "$d"
   install -m 644 -o root -g 65534 "$RENEWED_LINEAGE/fullchain.pem" "$d/fullchain.pem"
   install -m 640 -o root -g 65534 "$RENEWED_LINEAGE/privkey.pem"   "$d/privkey.pem"
   ```

   (Run it once by hand with `RENEWED_LINEAGE=/etc/letsencrypt/live/relay.example.org` to create the first copy.) Without Docker, run the relay as its own user and give that user's group read access to the same copy, or use `setfacl -m u:vnet:r`. The private key then exists in two places: treat `/etc/vnet-relay/certs` like `/etc/letsencrypt`.
8. **Watch the expiry.** The relay logs a `WARNING` at every handshake (at most once an hour) when its certificate has less than 7 days left, and an `ERROR` once it has expired: a renewal that silently fails shows up as WebTransport clients falling back to the WebSocket, and in the log. Browsers refuse an expired certificate.

### Sizing WebTransport

A WebTransport member is a QUIC connection with several goroutines and buffers, not just a socket entry. Measured on one relay process (load generator on the same machine, 100-byte payloads): **about 150 KB per connection** with 1000 mostly idle clients (1 packet per second each: 157 MB resident in all) and **about 250 KB per connection** at 30 packets per second per client (200 clients: 60 MB, 56% of one core, against 9 MB and 9% for UDP). A connection can also hold up to 128 KiB of QUIC receive window and 64 KiB of queued datagrams in the worst case, so budget **250 KB to 300 KB per connection**:

| `-max-wt` | memory to budget |
|---|---|
| 1000 | about 300 MB |
| 4000 (default) | about 1.2 GB |
| 10000 | about 3 GB |

`-max-sessions` (20000) is shared with WebSockets and would allow 5 GB of WebTransport connections, which is why `-max-wt` is lower and separate: the per-address caps alone do not bound it (157 addresses at 128 connections each, or about 40 IPv6 /48s, can hold the lot indefinitely). Size `-max-wt` to the host, and remember the CPU: every datagram is one encrypted packet and one system call. A client refused by `-max-wt` or by a per-address cap falls back to the WebSocket, so a full WebTransport relay degrades into a WebSocket one rather than failing.

### What happens to a WebTransport member that goes quiet

The relay pings every connection (QUIC keep-alive) and QUIC's idle timeout is `session-timeout` (15 s), so a vanished client is dropped within about 15 s, and an idle but connected member (a hidden tab) is kept, as with WebSocket pings. See [`doco/relay-protocol.md`](../doco/relay-protocol.md#webtransport).

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

**Limits that players behind one address share (for game developers, e.g. Bit Riot brief 12):** every cap above is per source address, so a classroom, a LAN party or a carrier-grade NAT with many players on one public address shares one budget: 128 members, 16 rooms, 128 open WebSockets and 32 sockets that have not entered a room yet at any instant, and the same two numbers again for WebTransport (128 QUIC connections, 32 of them not yet in a room), 20 new handshakes per second (burst 50). A browser that is refused retries by itself for about 4 s (the relay answers 503 and the browser backend retries); a native client gets `ERROR(LIMIT)` (`VNET_ELIMIT`) for the caps on members and rooms. Raise the flags if you expect more, and run the relay behind `-trust-forwarded-for` so that browsers behind a proxy are counted by their own addresses.

Other notes:

- One relay process is one failure domain: a restart drops all rooms (clients see `VNET_ECLOSED` within one keepalive interval).
- Capacity: one core handles the 200-client load test (50 rooms of 4, 30 packets per second each) at about 10% CPU and 6 MB of memory over UDP, 23% and 21 MB over WebSockets; see `scripts/loadtest.sh`.
- Run it unprivileged. It needs no files and no outbound connections.

## Development

```sh
cd relay && go vet ./... && go test -race -count=1 ./...
go test -run XXX -fuzz FuzzHandleUDP -fuzztime 30s .      # the fuzz targets: FuzzHandleUDP, FuzzHandleWS, FuzzHandleWT
scripts/loadtest.sh                                         # from the repo root: 50 rooms x 4 clients x 30 pps
```

The hub is transport-agnostic (`hub.go`); the UDP loop is `server.go`, the WebSocket handler `ws.go`, WebTransport `wt.go` (certificates: `certs.go`; `conn.go` is the interface the hub sees a stream transport through). Tests use a fake clock for timeouts and rate limits (no sleeping) and real sockets for interoperability. The WebTransport tests have two layers: `wt_test.go` runs the relay against a fake session (a datagram queue that blocks, a control stream stuck in `Write`, overlapping writes, hostile bytes), `wt_e2e_test.go` uses real QUIC with webtransport-go clients that accept the development certificate by hash.
