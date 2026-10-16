#!/usr/bin/env bash
# Build and run the maintained Doom port on the Rust VM or as native RCC code.
set -euo pipefail
PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
scene=title
detail=high
resolution_scale=1
wad="${DOOM_WAD:-$PORT/assets/doom1.wad}"
wad_mode="${DOOM_WAD_MODE:-${WAD_MODE:-cooked}}"
asset_validation="${DOOM_ASSET_VALIDATION:-preflight}"
build="${DOOM_BUILD:-$PORT/build}"
case "$build" in /*) ;; *) build="$PWD/$build";; esac
out="$build/run"
clock=""
input=""
frame=1
instructions=100000000000
timeout=300
jobs=4
headless=0
build_only=0
clean_only=0
rebuild=0
jit=0
target=ripple
audio=auto
audio_wav=""
usage() {
  cat <<'EOF'
Usage: ports/doom/run.sh [options]
  --target TARGET         ripple (default), or native (RCC/Cranelift)
  --scene SCENE           title (default), menu, e1m1, or demo1
  --low-detail            Faster, coarser world rendering; F5 also toggles it
  --half-resolution       Render top-left samples only: 160x100 (2x2 blocks)
  --quarter-resolution    Render top-left samples only: 80x50 (4x4 blocks)
  --wad FILE              Shareware WAD (default: assets/doom1.wad)
  --wad-mode MODE         packed, expanded or cooked (default: cooked)
  --asset-validation MODE preflight (default), or guest for extra guest checks
  --build-only            Build without running; native prints executable path
  --clean                 Remove generated Doom build files and exit
  --rebuild               Clean before building and running (or --build-only)
  --headless              Capture a frame without a window
  --frame N               Presentation to capture (default: 1)
  --input-script FILE     Deterministic keyboard input
  --output DIRECTORY      Captures, disk copy and reports (default: build/run)
  --clock MODE            paced, unthrottled or stepped
  --audio MODE            auto (default) or off for speaker playback
  --audio-wav FILE        Capture deterministic stereo PCM to WAV
  --jit                   Run the guest with rvm's Cranelift JIT
  --max-instructions N    Headless instruction budget (default: 100000000000)
  --timeout SECONDS       Headless time budget (default: 300)
  --jobs N                Parallel compiler jobs (default: 4)
EOF
}
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
value() { [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a value"; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) value "$@"; target="$2"; shift 2;;
    --scene) value "$@"; scene="$2"; shift 2;;
    --low-detail) detail=low; shift;;
    --half-resolution) [[ "$resolution_scale" != 4 ]] || fail 'resolution flags cannot be combined'; resolution_scale=2; shift;;
    --quarter-resolution) [[ "$resolution_scale" != 2 ]] || fail 'resolution flags cannot be combined'; resolution_scale=4; shift;;
    --wad) value "$@"; wad="$2"; shift 2;;
    --wad-mode) value "$@"; wad_mode="$2"; shift 2;;
    --asset-validation) value "$@"; asset_validation="$2"; shift 2;;
    --build-only) build_only=1; shift;;
    --clean) clean_only=1; shift;;
    --rebuild) rebuild=1; shift;;
    --headless) headless=1; shift;;
    --frame) value "$@"; frame="$2"; shift 2;;
    --input-script) value "$@"; input="$2"; shift 2;;
    --output) value "$@"; out="$2"; shift 2;;
    --clock) value "$@"; clock="$2"; shift 2;;
    --jit) jit=1; shift;;
    --audio) value "$@"; audio="$2"; shift 2;;
    --audio-wav) value "$@"; audio_wav="$2"; shift 2;;
    --max-instructions) value "$@"; instructions="$2"; shift 2;;
    --timeout) value "$@"; timeout="$2"; shift 2;;
    --jobs) value "$@"; jobs="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) fail "unknown option: $1";;
  esac
done
[[ "$clean_only" == 0 || "$rebuild" == 0 ]] || fail '--clean and --rebuild cannot be combined'
[[ "$audio" == auto || "$audio" == off ]] || fail "audio must be auto or off"
[[ "$target" == ripple || "$target" == native ]] || fail 'target must be ripple or native'
if [[ "$target" == native ]]; then
  [[ "$jit" == 0 ]] || fail '--jit applies to the Ripple VM; native code is compiled ahead of time'
  [[ "$clock" != stepped ]] || fail 'native execution supports paced or unthrottled clocks'
fi
[[ "$scene" == title || "$scene" == menu || "$scene" == e1m1 || "$scene" == demo1 ]] || fail 'scene must be title, menu, e1m1 or demo1'
[[ "$wad_mode" == packed || "$wad_mode" == expanded || "$wad_mode" == cooked ]] || fail 'WAD mode must be packed, expanded or cooked'
[[ "$asset_validation" == preflight || "$asset_validation" == guest ]] || fail 'asset validation must be preflight or guest'
[[ -z "$clock" || "$clock" == paced || "$clock" == unthrottled || "$clock" == stepped ]] || fail 'clock must be paced, unthrottled or stepped'
for numeric in "$frame" "$instructions" "$timeout" "$jobs"; do
  [[ "$numeric" =~ ^[1-9][0-9]*$ ]] || fail 'frame, instruction limit, timeout and jobs must be positive integers'
done
if [[ "$clean_only" == 1 ]]; then
  make -C "$PORT" clean "BUILD=$build"
  exit 0
fi
[[ -f "$wad" ]] || fail "WAD not found: $wad"
wad="$(cd "$(dirname "$wad")" && pwd)/$(basename "$wad")"
if [[ -n "$audio_wav" ]]; then
  audio_wav="$(node "$PORT/tools/build-support.mjs" audio-output "$wad" "$audio_wav")"
fi
if [[ "$scene" == menu ]]; then
  [[ -z "$input" ]] || fail '--scene menu supplies its own input script; use --scene title for a custom replay'
  input="$PORT/tests/menu-open.json"
fi
if [[ -n "$input" ]]; then
  [[ -f "$input" ]] || fail "input script not found: $input"
  input="$(cd "$(dirname "$input")" && pwd)/$(basename "$input")"
fi
if [[ "$target" == ripple && "$build_only" == 0 && ! -x "$ROOT/target/release/rvm" ]]; then
  fail 'Rust VM missing; build the release toolchain from the repository root first'
fi
start_map=0
start_demo=0
low_detail=0
[[ "$scene" == e1m1 ]] && start_map=1
[[ "$scene" == demo1 ]] && start_demo=1
[[ "$detail" == low ]] && low_detail=1
make_target=ripple
[[ "$target" == native ]] && make_target=rcc-native
if [[ "$rebuild" == 1 ]]; then
  make -C "$PORT" clean "BUILD=$build"
fi
make -C "$PORT" -j "$jobs" PROGRAM=engine "TARGET=$make_target" "BUILD=$build" "WAD=$wad" "WAD_MODE=$wad_mode" "ASSET_VALIDATION=$asset_validation" "START_MAP=$start_map" "START_DEMO=$start_demo" "LOW_DETAIL=$low_detail" "RESOLUTION_SCALE=$resolution_scale"
if [[ "$build_only" == 1 ]]; then
  if [[ "$target" == native ]]; then
    printf 'Native executable: %s\n' "$build/rcc-native/engine/engine"
    printf 'Run: %q\n' "$build/rcc-native/engine/engine"
  fi
  exit 0
fi
mkdir -p "$out"
out="$(cd "$out" && pwd)"
if [[ -z "$clock" ]]; then
  if [[ "$headless" == 1 ]]; then clock=unthrottled; else clock=paced; fi
fi
if [[ "$target" == native ]]; then
  command=(env "RCC_NATIVE_FRAME_NUMBER=$frame" "RCC_NATIVE_AUDIO=$audio")
  [[ -z "$audio_wav" ]] || command+=("RCC_NATIVE_AUDIO_WAV=$audio_wav")
  if [[ "$headless" == 1 ]]; then
    command+=(RCC_NATIVE_HEADLESS=1 "RCC_NATIVE_FRAME=$out/$scene.rgb565" "RCC_NATIVE_TIMEOUT=$timeout")
  fi
  [[ "$clock" != unthrottled ]] || command+=(RCC_NATIVE_UNTHROTTLED=1)
  [[ "$clock" != paced ]] || command+=(RCC_NATIVE_PACED=1)
  [[ -z "$input" ]] || command+=("RCC_NATIVE_INPUT_SCRIPT=$input")
  command+=("$build/rcc-native/engine/engine")
  node "$PORT/tools/build-support.mjs" run-manifest "$build/manifest.json" - \
    "$out/$scene.manifest.json" "$scene" "$detail" "${command[@]}"
  cd "$ROOT"
  exec "${command[@]}"
fi
node "$PORT/tools/build-support.mjs" disk "$wad" "$out/doom1.wad" "$wad_mode" "$build/manifest.json" > "$out/disk-image.json"
command=("$ROOT/target/release/rvm" "$build/ripple/engine/engine.bin"
  --bank-size 64000 --memory 16384000 --disk "$out/doom1.wad" --seed 1
  --clock "$clock" --report "$out/$scene.json" --audio "$audio")
[[ -z "$audio_wav" ]] || command+=(--audio-wav "$audio_wav")
if [[ "$headless" == 1 ]]; then
  command+=(--headless --capture-frame "$frame" --screenshot "$out/$scene.png"
    --raw-frame "$out/$scene.rgb565" --max-instructions "$instructions" --timeout "$timeout")
else
  command+=(--visual)
fi
[[ -z "$input" ]] || command+=(--input-script "$input")
[[ "$clock" != stepped ]] || command+=(--control-stdin)
[[ "$jit" == 0 ]] || command+=(--jit)
node "$PORT/tools/build-support.mjs" run-manifest "$build/manifest.json" "$out/disk-image.json" \
  "$out/$scene.manifest.json" "$scene" "$detail" "${command[@]}"
cd "$ROOT"
exec "${command[@]}"
