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
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(realpath -m "${1:-$root/build-load}")"
mkdir -p "$out"
(cd "$root/relay" && CGO_ENABLED=0 go build -trimpath -o "$out/vnet-relay" ./cmd/vnet-relay &&
    go build -o "$out/vnet-loadtest" ./cmd/vnet-loadtest)

# 200 clients start from one IP: lift the per-IP handshake limit for the test.
"$out/vnet-relay" -udp 127.0.0.1:0 -http 127.0.0.1:0 -handshake-rate 1000 -handshake-burst 1000 >"$out/relay.out" &
relay=$!
trap 'kill $relay 2>/dev/null || true' EXIT
for _ in $(seq 200); do grep -q '^LISTEN' "$out/relay.out" && break; sleep 0.05; done
udp="$(sed -n 's/^LISTEN udp=\([^ ]*\) .*/\1/p' "$out/relay.out")"
http="$(sed -n 's/^LISTEN udp=[^ ]* http=\(.*\)/\1/p' "$out/relay.out")"
"$out/vnet-loadtest" -udp "$udp" -ws "ws://$http/vnet" -pid "$relay" "${@:2}"
