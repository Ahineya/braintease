#!/usr/bin/env bash
# Actual compiled C through the interpreter, JIT and native runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${MUSIC_TEST_OUT:-$ROOT/build/music/device}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RCC="$ROOT/target/release/rcc"
RVM="$ROOT/target/release/rvm"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
SOURCE="$ROOT/c-test/fixtures/devices/music.c"
make -C "$ROOT/runtime" all AUDIO=1 BANK_SIZE=64000 > "$OUT/runtime.log" 2>&1
"$RCC" compile "$SOURCE" -I "$ROOT/runtime/include" -o "$OUT/music.asm" > "$OUT/compile.log" 2>&1
"$RASM" assemble "$OUT/music.asm" -o "$OUT/music.pobj" --bank-size 64000 --max-immediate 65535 > "$OUT/assemble.log"
"$RLINK" "$ROOT/runtime/crt0.pobj" "$ROOT/runtime/libruntime.par" "$OUT/music.pobj" -f binary --bank-size 64000 -o "$OUT/music.bin" > "$OUT/link.log"
for engine in interpreter jit; do
    options=(--audio off)
    if [[ "$engine" == jit ]]; then options+=(--jit); fi
    "$RVM" "$OUT/music.bin" --headless --exit-code --memory 2000000 --disk "$OUT/$engine.disk" \
        --audio-wav "$OUT/$engine.wav" --max-instructions 10000000 --timeout 30 "${options[@]}" > "$OUT/$engine.stdout"
    [[ "$(cat "$OUT/$engine.stdout")" == 'music device ok' ]]
done
objects=()
for source in "$SOURCE" "$ROOT/runtime/vm_devices.c" "$ROOT/runtime/vm_audio.c" "$ROOT/runtime/vm_music.c"; do
    unit="$(basename "${source%.c}")"
    "$RCC" compile "$source" --target native --emit-ir -DRIPPLE_MMIO_NO_INLINE=1 -I "$ROOT/runtime/include" > "$OUT/$unit.ir" 2> "$OUT/$unit.log"
    objects+=("$OUT/$unit.ir")
done
"$RCC" link-native "${objects[@]}" -o "$OUT/music.native"
RCC_NATIVE_HEADLESS=1 RCC_NATIVE_AUDIO=off RCC_NATIVE_AUDIO_WAV="$OUT/native.wav" "$OUT/music.native" > "$OUT/native.stdout"
[[ "$(cat "$OUT/native.stdout")" == 'music device ok' ]]
python3 - "$OUT" <<'PYTHON'
import pathlib,struct,sys,wave
root=pathlib.Path(sys.argv[1])
for name in ['interpreter','jit','native']:
    with wave.open(str(root/(name+'.wav')),'rb') as f:
        assert (f.getnchannels(),f.getsampwidth(),f.getframerate(),f.getnframes())==(2,2,8000,1008)
        samples=struct.unpack('<2016h',f.readframes(1008))
        assert any(samples[:320]), 'synthesizer must produce a note'
        assert not any(samples[320:480]), 'music pause must be silent'
        assert any(samples[480:1760]), 'resume must produce sound'
        assert not any(samples[1760:]), 'volume zero and stop must be silent'
assert (root/'interpreter.wav').read_bytes()==(root/'jit.wav').read_bytes()==(root/'native.wav').read_bytes()
print('Music device checks passed: identical OPL3 PCM, pause/resume, loops, volume and stop across interpreter, JIT and native.')
PYTHON
