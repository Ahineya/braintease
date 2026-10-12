#!/usr/bin/env bash
# Bounded, small Brainfuck CPU/device checks. Never builds or executes Doom.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BF_SMOKE_OUT:-$ROOT/build/bf-smoke}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RBF="$ROOT/target/release/rbf"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
BFM="$ROOT/src/bf-macro-expander/target/release/bfm"
for tool in "$RBF" "$RASM" "$RLINK" "$BFM" "$ROOT/target/release/rcc"; do
    [[ -x "$tool" ]] || { echo "Missing tool: $tool (see rbf/README.md)" >&2; exit 1; }
done
for fixture in devices legacy io; do
    BANK_SIZE=512 MEMORY_BANKS=4 "$ROOT/scripts/build-bf.sh" "$ROOT/rbf/tests/fixtures/$fixture.asm" "$OUT/$fixture" > "$OUT/$fixture-build.log" 2>&1 || { cat "$OUT/$fixture-build.log" >&2; exit 1; }
done
BANK_SIZE=37 MEMORY_BANKS=4 "$ROOT/scripts/build-bf.sh" "$ROOT/rbf/tests/fixtures/multibank.asm" "$OUT/multibank" > "$OUT/multibank-build.log" 2>&1 || { cat "$OUT/multibank-build.log" >&2; exit 1; }
BANK_SIZE=2048 MEMORY_BANKS=1 "$ROOT/scripts/build-bf.sh" "$ROOT/rbf/tests/fixtures/text40.asm" "$OUT/text40" > "$OUT/text40-build.log" 2>&1 || { cat "$OUT/text40-build.log" >&2; exit 1; }
BANK_SIZE=512 MEMORY_BANKS=1 "$ROOT/scripts/build-bf.sh" "$ROOT/rbf/examples/rgb565.asm" "$OUT/rgb565" > "$OUT/rgb565-build.log" 2>&1 || { cat "$OUT/rgb565-build.log" >&2; exit 1; }
node --input-type=module - "$OUT" <<'JS'
import fs from 'node:fs';
import path from 'node:path';
const out = process.argv[2];
fs.writeFileSync(path.join(out, 'io.disk.img'), Buffer.from([0x34,0x12,0x56]));
for (const [name, pixels] of [['devices',[0xf800,0x07e0,0x001f,0xffff]],['legacy',[0xf800,0x001f]]]) {
    const data = Buffer.alloc(pixels.length * 2);
    pixels.forEach((p,i) => data.writeUInt16LE(p,i*2));
    fs.writeFileSync(path.join(out, `${name}.expected.rgb565`), data);
}
JS
for clock in unthrottled stepped; do
    args=(--input-script "$ROOT/rbf/tests/fixtures/keys.json")
    if [[ "$clock" == stepped ]]; then args+=(--control-stdin); fi
    "$RBF" "$OUT/devices.bf" --headless --clock "$clock" "${args[@]}" \
        --screenshot "$OUT/devices-$clock.png" --raw-frame "$OUT/devices-$clock.rgb565" \
        --expect-frame "$OUT/devices.expected.rgb565" --report "$OUT/devices-$clock.json" \
        --max-dispatches 3000000000 --max-instructions 1000 --timeout 30 \
        <<< '{"op":"step","ticks":1}' > "$OUT/devices-$clock.stdout"
    [[ "$(cat "$OUT/devices-$clock.stdout")" == Y ]] || { echo "Device assertions failed" >&2; exit 1; }
done
"$RBF" "$OUT/legacy.bf" --headless --screenshot "$OUT/legacy.png" \
    --raw-frame "$OUT/legacy.rgb565" --expect-frame "$OUT/legacy.expected.rgb565" \
    --report "$OUT/legacy.json" --max-dispatches 3000000000 --timeout 30
"$RBF" "$OUT/io.bf" --headless --seed 1 --input AZ --disk "$OUT/io.disk.img" \
    --exit-code --report "$OUT/io.json" --max-dispatches 1000000000 \
    --max-instructions 1000 --timeout 15 > "$OUT/io.stdout"
[[ "$(cat "$OUT/io.stdout")" == Y ]] || { echo "I/O assertions failed" >&2; exit 1; }
"$RBF" "$OUT/multibank.bf" --headless --exit-code --report "$OUT/multibank.json" \
    --max-dispatches 1000000000 --max-instructions 1000 --timeout 30 > "$OUT/multibank.stdout"
[[ "$(cat "$OUT/multibank.stdout")" == Y ]] || { echo "Code-bank assertions failed" >&2; exit 1; }
"$RBF" "$OUT/text40.bf" --headless --text-frame "$OUT/text40.cells" \
    --report "$OUT/text40.json" --max-dispatches 1000000000 --max-instructions 1000 --timeout 30

# The public animation must show a complete first frame, then move the marker
# without stale pixels after swapping buffers and wrapping around the screen.
for frame in 1 3 4 19; do
    "$RBF" "$OUT/rgb565.bf" --headless --clock unthrottled --capture-frame "$frame" \
        --screenshot "$OUT/rgb565-$frame.png" --raw-frame "$OUT/rgb565-$frame.rgb565" \
        --report "$OUT/rgb565-$frame.json" --max-dispatches 1000000000 \
        --max-instructions 10000 --timeout 30
done

# Real compiled C exercises startup, separate stack/global banks, and multiple code banks.
BANK_SIZE=64 MEMORY_BANKS=4 "$ROOT/scripts/build-bf.sh" "$ROOT/rbf/tests/fixtures/tiny.c" "$OUT/tiny" > "$OUT/tiny-build.log" 2>&1 || { cat "$OUT/tiny-build.log" >&2; exit 1; }
"$RBF" "$OUT/tiny.bf" --headless --exit-code --report "$OUT/tiny.json" \
    --max-dispatches 1000000000 --timeout 30 > "$OUT/tiny.stdout"
[[ "$(cat "$OUT/tiny.stdout")" == Y ]] || { echo "C assertions failed" >&2; exit 1; }
node --input-type=module - "$OUT" <<'JS'
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const out = process.argv[2];
assert.deepEqual([...fs.readFileSync(path.join(out,'io.disk.img')).subarray(0,4)], [0x34,0x12,0xcd,0xab]);
const io=JSON.parse(fs.readFileSync(path.join(out,'io.json')));
assert.equal(io.success,true); assert.equal(io.guest_result,0);
for (const clock of ['unthrottled','stepped']) {
    const report=JSON.parse(fs.readFileSync(path.join(out,`devices-${clock}.json`)));
    assert.equal(report.success,true); assert.equal(report.tick,1); assert.equal(report.frames,1);
    assert.equal(report.width,2); assert.equal(report.height,2);
}
const tiny=JSON.parse(fs.readFileSync(path.join(out,'tiny.json')));
assert.equal(tiny.success,true); assert.equal(tiny.stop_reason,'halted'); assert.equal(tiny.guest_result,0);
assert.ok(tiny.layout.instructions > tiny.layout.bank_size,
    'Keep the compiled C fixture spanning code banks as compiler output shrinks');
const multibank=JSON.parse(fs.readFileSync(path.join(out,'multibank.json')));
assert.equal(multibank.success,true); assert.equal(multibank.guest_result,0);
assert.ok(multibank.layout.entry >= multibank.layout.bank_size);
const text=JSON.parse(fs.readFileSync(path.join(out,'text40.json')));
assert.equal(text.success,true); assert.equal(text.frames,1);
assert.equal(text.width,40); assert.equal(text.height,25);
const cells=fs.readFileSync(path.join(out,'text40.cells'));
assert.equal(cells.length,2000);
for (let index=0; index<1000; index++) {
    const expected=index===0 ? 0x1e41 : index===1 ? 0x2f42 : index===999 ? 0x4f5a : 0;
    assert.equal(cells.readUInt16LE(index*2),expected,`TEXT40 cell ${index}`);
}
for (const frame of [1,3,4,19]) {
    const report=JSON.parse(fs.readFileSync(path.join(out,`rgb565-${frame}.json`)));
    assert.equal(report.success,true); assert.equal(report.frames,frame);
    assert.equal(report.width,16); assert.equal(report.height,12);
    const pixels=fs.readFileSync(path.join(out,`rgb565-${frame}.rgb565`));
    assert.equal(pixels.length,16*12*2);
    for (let y=0; y<12; y++) for (let x=0; x<16; x++) {
        let expected=y<4 ? 0xf800 : y<8 ? 0x07e0 : 0x001f;
        if (frame>=3 && x===(frame-3)%16 && (y===5 || y===6)) expected=0xffff;
        assert.equal(pixels.readUInt16LE((y*16+x)*2),expected,`RGB565 frame ${frame} pixel ${x},${y}`);
    }
}
JS
echo "Brainfuck host smoke checks passed. Artifacts: $OUT"
