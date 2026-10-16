#!/usr/bin/env bash
# Actual compiled C through the interpreter, JIT and native runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${PCM_TEST_OUT:-$ROOT/build/pcm/device}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RCC="$ROOT/target/release/rcc"
RVM="$ROOT/target/release/rvm"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
SOURCE="$ROOT/c-test/fixtures/devices/pcm.c"
make -C "$ROOT/runtime" all AUDIO=1 BANK_SIZE=64000 > "$OUT/runtime.log" 2>&1
"$RCC" compile "$SOURCE" -I "$ROOT/runtime/include" -o "$OUT/pcm.asm" > "$OUT/compile.log" 2>&1
"$RASM" assemble "$OUT/pcm.asm" -o "$OUT/pcm.pobj" --bank-size 64000 --max-immediate 65535 > "$OUT/assemble.log"
"$RLINK" "$ROOT/runtime/crt0.pobj" "$ROOT/runtime/libruntime.par" "$OUT/pcm.pobj" -f binary --bank-size 64000 -o "$OUT/pcm.bin" > "$OUT/link.log"
for engine in interpreter jit; do
    options=(--audio off)
    if [[ "$engine" == jit ]]; then options+=(--jit); fi
    "$RVM" "$OUT/pcm.bin" --headless --exit-code --memory 2000000 --disk "$OUT/$engine.disk" \
        --audio-wav "$OUT/$engine.wav" --max-instructions 10000000 --timeout 30 "${options[@]}" > "$OUT/$engine.stdout"
    [[ "$(cat "$OUT/$engine.stdout")" == 'pcm device ok' ]]
done
objects=()
for source in "$SOURCE" "$ROOT/runtime/vm_devices.c" "$ROOT/runtime/vm_audio.c"; do
    unit="$(basename "${source%.c}")"
    "$RCC" compile "$source" --target native --emit-ir -DRIPPLE_MMIO_NO_INLINE=1 -I "$ROOT/runtime/include" > "$OUT/$unit.ir" 2> "$OUT/$unit.log"
    objects+=("$OUT/$unit.ir")
done
"$RCC" link-native "${objects[@]}" -o "$OUT/pcm.native"
RCC_NATIVE_HEADLESS=1 RCC_NATIVE_AUDIO=off RCC_NATIVE_AUDIO_WAV="$OUT/native.wav" "$OUT/pcm.native" > "$OUT/native.stdout"
[[ "$(cat "$OUT/native.stdout")" == 'pcm device ok' ]]
python3 - "$OUT" <<'PY'
import pathlib,struct,sys,wave
root=pathlib.Path(sys.argv[1])
for name in ['interpreter','jit','native']:
    with wave.open(str(root/(name+'.wav')),'rb') as f:
        assert (f.getnchannels(),f.getsampwidth(),f.getframerate(),f.getnframes())==(1,2,8000,8)
        assert struct.unpack('<8h',f.readframes(8))==(-32768,0,32767,-1,16383,-16384,0,0)
assert (root/'interpreter.wav').read_bytes()==(root/'jit.wav').read_bytes()==(root/'native.wav').read_bytes()
print('PCM device checks passed: exact signed samples through interpreter, JIT and native.')
PY
