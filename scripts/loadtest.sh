#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Load sanity test: one relay process, 50 rooms x 4 clients x 30 packets/s
# each (see relay/cmd/vnet-loadtest). Reports delivery, latency, and the relay
# process's CPU and memory. Not part of CI.
#
#   scripts/loadtest.sh [build-dir] [extra vnet-loadtest flags...]
#   scripts/loadtest.sh /tmp/lt -duration 60s -ws-every 1     # all WebSocket
#   scripts/loadtest.sh /tmp/lt -fanout 3                     # 3 destinations per tick
#   scripts/loadtest.sh /tmp/lt -wt-every 1                   # all WebTransport (QUIC datagrams)
#   scripts/loadtest.sh /tmp/lt -wt-every 2 -ws-every 3       # a mix of all three
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(realpath -m "${1:-$root/build-load}")"
mkdir -p "$out"
(cd "$root/relay" && CGO_ENABLED=0 go build -trimpath -o "$out/vnet-relay" ./cmd/vnet-relay &&
    go build -o "$out/vnet-loadtest" ./cmd/vnet-loadtest)

# 200 clients start from one IP: lift the per-source-address limits for the test.
"$out/vnet-relay" -udp 127.0.0.1:0 -http 127.0.0.1:0 -handshake-rate 1000 -handshake-burst 1000 \
    -max-sessions-per-ip 1000 -max-rooms-per-ip 1000 -max-ws-per-ip 1000 -max-pending-ws-per-ip 1000 \
    -max-wt-per-ip 1000 -max-pending-wt-per-ip 1000 -wt 127.0.0.1:0 -dev-cert >"$out/relay.out" 2>"$out/relay.err" &
relay=$!
trap 'kill $relay 2>/dev/null || true' EXIT
for _ in $(seq 200); do grep -q '^LISTEN' "$out/relay.out" && break; sleep 0.05; done
udp="$(sed -n 's/^LISTEN udp=\([^ ]*\) .*/\1/p' "$out/relay.out")"
http="$(sed -n 's/^LISTEN udp=[^ ]* http=\([^ ]*\).*/\1/p' "$out/relay.out")"
wt="$(sed -n 's/^LISTEN .* wt=\([^ ]*\) .*/\1/p' "$out/relay.out")"
hash="$(sed -n 's/^LISTEN .* wt_cert_sha256=\([0-9a-f]*\).*/\1/p' "$out/relay.out")"
"$out/vnet-loadtest" -udp "$udp" -ws "ws://$http/vnet" -wt "https://$wt/vnet" -wt-hash "$hash" -pid "$relay" "${@:2}"
