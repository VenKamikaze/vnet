#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Windows check for the relay backend: cross-compiles examples/relay_peer.c
# with MinGW-w64 and runs relay_peer.exe under Wine against the real Go relay,
# with a native Linux relay_peer as the other player, in both directions
# (Windows guest, Windows host). This exercises the Winsock code paths of
# the relay UDP backend (sockets, host name lookup, rand_s).
#
# Needs: go, cmake, mingw-w64, wine64.   Usage: scripts/test-mingw-relay.sh [build-dir]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(realpath -m "${1:-$root/build-mingw-relay}")"
mkdir -p "$out"
WINE="$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)"
export WINEDEBUG=-all
[ -n "${WINEPREFIX:-}" ] || export WINEPREFIX="$out/wineprefix"

(cd "$root/relay" && CGO_ENABLED=0 go build -trimpath -o "$out/vnet-relay" ./cmd/vnet-relay)

cmake -S "$root" -B "$out/linux" -DCMAKE_BUILD_TYPE=Release \
    -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_RELAY=OFF -DVNET_INSTALL=OFF >/dev/null
cmake --build "$out/linux" --target relay_peer -j 4 >/dev/null

cmake -S "$root" -B "$out/win" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_RELAY=OFF -DVNET_INSTALL=OFF \
    -DVNET_WARNINGS_AS_ERRORS=ON >/dev/null
cmake --build "$out/win" --target relay_peer -j 4 >/dev/null

# The first Wine start creates the prefix, which can take longer than a peer's
# timeout: do it before any peer is running.
"$WINE" "$out/win/relay_peer.exe" --help >/dev/null 2>&1 || true

"$out/vnet-relay" -udp 127.0.0.1:0 -http 127.0.0.1:0 >"$out/relay.out" &
relay=$!
trap 'kill $relay 2>/dev/null || true' EXIT
for _ in $(seq 200); do grep -q '^LISTEN' "$out/relay.out" && break; sleep 0.05; done
udp="$(sed -n 's/^LISTEN udp=\([^ ]*\) .*/\1/p' "$out/relay.out")"
[ -n "$udp" ] || { echo "relay did not start"; exit 1; }
echo "relay on $udp"

linux_peer() { "$out/linux/relay_peer" --relay "$udp" --count 200 --timeout 40000 "$@"; }
win_peer() { "$WINE" "$out/win/relay_peer.exe" --relay "$udp" --count 200 --timeout 40000 "$@"; }

# Wait (bounded) for a "ROOM <code> ..." line in a log file; print the code.
room_code() {
    for _ in $(seq 600); do
        code="$(sed -n 's/^ROOM \([A-Z0-9]*\) .*/\1/p' "$1" | tr -d '\r' | head -1)"
        [ -z "$code" ] || { echo "$code"; return 0; }
        sleep 0.1
    done
    return 1
}
ok() { grep -q '^RESULT {"ok":true' "$1" || { echo "FAILED: $1"; cat "$1"; return 1; }; }

echo "== Windows guest, Linux host"
linux_peer >"$out/host1.log" &
host=$!
code="$(room_code "$out/host1.log")"
echo "room $code"
win_peer --room "$code" >"$out/guest1.log"
wait "$host"
ok "$out/host1.log"; ok "$out/guest1.log"
grep -h '^ROOM\|^RESULT' "$out/host1.log" "$out/guest1.log"

echo "== Windows host, Linux guest"
win_peer >"$out/host2.log" &
host=$!
code="$(room_code "$out/host2.log")"
echo "room $code"
linux_peer --room "$code" >"$out/guest2.log"
wait "$host"
ok "$out/host2.log"; ok "$out/guest2.log"
grep -h '^ROOM\|^RESULT' "$out/host2.log" "$out/guest2.log"
echo "MinGW relay_peer.exe: ok"
