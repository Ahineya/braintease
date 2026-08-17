#!/usr/bin/env bash
# Build and run the C Doom TITLEPIC demo (c-test/examples/doom).
#
# Multi-file C: wad.c, mem.c, picture.c, doom-fixed.c.
# Linked with the C runtime (crt0 + libruntime), unlike the BFM demo.
#
# Usage:
#   scripts/run-doom-c.sh /path/to/DOOM1.WAD
#   scripts/run-doom-c.sh --build-only
#   scripts/run-doom-c.sh --frequency 2MHz ~/games/DOOM1.WAD
#
# Default WAD: c-test/examples/doom/doom1.wad (or $DOOM_WAD).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/c-test/examples/doom"
DEFAULT_WAD="$SRC/doom1.wad"

build_only=0
frequency=""
wad=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [--build-only] [--frequency HZ] [WAD]

Build and run the C Doom TITLEPIC demo on rvm.

  --build-only       Compile/assemble/link only; do not run
  --frequency HZ     Throttle the VM (e.g. 2MHz, 500KHz)
  WAD                Path to a Doom IWAD (DOOM1.WAD, doom.wad, …)
                     Passed to rvm as --disk.

The demo opens an RGB565 window (--visual) and loops on the title pic.
Close the window to exit.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --build-only)
      build_only=1
      shift
      ;;
    --frequency)
      frequency="${2:?--frequency requires a value (e.g. 2MHz)}"
      shift 2
      ;;
    --frequency=*)
      frequency="${1#--frequency=}"
      shift
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [[ -n "$wad" ]]; then
        echo "error: unexpected extra argument: $1" >&2
        usage >&2
        exit 1
      fi
      wad="$1"
      shift
      ;;
  esac
done

if [[ -n "${DOOM_WAD:-}" && -z "$wad" ]]; then
  wad="$DOOM_WAD"
fi

if [[ -z "$wad" && -f "$DEFAULT_WAD" ]]; then
  wad="$DEFAULT_WAD"
fi

make_args=()
if [[ -n "$wad" ]]; then
  make_args+=("WAD=$wad")
fi
if [[ -n "$frequency" ]]; then
  make_args+=("FREQUENCY=$frequency")
fi

if [[ "$build_only" -eq 1 ]]; then
  exec make -C "$SRC" "${make_args[@]}"
fi

exec make -C "$SRC" run "${make_args[@]}"
