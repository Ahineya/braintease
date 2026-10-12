#!/usr/bin/env bash
# Build and run the maintained Doom port on the Rust VM.
set -euo pipefail
PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
scene=title
detail=high
wad="${DOOM_WAD:-$PORT/assets/doom1.wad}"
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
usage() {
  cat <<'EOF'
Usage: ports/doom/run.sh [options]
  --scene title|e1m1       Initial scene (default: title)
  --low-detail            Faster, coarser world rendering; F5 also toggles it
  --wad FILE              Shareware WAD (default: assets/doom1.wad)
  --build-only            Build without opening the VM
  --headless              Capture a frame without a window
  --frame N               Presentation to capture (default: 1)
  --input-script FILE     Deterministic keyboard input
  --output DIRECTORY      Captures, disk copy and reports (default: build/run)
  --clock MODE            paced, unthrottled or stepped
  --max-instructions N    Headless instruction budget (default: 100000000000)
  --timeout SECONDS       Headless time budget (default: 300)
  --jobs N                Parallel compiler jobs (default: 4)
EOF
}
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
value() { [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a value"; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scene) value "$@"; scene="$2"; shift 2;;
    --low-detail) detail=low; shift;;
    --wad) value "$@"; wad="$2"; shift 2;;
    --build-only) build_only=1; shift;;
    --headless) headless=1; shift;;
    --frame) value "$@"; frame="$2"; shift 2;;
    --input-script) value "$@"; input="$2"; shift 2;;
    --output) value "$@"; out="$2"; shift 2;;
    --clock) value "$@"; clock="$2"; shift 2;;
    --max-instructions) value "$@"; instructions="$2"; shift 2;;
    --timeout) value "$@"; timeout="$2"; shift 2;;
    --jobs) value "$@"; jobs="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) fail "unknown option: $1";;
  esac
done
[[ "$scene" == title || "$scene" == e1m1 ]] || fail 'scene must be title or e1m1'
[[ -z "$clock" || "$clock" == paced || "$clock" == unthrottled || "$clock" == stepped ]] || fail 'clock must be paced, unthrottled or stepped'
for numeric in "$frame" "$instructions" "$timeout" "$jobs"; do
  [[ "$numeric" =~ ^[1-9][0-9]*$ ]] || fail 'frame, instruction limit, timeout and jobs must be positive integers'
done
[[ -f "$wad" ]] || fail "WAD not found: $wad"
wad="$(cd "$(dirname "$wad")" && pwd)/$(basename "$wad")"
if [[ -n "$input" ]]; then
  [[ -f "$input" ]] || fail "input script not found: $input"
  input="$(cd "$(dirname "$input")" && pwd)/$(basename "$input")"
fi
if [[ "$build_only" == 0 && ! -x "$ROOT/target/release/rvm" ]]; then
  fail 'Rust VM missing; build the release toolchain from the repository root first'
fi
start_map=0
low_detail=0
[[ "$scene" == e1m1 ]] && start_map=1
[[ "$detail" == low ]] && low_detail=1
make -C "$PORT" -j "$jobs" PROGRAM=engine TARGET=ripple "BUILD=$build" "WAD=$wad" "START_MAP=$start_map" "LOW_DETAIL=$low_detail"
[[ "$build_only" == 0 ]] || exit 0
mkdir -p "$out"
out="$(cd "$out" && pwd)"
node "$PORT/tools/build-support.mjs" disk "$wad" "$out/doom1.wad" > "$out/disk-image.json"
if [[ -z "$clock" ]]; then
  if [[ "$headless" == 1 ]]; then clock=unthrottled; else clock=paced; fi
fi
command=("$ROOT/target/release/rvm" "$build/ripple/engine/engine.bin"
  --bank-size 64000 --memory 16384000 --disk "$out/doom1.wad" --seed 1
  --clock "$clock" --report "$out/$scene.json")
if [[ "$headless" == 1 ]]; then
  command+=(--headless --capture-frame "$frame" --screenshot "$out/$scene.png"
    --raw-frame "$out/$scene.rgb565" --max-instructions "$instructions" --timeout "$timeout")
else
  command+=(--visual)
fi
[[ -z "$input" ]] || command+=(--input-script "$input")
[[ "$clock" != stepped ]] || command+=(--control-stdin)
node "$PORT/tools/build-support.mjs" run-manifest "$build/manifest.json" "$out/disk-image.json" \
  "$out/$scene.manifest.json" "$scene" "$detail" "${command[@]}"
cd "$ROOT"
exec "${command[@]}"
