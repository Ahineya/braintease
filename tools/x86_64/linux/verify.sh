#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -m)" == x86_64 ]] || { echo 'Expected an x86_64 Linux container' >&2; exit 1; }
cd /repo
rm -f /out/result.json
printf 'Platform: %s\n' "$(uname -sm)"
rustc --version
cargo build --locked --release -p rcc-driver -p rcc-native-runtime
cargo test --locked --release -p rcc-driver --test native --test native_process

build=/out/hosted
make -C fuse-x -j4 BUILD="$build" \
    "$build/fuse-x" "$build/apps/native-hello.fxe"
python3 fuse-x/apps.py sh cat ls cp echo sleep ps --build "$build"
python3 tools/x86_64/linux/smoke.py --build "$build"
