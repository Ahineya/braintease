#!/usr/bin/env bash
# Build a C or assembly program for the Brainfuck host or an ordinary interpreter.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -lt 1 || "$1" == --help ]]; then
    echo "Usage: $0 source.c|source.asm [output-prefix] [extra-object.pobj ...]"
    echo "Environment: BANK_SIZE=64000 MEMORY_BANKS=16 BF_HOST=1 BF_IRQ=0"
    echo "Set BF_HOST=0 for ordinary 16-bit Brainfuck interpreters (native '.' output, no host devices)."
    echo "Set BF_IRQ=1 to compile the interrupt-capable CPU (default has no IRQ polling)."
    echo "Build tools first: cargo build --release; cargo build --release --manifest-path src/ripple-asm/Cargo.toml; cargo build --release --manifest-path src/bf-macro-expander/Cargo.toml"
    exit 0
fi
SOURCE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="${2:-$ROOT/build/bf/$(basename "${SOURCE%.*}")}"
if [[ $# -ge 2 ]]; then shift 2; else shift; fi
BANK_SIZE="${BANK_SIZE:-64000}"
MEMORY_BANKS="${MEMORY_BANKS:-16}"
BF_HOST="${BF_HOST:-1}"
BF_IRQ="${BF_IRQ:-0}"
link_flags=(-f macro --standalone)
case "$BF_HOST" in
    1) link_flags+=(--bf-host --memory-banks "$MEMORY_BANKS") ;;
    0) ;;
    *) echo "BF_HOST must be 0 or 1" >&2; exit 1 ;;
esac
case "$BF_IRQ" in
    1) link_flags+=(--bf-irq) ;;
    0) ;;
    *) echo "BF_IRQ must be 0 or 1" >&2; exit 1 ;;
esac
mkdir -p "$(dirname "$OUT")"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
BFM="$ROOT/src/bf-macro-expander/target/release/bfm"
for tool in "$RASM" "$RLINK" "$BFM"; do
    [[ -x "$tool" ]] || { echo "Missing tool: $tool (see --help)" >&2; exit 1; }
done
objects=()
case "$SOURCE" in
    *.c)
        # Keep this bank configuration inside the output directory. A small BF
        # build must not replace the normal RVM runtime's startup object.
        "$RASM" assemble "$ROOT/runtime/crt0.asm" -o "$OUT.crt0.pobj" --bank-size "$BANK_SIZE" --max-immediate 65535
        "$ROOT/target/release/rcc" compile "$SOURCE" -o "$OUT.asm" -I "$ROOT/runtime/include" --bank-size "$BANK_SIZE"
        "$RASM" assemble "$OUT.asm" -o "$OUT.pobj" --bank-size "$BANK_SIZE" --max-immediate 65535
        objects+=("$OUT.crt0.pobj" "$OUT.pobj")
        ;;
    *.asm)
        "$RASM" assemble "$SOURCE" -o "$OUT.pobj" --bank-size "$BANK_SIZE" --max-immediate 65535
        objects+=("$OUT.pobj")
        ;;
    *) echo "Expected .c or .asm source" >&2; exit 1 ;;
esac
objects+=("$@")
"$RLINK" "${objects[@]}" "${link_flags[@]}" --bank-size "$BANK_SIZE" -o "$OUT.bfm"
"$BFM" expand "$OUT.bfm" --strip-comments -o "$OUT.bf"
echo "Built $OUT.bf"
if [[ "$BF_HOST" == 1 ]]; then
    echo "Run: $ROOT/target/release/rbf $OUT.bf --headless --timeout 30"
else
    echo "Run in a regular Brainfuck interpreter with 16-bit wrapping cells and a sufficiently large wrapping tape (size divisible by 8)."
fi
