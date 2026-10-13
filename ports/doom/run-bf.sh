#!/usr/bin/env bash
# Build and run the maintained full Doom port on the Brainfuck CPU.
set -euo pipefail
PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
build="${DOOM_BUILD:-$PORT/build}"
case "$build" in /*) ;; *) build="$PWD/$build";; esac
wad="${DOOM_WAD:-$PORT/assets/doom1.wad}"
out="$build/run-bf"
scene=title
detail=high
jobs=4
build_only=0
mode=--visual
report=""
args=()

usage() {
  cat <<'EOF'
Usage: scripts/run-bf-doom.sh [options] [rbf options...]

Build and run the full Doom port on the Brainfuck host (visual by default).
There is no default timeout. Close the window or press Ctrl-C to stop.

  --build-only       Build the BF image without running it
  --headless         Run without a window (does not request a capture by itself)
  --visual           Open the graphics window (default)
  --scene title|e1m1  Initial scene (default: title)
  --low-detail       Start with coarser world rendering
  --wad FILE         IWAD (default: bundled shareware WAD)
  --output DIRECTORY Disk copy, manifest and default report (default: build/run-bf)
  --report FILE      Override the execution report path
  --jobs N           Parallel compiler jobs (default: 4)
  --telemetry        Report progress on stderr (forwarded to rbf)
  --timeout SECONDS  Optional execution time limit (forwarded to rbf)

Other rbf options, including --capture-frame, --screenshot, --raw-frame,
--input-script, --clock and --max-instructions, are forwarded unchanged.
Use rbf --help for their syntax. Its default clock is unthrottled logical time.

Cargo checks release tool freshness, then Make reuses current engine objects.
The image is ports/doom/build/ripple/engine/engine.bf. DOOM_BUILD and DOOM_WAD
override the build directory and WAD. A padded COPY of the WAD is mounted;
--disk and --max-tape-cells are configured by this launcher.

This keeps Doom's 64,000-cell banks and 256 RAM banks. Tape alone needs over
250 MiB; code size determines the exact allocation. Startup and the first
frame can take a long time. --telemetry makes progress visible.
EOF
}
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
value() { [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a value"; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0;;
    --build-only) build_only=1; shift;;
    --headless|--visual) mode="$1"; shift;;
    --scene) value "$@"; scene="$2"; shift 2;;
    --low-detail) detail=low; shift;;
    --wad) value "$@"; wad="$2"; shift 2;;
    --output) value "$@"; out="$2"; shift 2;;
    --report) value "$@"; report="$2"; shift 2;;
    --report=*) report="${1#*=}"; [[ -n "$report" ]] || fail '--report requires a value'; shift;;
    --jobs) value "$@"; jobs="$2"; shift 2;;
    --disk|--disk=*|--max-tape-cells|--max-tape-cells=*) fail "$1 is managed by the launcher; use --wad to select an IWAD";;
    --) shift; args+=("$@"); break;;
    *) args+=("$1"); shift;;
  esac
done
[[ "$scene" == title || "$scene" == e1m1 ]] || fail 'scene must be title or e1m1'
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail 'jobs must be a positive integer'
[[ -f "$wad" ]] || fail "WAD not found: $wad"
wad="$(cd "$(dirname "$wad")" && pwd)/$(basename "$wad")"

printf 'Checking release compiler, assembler and Brainfuck tools...\n' >&2
cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rbf
cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink

start_map=0
low_detail=0
[[ "$scene" == e1m1 ]] && start_map=1
[[ "$detail" == low ]] && low_detail=1
make -C "$PORT" -j "$jobs" PROGRAM=engine TARGET=bf "BUILD=$build" "WAD=$wad" \
  "START_MAP=$start_map" "LOW_DETAIL=$low_detail"
[[ "$build_only" == 0 ]] || exit 0

image="$build/ripple/engine/engine.bf"
tape_cells="$(node "$PORT/tools/build-support.mjs" bf-tape-cells "$image")"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
# A report supplied after -- belongs to rbf too; do not duplicate its option.
forwarded_report=0
if [[ ${#args[@]} -gt 0 ]]; then
  for argument in "${args[@]}"; do
    case "$argument" in --report|--report=*) forwarded_report=1;; esac
  done
fi
if [[ -z "$report" && "$forwarded_report" == 0 ]]; then report="$out/$scene.json"; fi
node "$PORT/tools/build-support.mjs" disk "$wad" "$out/doom1.wad" > "$out/disk-image.json"
command=("$ROOT/target/release/rbf" "$image" "$mode" --disk "$out/doom1.wad"
  --max-tape-cells "$tape_cells")
[[ -z "$report" ]] || command+=(--report "$report")
if [[ ${#args[@]} -gt 0 ]]; then command+=("${args[@]}"); fi
node "$PORT/tools/build-support.mjs" run-manifest "$build/manifest.json" "$out/disk-image.json" \
  "$out/$scene.manifest.json" "$scene" "$detail" "${command[@]}"
printf 'Running Doom in Brainfuck (%s tape cells); first frame may take a long time.\n' "$tape_cells" >&2
exec "${command[@]}"
