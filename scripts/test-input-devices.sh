#!/usr/bin/env bash
# Verify the C input contract through the VM interpreter/JIT and native runtime.
# Build rvm, rcc and rcc-native-runtime in release, and make -C runtime first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${INPUT_SMOKE_OUT:-$ROOT/build/input-smoke}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RCC="$ROOT/target/release/rcc"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
RVM="$ROOT/target/release/rvm"
SOURCE="$ROOT/c-test/fixtures/devices/pointer_replay.c"
SCRIPT="$ROOT/c-test/fixtures/devices/pointer_replay.json"
"$RCC" compile "$SOURCE" -I "$ROOT/runtime/include" --bank-size 64000 -o "$OUT/pointer.asm"
"$RASM" assemble "$OUT/pointer.asm" --bank-size 64000 --max-immediate 65535 -o "$OUT/pointer.pobj"
"$RLINK" "$ROOT/runtime/crt0.pobj" "$ROOT/runtime/libruntime.par" "$OUT/pointer.pobj" \
    -f binary --bank-size 64000 -o "$OUT/pointer.bin"
for engine in interpreter jit; do
    extra=(--clock unthrottled)
    if [[ "$engine" == jit ]]; then extra+=(--jit); fi
    "$RVM" "$OUT/pointer.bin" --headless --memory 2000000 --disk "$OUT/$engine.disk.img" \
        --input-script "$SCRIPT" --exit-code "${extra[@]}" \
        --max-instructions 10000000 --timeout 30 > "$OUT/$engine.stdout"
    [[ "$(cat "$OUT/$engine.stdout")" == 'pointer replay ok' ]]
done
# Compile portable C wrappers for the native target, keeping the same logical
# guest-pointer ABI; MMIO itself is provided by native runtime services.
objects=()
for source in "$SOURCE" "$ROOT/runtime/vm_devices.c" "$ROOT/runtime/keyboard.c"; do
    unit="$(basename "${source%.c}")"
    "$RCC" compile "$source" --target native --emit-ir -DRIPPLE_MMIO_NO_INLINE=1 \
        -I "$ROOT/runtime/include" > "$OUT/$unit.ir"
    objects+=("$OUT/$unit.ir")
done
"$RCC" link-native "${objects[@]}" -o "$OUT/pointer.native"
RCC_NATIVE_HEADLESS=1 RCC_NATIVE_UNTHROTTLED=1 RCC_NATIVE_INPUT_SCRIPT="$SCRIPT" \
    "$OUT/pointer.native" > "$OUT/native.stdout"
[[ "$(cat "$OUT/native.stdout")" == 'pointer replay ok' ]]
echo 'Input device checks passed: interpreter, JIT and native C.'
