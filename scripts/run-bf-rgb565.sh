#!/usr/bin/env bash
# Build and run a small RGB565 animation on the real Brainfuck CPU.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BF_RGB565_OUT:-$ROOT/build/bf-rgb565}"
build_only=0
visual=1
clock=paced
tick_hz=8
args=()

usage() {
    cat <<'EOF'
Usage: scripts/run-bf-rgb565.sh [--build-only] [--headless] [rbf options...]

Show a 16x12 RGB565 animation running on the Brainfuck CPU.
Close its window or press Ctrl-C to stop. No C runtime or disk is required.

  --build-only       Build the demo and tools without starting it
  --headless         Run without a window (for captures)
  --clock POLICY     paced (default), unthrottled, or stepped
  --tick-hz HZ       Animation tick rate (default 8)

Other options, such as --timeout, --capture-frame, --screenshot, --raw-frame,
--report and --no-optimize, are passed to rbf. BF_RGB565_OUT selects the output
directory; it defaults to build/bf-rgb565.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --build-only) build_only=1; shift ;;
        --headless) visual=0; shift ;;
        --visual) visual=1; shift ;;
        --clock) clock="${2:?--clock requires a value}"; shift 2 ;;
        --clock=*) clock="${1#--clock=}"; shift ;;
        --tick-hz) tick_hz="${2:?--tick-hz requires a value}"; shift 2 ;;
        --tick-hz=*) tick_hz="${1#--tick-hz=}"; shift ;;
        --) shift; args+=("$@"); break ;;
        *) args+=("$1"); shift ;;
    esac
done

# Cargo checks freshness and builds only changed/missing tools. Keep all demo
# objects under build/; never replace the normal C runtime's bank configuration.
cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rbf
cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink
cargo build --quiet --release --manifest-path "$ROOT/src/bf-macro-expander/Cargo.toml" --bin bfm
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
BANK_SIZE=512 MEMORY_BANKS=1 BF_HOST=1 "$ROOT/scripts/build-bf.sh" \
    "$ROOT/rbf/examples/rgb565.asm" "$OUT/rgb565"
if [[ "$build_only" == 1 ]]; then exit 0; fi

mode=--headless
if [[ "$visual" == 1 ]]; then mode=--visual; fi
exec "$ROOT/target/release/rbf" "$OUT/rgb565.bf" "$mode" \
    --clock "$clock" --tick-hz "$tick_hz" "${args[@]}"
