#!/usr/bin/env bash
# Build and run the maintained full Doom port on the Brainfuck CPU.
set -euo pipefail
PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
build="${DOOM_BUILD:-$PORT/build}"
case "$build" in /*) ;; *) build="$PWD/$build";; esac
wad="${DOOM_WAD:-$PORT/assets/doom1.wad}"
wad_mode="${DOOM_WAD_MODE:-${WAD_MODE:-cooked}}"
asset_validation="${DOOM_ASSET_VALIDATION:-preflight}"
out="$build/run-bf"
scene=title
detail=high
resolution_scale=1
jobs=4
build_only=0
mode=--visual
report=""
resume_build_setting=0
args=()

usage() {
  cat <<'EOF'
Usage: scripts/run-bf-doom.sh [options] [rbf options...]

Build and run the full Doom port on the Brainfuck host (visual by default).
There is no default timeout. Close the window or press Ctrl-C to stop.

  --build-only       Build the BF image without running it
  --headless         Run without a window (does not request a capture by itself)
  --visual           Open the graphics window (default)
  --scene SCENE      title (default), menu, e1m1, or demo1
  --low-detail       Start with coarser world rendering
  --half-resolution  Render top-left samples only: 160x100 (2x2 blocks)
  --quarter-resolution  Render top-left samples only: 80x50 (4x4 blocks)
  --wad FILE         IWAD (default: bundled shareware WAD)
  --wad-mode MODE    packed, expanded or cooked (default: cooked)
  --asset-validation MODE  preflight (default), or guest for extra guest checks
  --output DIRECTORY Disk copy, manifest and default report (default: build/run-bf)
  --report FILE      Override the execution report path
  --jobs N           Parallel compiler jobs (default: 4)
  --telemetry-lite   Dot every 5s; BF/guest counts and rates every 30s on stderr
  --telemetry        Report detailed progress on stderr (forwarded to rbf)
  --profile FILE     Write generic BF operation timings on exit (adds overhead)
  --timeout SECONDS  Optional execution time limit (forwarded to rbf)
  --save-state FILE  Save BF execution state on exit (forwarded to rbf)
  --save-at-frame N  Stop and save at presentation N (requires --save-state)
  --load-state FILE  Resume the existing image and disk without rebuilding Doom

Other rbf options, including --capture-frame, --screenshot, --raw-frame,
--input-script, --clock and --max-instructions, are forwarded unchanged.
Use rbf --help for their syntax. Its default clock is unthrottled logical time.

Cargo checks release tool freshness, then Make reuses current engine objects.
The image is ports/doom/build/ripple/engine/engine.bf. DOOM_BUILD and DOOM_WAD
override the build directory and WAD. DOOM_WAD_MODE selects the asset format.
A copy of the matching generated disk image is mounted; the WAD stays intact.
Cooked assets are fully checked when packed and reused on later launches.
DOOM_ASSET_VALIDATION or --asset-validation guest also enables the slower
diagnostic guest checks. Neither VM host performs Doom asset validation.
--disk and --max-tape-cells are configured by this launcher.
Menu queues Escape at the first logical tick; demo1 plays the IWAD recording
immediately. A resumed menu request queues Escape one tick after the saved tick.
Resuming requires the original build image and --output directory; it preserves
the mounted disk. Scene/detail/resolution/WAD build settings cannot be changed
on resume; the saved image already contains its selected resolution.

This keeps Doom's 64,000-cell banks and 256 RAM banks. Tape alone needs over
250 MiB; code size determines the exact allocation. Startup and the first
frame can take a long time. Use --telemetry-lite for compact progress or
--telemetry for details; these modes cannot be combined.
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
    --low-detail) detail=low; resume_build_setting=1; shift;;
    --half-resolution) [[ "$resolution_scale" != 4 ]] || fail 'resolution flags cannot be combined'; resolution_scale=2; resume_build_setting=1; shift;;
    --quarter-resolution) [[ "$resolution_scale" != 2 ]] || fail 'resolution flags cannot be combined'; resolution_scale=4; resume_build_setting=1; shift;;
    --wad) value "$@"; wad="$2"; resume_build_setting=1; shift 2;;
    --wad-mode) value "$@"; wad_mode="$2"; resume_build_setting=1; shift 2;;
    --asset-validation) value "$@"; asset_validation="$2"; resume_build_setting=1; shift 2;;
    --output) value "$@"; out="$2"; shift 2;;
    --report) value "$@"; report="$2"; shift 2;;
    --report=*) report="${1#*=}"; [[ -n "$report" ]] || fail '--report requires a value'; shift;;
    --jobs) value "$@"; jobs="$2"; shift 2;;
    --disk|--disk=*|--max-tape-cells|--max-tape-cells=*) fail "$1 is managed by the launcher; use --wad to select an IWAD";;
    --) shift; args+=("$@"); break;;
    *) args+=("$1"); shift;;
  esac
done
[[ "$scene" == title || "$scene" == menu || "$scene" == e1m1 || "$scene" == demo1 ]] || fail 'scene must be title, menu, e1m1 or demo1'
[[ "$wad_mode" == packed || "$wad_mode" == expanded || "$wad_mode" == cooked ]] || fail 'WAD mode must be packed, expanded or cooked'
[[ "$asset_validation" == preflight || "$asset_validation" == guest ]] || fail 'asset validation must be preflight or guest'
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail 'jobs must be a positive integer'
load_state=""
has_input=0
relative_input=0
for ((index=0; index<${#args[@]}; index++)); do
  case "${args[index]}" in
    --load-state)
      [[ $((index + 1)) -lt ${#args[@]} ]] || fail '--load-state requires a value'
      load_state="${args[index + 1]}"; [[ -n "$load_state" ]] || fail '--load-state requires a value';;
    --load-state=*) load_state="${args[index]#*=}"; [[ -n "$load_state" ]] || fail '--load-state requires a value';;
    --input-script|--input-script=*) has_input=1;;
    --input-script-relative) relative_input=1;;
  esac
done
if [[ -n "$load_state" ]]; then
  [[ -f "$load_state" ]] || fail "saved state not found: $load_state"
  [[ "$build_only" == 0 ]] || fail '--load-state cannot be used with --build-only'
  [[ "$scene" == title || "$scene" == menu ]] || fail 'a saved state already has its scene; only --scene menu can queue new input'
  [[ "$resume_build_setting" == 0 ]] || fail 'WAD/detail/resolution/asset settings belong to the saved build and cannot be changed on resume'
else
  [[ -f "$wad" ]] || fail "WAD not found: $wad"
  wad="$(cd "$(dirname "$wad")" && pwd)/$(basename "$wad")"
fi
if [[ "$scene" == menu ]]; then
  [[ "$has_input" == 0 ]] || fail '--scene menu supplies its own input script; use --scene title for a custom replay'
  args+=(--input-script "$PORT/tests/menu-open.json")
  if [[ -n "$load_state" && "$relative_input" == 0 ]]; then args+=(--input-script-relative); fi
fi

if [[ -n "$load_state" ]]; then
  for required in "$build/ripple/engine/engine.bf" "$build/manifest.json" "$out/doom1.wad" "$out/disk-image.json"; do
    [[ -f "$required" ]] || fail "resume needs the existing file: $required (use the original DOOM_BUILD and --output)"
  done
  printf 'Checking release Brainfuck host; preserving saved image and disk...\n' >&2
  cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rbf
else
  printf 'Checking release compiler, assembler and Brainfuck tools...\n' >&2
  cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rbf
  cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink

  start_map=0
  start_demo=0
  low_detail=0
  [[ "$scene" == e1m1 ]] && start_map=1
  [[ "$scene" == demo1 ]] && start_demo=1
  [[ "$detail" == low ]] && low_detail=1
  make -C "$PORT" -j "$jobs" PROGRAM=engine TARGET=bf "BUILD=$build" "WAD=$wad" \
    "WAD_MODE=$wad_mode" "ASSET_VALIDATION=$asset_validation" "START_MAP=$start_map" "START_DEMO=$start_demo" "LOW_DETAIL=$low_detail" "RESOLUTION_SCALE=$resolution_scale"
  [[ "$build_only" == 0 ]] || exit 0
fi

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
if [[ -z "$load_state" ]]; then
  node "$PORT/tools/build-support.mjs" disk "$wad" "$out/doom1.wad" "$wad_mode" "$build/manifest.json" > "$out/disk-image.json"
fi
command=("$ROOT/target/release/rbf" "$image" "$mode" --disk "$out/doom1.wad"
  --max-tape-cells "$tape_cells")
[[ -z "$report" ]] || command+=(--report "$report")
if [[ ${#args[@]} -gt 0 ]]; then command+=("${args[@]}"); fi
node "$PORT/tools/build-support.mjs" run-manifest "$build/manifest.json" "$out/disk-image.json" \
  "$out/$scene.manifest.json" "$scene" "$detail" "${command[@]}"
if [[ -n "$load_state" ]]; then
  printf 'Resuming Doom in Brainfuck (%s tape cells).\n' "$tape_cells" >&2
else
  printf 'Running Doom in Brainfuck (%s tape cells); first frame may take a long time.\n' "$tape_cells" >&2
fi
exec "${command[@]}"
