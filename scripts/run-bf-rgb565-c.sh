#!/usr/bin/env bash
# Build and run the original 64x64 C RGB565 demo on the Brainfuck CPU.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BF_RGB565_C_OUT:-$ROOT/build/bf}"
DEMO_BANK_SIZE="${BF_RGB565_C_BANK_SIZE-16384}"
build_only=0
visual=1
args=()

usage() {
    cat <<'EOF'
Usage: scripts/run-bf-rgb565-c.sh [--build-only] [--headless] [rbf options...]

Build and run c-test/examples/rgb565_interactive_simple.c (64x64 RGB565).
Use arrow keys to move the red square. Close its window or press Ctrl-C to stop.

  --build-only       Rebuild the BF image without starting it
  --headless         Run without a window (for captures)
  --visual           Open the graphics window (default)
  --telemetry        Report ongoing BF execution and drawing progress on stderr

Other options, such as --timeout, --capture-frame, --screenshot, --raw-frame,
--report and --no-optimize, are passed to rbf. Use rbf --help for the full list.

Cargo checks tool freshness on every run. The C demo and its graphics/MMIO
runtime use three RAM banks. BF_RGB565_C_BANK_SIZE selects cells per bank
(default 16384; decimal integer 8224..65535). Outputs stay under build/bf;
the BF image is build/bf/rgb565-demo.bf. BF_RGB565_C_OUT selects a different
output directory. The ordinary runtime objects are not modified.

The first frame is published only after thousands of C pixel writes. A black
window while telemetry shows drawing progress means that frame is still being
computed. For the smaller assembly demo, use scripts/run-bf-rgb565.sh instead.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --build-only) build_only=1; shift ;;
        --headless) visual=0; shift ;;
        --visual) visual=1; shift ;;
        --) shift; args+=("$@"); break ;;
        *) args+=("$1"); shift ;;
    esac
done

# The legacy display occupies bank zero: 32 MMIO cells and two 64x64 buffers.
# Restrict input before shell arithmetic, including its width to avoid overflow.
if [[ ! "$DEMO_BANK_SIZE" =~ ^[0-9]{1,5}$ ]]; then
    printf 'BF_RGB565_C_BANK_SIZE must be a decimal integer from 8224 to 65535.\n' >&2
    exit 1
fi
DEMO_BANK_SIZE=$((10#$DEMO_BANK_SIZE))
if (( DEMO_BANK_SIZE < 8224 || DEMO_BANK_SIZE > 65535 )); then
    printf 'BF_RGB565_C_BANK_SIZE must be from 8224 to 65535 (the display needs 8224 cells).\n' >&2
    exit 1
fi

printf 'Checking release compiler, assembler and Brainfuck tools...\n' >&2
cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rbf
cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink
cargo build --quiet --release --manifest-path "$ROOT/src/bf-macro-expander/Cargo.toml" --bin bfm

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
PREFIX="$OUT/rgb565-demo"
RUNTIME_OUT="$OUT/rgb565-demo-runtime"
mkdir -p "$RUNTIME_OUT"

# Recompile dependencies as well as main: generated BF must never keep an old
# compiler's runtime code. Keep this bank configuration separate from runtime/.
printf 'Building the 64x64 C demo (%s-cell banks, three RAM banks)...\n' "$DEMO_BANK_SIZE" >&2
objects=()
for library in graphics mmio; do
    "$ROOT/target/release/rcc" compile "$ROOT/runtime/$library.c" \
        -o "$RUNTIME_OUT/$library.asm" -I "$ROOT/runtime/include" --bank-size "$DEMO_BANK_SIZE" >&2
    "$ROOT/src/ripple-asm/target/release/rasm" assemble "$RUNTIME_OUT/$library.asm" \
        -o "$RUNTIME_OUT/$library.pobj" --bank-size "$DEMO_BANK_SIZE" --max-immediate 65535 >&2
    objects+=("$RUNTIME_OUT/$library.pobj")
done
BANK_SIZE="$DEMO_BANK_SIZE" MEMORY_BANKS=3 BF_HOST=1 "$ROOT/scripts/build-bf.sh" \
    "$ROOT/c-test/examples/rgb565_interactive_simple.c" "$PREFIX" "${objects[@]}" >&2

# Keep an equivalent RVM image for profiling and exact frame comparisons.
"$ROOT/src/ripple-asm/target/release/rlink" "$PREFIX.crt0.pobj" "$PREFIX.pobj" \
    "${objects[@]}" -f binary --bank-size "$DEMO_BANK_SIZE" -o "$PREFIX.bin" >&2
if [[ "$build_only" == 1 ]]; then exit 0; fi

mode=--headless
if [[ "$visual" == 1 ]]; then mode=--visual; fi
run_args=("$PREFIX.bf" "$mode")
if [[ ${#args[@]} -gt 0 ]]; then run_args+=("${args[@]}"); fi
printf 'Running the C demo; the window stays black until its first frame is complete.\n' >&2
exec "$ROOT/target/release/rbf" "${run_args[@]}"
