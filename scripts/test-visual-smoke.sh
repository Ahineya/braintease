#!/usr/bin/env bash
# Rust-only visual/device checks. Pass an optional WAD for the TITLEPIC check.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${VISUAL_SMOKE_OUT:-$ROOT/c-test/build/visual-smoke}"
WAD="${1:-${DOOM_WAD:-}}"
if [[ -n "$WAD" ]]; then
    if [[ ! -f "$WAD" ]]; then echo "WAD not found: $WAD" >&2; exit 1; fi
    WAD="$(cd "$(dirname "$WAD")" && pwd)/$(basename "$WAD")"
fi
RCC="$ROOT/target/release/rcc"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
RVM="$ROOT/target/release/rvm"
RCT="$ROOT/target/release/rct"

for tool in "$RCC" "$RASM" "$RLINK" "$RVM" "$RCT"; do
    if [[ ! -x "$tool" ]]; then
        echo "Missing tool: $tool" >&2
        exit 1
    fi
done
mkdir -p "$OUT/build"
OUT="$(cd "$OUT" && pwd)"
cd "$ROOT"

"$RCT" test_vm_devices --backend rvm --timeout 30 --no-parallel \
    --build-dir "$OUT/build" > "$OUT/c-runtime.log" 2>&1 || {
    cat "$OUT/c-runtime.log" >&2
    exit 1
}

# Independent expected pixels catch wrong colors, orientation, stride or banks.
node --input-type=module - "$OUT" <<'JS_PIXELS'
import fs from 'node:fs';
import path from 'node:path';
const out = process.argv[2];
const pixels = Buffer.alloc(64000 * 2);
for (let i = 0; i < 64000; i++) pixels.writeUInt16LE(i < 32000 ? 0xf800 : 0x001f, i * 2);
for (const [index, color] of [[0, 0xffff], [319, 0x07e0], [63680, 0xffe0], [63999, 0x07ff]]) {
    pixels.writeUInt16LE(color, index * 2);
}
fs.writeFileSync(path.join(out, 'devices.expected.rgb565'), pixels);
const input = Buffer.alloc(16);
for (let i = 0; i < 8; i++) input.writeUInt16LE(0x001f, i * 2);
fs.writeFileSync(path.join(out, 'input.expected.rgb565'), input);
JS_PIXELS

"$RVM" "$OUT/build/test_vm_devices.bin" --headless --memory 2000000 \
    --disk "$OUT/devices.disk.img" --seed 1 --clock unthrottled \
    --screenshot "$OUT/devices.png" --raw-frame "$OUT/devices.rgb565" \
    --expect-frame "$OUT/devices.expected.rgb565" --report "$OUT/devices.json" \
    --max-instructions 100000000 --timeout 30

"$RCC" compile "$ROOT/c-test/fixtures/devices/input_replay.c" \
    -o "$OUT/input_replay.asm" -I "$ROOT/runtime/include" --bank-size 64000 \
    > "$OUT/input-compile.log" 2>&1 || { cat "$OUT/input-compile.log" >&2; exit 1; }
"$RASM" assemble "$OUT/input_replay.asm" -o "$OUT/input_replay.pobj" \
    --bank-size 64000 --max-immediate 65535
"$RLINK" "$ROOT/runtime/crt0.pobj" "$ROOT/runtime/libruntime.par" \
    "$OUT/input_replay.pobj" -f binary --bank-size 64000 -o "$OUT/input_replay.bin"

for mode in unthrottled stepped control; do
    clock="$mode"
    extra=(--input-script "$ROOT/c-test/fixtures/devices/input_replay.json")
    control_file="$ROOT/c-test/fixtures/devices/tick_grants.jsonl"
    if [[ "$mode" == stepped ]]; then extra+=(--control-stdin); fi
    if [[ "$mode" == control ]]; then
        clock=stepped
        extra=(--control-stdin)
        control_file="$ROOT/c-test/fixtures/devices/input_control.jsonl"
    fi
    "$RVM" "$OUT/input_replay.bin" --headless --memory 2000000 \
        --disk "$OUT/input-$mode.disk.img" --seed 1 --clock "$clock" "${extra[@]}" \
        --screenshot "$OUT/input-$mode.png" --raw-frame "$OUT/input-$mode.rgb565" \
        --capture-frame 3 --expect-frame "$OUT/input.expected.rgb565" \
        --report "$OUT/input-$mode.json" --max-instructions 10000000 --timeout 30 \
        < "$control_file"
done
cmp "$OUT/input-unthrottled.rgb565" "$OUT/input-stepped.rgb565"
cmp "$OUT/input-unthrottled.rgb565" "$OUT/input-control.rgb565"

if [[ -n "$WAD" ]]; then
    if [[ ! -f "$WAD" ]]; then echo "WAD not found: $WAD" >&2; exit 1; fi
    # The VM opens disk images writable, so keep the source fixture untouched.
    cp "$WAD" "$OUT/titlepic.wad"
    make -C "$ROOT/ports/doom/tests/titlepic" BUILD="$OUT/doom-build" \
        > "$OUT/titlepic-build.log" 2>&1 || { cat "$OUT/titlepic-build.log" >&2; exit 1; }
    "$RVM" "$OUT/doom-build/doom-c.bin" --headless --memory 2000000 \
        --disk "$OUT/titlepic.wad" --seed 1 --clock unthrottled \
        --screenshot "$OUT/titlepic.png" --raw-frame "$OUT/titlepic.rgb565" \
        --report "$OUT/titlepic.json" --max-instructions 500000000 --timeout 60
    "$RVM" "$OUT/doom-build/doom-c.bin" --headless --memory 2000000 \
        --disk "$OUT/titlepic.wad" --seed 1 --clock unthrottled \
        --screenshot "$OUT/titlepic-repeat.png" --expect-frame "$OUT/titlepic.rgb565" \
        --report "$OUT/titlepic-repeat.json" --max-instructions 500000000 --timeout 60
fi

echo "Visual smoke checks passed. Artifacts: $OUT"
