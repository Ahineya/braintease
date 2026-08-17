#!/usr/bin/env bash
# Build and run the BFM Doom titlepic demo (c-test/examples/doom/bfm-doom.bfm).
#
# This is Ripple assembly written in the Brainfuck macro language, not C.
# It expands to .asm, assembles, and links standalone (its own crt0 — no C runtime).
# At runtime it reads an IWAD from RVM block storage and draws TITLEPIC in RGB565.
#
# Pass the WAD file itself as the disk image: storage maps it as 64KB blocks,
# so file offset 0xHHHHhhhh is block 0xHHHH, byte address 0xhhhh.
#
# Usage:
#   scripts/run-bfm-doom.sh /path/to/DOOM1.WAD
#   scripts/run-bfm-doom.sh --build-only
#   scripts/run-bfm-doom.sh --frequency 2MHz ~/games/DOOM1.WAD
#
# Equivalent (also rebuilds the C runtime, which this demo does not need):
#   ./rct run bfm-doom --visual --disk /path/to/DOOM1.WAD

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/c-test/examples/doom/bfm-doom.bfm"
BUILD="$ROOT/c-test/build"
BANK_SIZE=64000

BFM="$ROOT/src/bf-macro-expander/target/release/bfm"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
RVM="$ROOT/target/release/rvm"

ASM="$BUILD/bfm-doom.asm"
POBJ="$BUILD/bfm-doom.pobj"
BIN="$BUILD/bfm-doom.bin"
EXPAND_LOG="$BUILD/bfm-doom.expand.log"

build_only=0
frequency=""
wad=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [--build-only] [--frequency HZ] [WAD]

Build and run c-test/examples/doom/bfm-doom.bfm on rvm.

  --build-only       Expand/assemble/link only; do not run
  --frequency HZ     Throttle the VM (e.g. 2MHz, 500KHz)
  WAD                Path to a Doom IWAD (DOOM1.WAD, doom.wad, …)
                     Passed to rvm as --disk. Required unless --build-only.

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

require_tool() {
  local path="$1" name="$2"
  if [[ ! -x "$path" ]]; then
    echo "error: missing $name at $path" >&2
    echo "Build the toolchain first, e.g.:" >&2
    echo "  cargo build --release" >&2
    echo "  (cd src/ripple-asm && cargo build --release)" >&2
    echo "  (cd src/bf-macro-expander && cargo build --release)" >&2
    exit 1
  fi
}

require_tool "$BFM" "bfm"
require_tool "$RASM" "rasm"
require_tool "$RLINK" "rlink"
if [[ "$build_only" -eq 0 ]]; then
  require_tool "$RVM" "rvm"
fi

if [[ ! -f "$SRC" ]]; then
  echo "error: source not found: $SRC" >&2
  exit 1
fi

mkdir -p "$BUILD"

echo "Expanding $SRC"
# bfm currently prints {proc}/{local} debug traces on stderr
if ! "$BFM" expand "$SRC" -o "$ASM" --collapse-empty-lines 2>"$EXPAND_LOG"; then
  echo "error: bfm expand failed" >&2
  cat "$EXPAND_LOG" >&2
  exit 1
fi

echo "Assembling $ASM (bank_size=$BANK_SIZE)"
"$RASM" assemble "$ASM" -o "$POBJ" --bank-size "$BANK_SIZE" --max-immediate 65535

echo "Linking $POBJ (standalone, no C runtime)"
"$RLINK" "$POBJ" -f binary --bank-size "$BANK_SIZE" --standalone -o "$BIN"

echo "Built $BIN"

if [[ "$build_only" -eq 1 ]]; then
  exit 0
fi

if [[ -z "$wad" ]]; then
  echo "error: IWAD path required to run (or set DOOM_WAD)" >&2
  echo "Example: $0 /path/to/DOOM1.WAD" >&2
  exit 1
fi

if [[ ! -f "$wad" ]]; then
  echo "error: WAD not found: $wad" >&2
  exit 1
fi

echo "Running rvm --visual --disk $wad"
rvm_args=("$BIN" --visual --disk "$wad")
if [[ -n "$frequency" ]]; then
  rvm_args+=(--frequency "$frequency")
fi
exec "$RVM" "${rvm_args[@]}"
