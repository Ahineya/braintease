#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/network/ripple-net-vmnet}"
[[ "$(uname -s)" == Darwin ]] || { echo "vmnet requires macOS" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
NET_SDK="$(xcrun --sdk macosx --show-sdk-path)"
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fblocks \
    -isysroot "$NET_SDK" "$ROOT/tools/network/vmnet.c" -framework vmnet -o "$OUT"
echo "$OUT"
