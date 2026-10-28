#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Browser integration test: builds the Go relay, the native relay_peer and the
# Emscripten relay_peer (wasm), then runs tests/browser/relay_browser.test.js,
# which drives Chromium with Playwright against a relay started on loopback.
#
# Prerequisites: go, cmake, node with `playwright` resolvable by require()
# (NODE_PATH), a Chromium that Playwright can launch, and an activated emsdk
# (run scripts/setup-emsdk.sh once; this script sources it if emcc is not on
# PATH). Usage: scripts/test-browser.sh [build-dir]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(realpath -m "${1:-$root/build-browser}")"
mkdir -p "$out"

if ! command -v emcc >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source "${EMSDK_DIR:-$root/.emsdk}/emsdk_env.sh" >/dev/null
fi
echo "emcc: $(emcc --version | head -1)"

echo "== Go relay"
(cd "$root/relay" && CGO_ENABLED=0 go build -trimpath -o "$out/vnet-relay" ./cmd/vnet-relay)

echo "== native relay_peer"
cmake -S "$root" -B "$out/native" -DCMAKE_BUILD_TYPE=Release \
    -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_RELAY=OFF -DVNET_INSTALL=OFF -DVNET_WARNINGS_AS_ERRORS=ON >/dev/null
cmake --build "$out/native" --target relay_peer -j 4

echo "== wasm relay_peer"
emcmake cmake -S "$root" -B "$out/wasm" -DCMAKE_BUILD_TYPE=Release \
    -DVNET_BUILD_TESTS=OFF -DVNET_BUILD_RELAY=OFF -DVNET_INSTALL=OFF -DVNET_WARNINGS_AS_ERRORS=ON >/dev/null
cmake --build "$out/wasm" --target relay_peer -j 4

echo "== Chromium"
node "$root/tests/browser/relay_browser.test.js" \
    --relay-bin "$out/vnet-relay" --peer-bin "$out/native/relay_peer" --web-dir "$out/wasm" "${@:2}"
