#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Loss behaviour of WebTransport against WebSocket, in Chromium, through a
# relay on loopback (a measurement, not a test; results are in
# doco/11-vnet-webtransport.md). WebTransport runs with 10% of the QUIC packets
# dropped by a user-space UDP proxy; WebSocket cannot be degraded that way
# without `tc netem` (NET_ADMIN and CONFIG_NET_SCH_NETEM), so the sender drops
# 10% itself. Needs go, node and playwright (NODE_PATH).
#
#   scripts/measure-transport-loss.sh [build-dir] [--loss 10] [--rate 100] [--seconds 10] [--size 200]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(realpath -m "${1:-$root/build-loss}")"
mkdir -p "$out"
(cd "$root/relay" && CGO_ENABLED=0 go build -trimpath -o "$out/vnet-relay" ./cmd/vnet-relay)
exec node "$root/tests/browser/transport_loss.js" --relay-bin "$out/vnet-relay" "${@:2}"
