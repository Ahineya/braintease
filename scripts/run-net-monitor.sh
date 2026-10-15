#!/usr/bin/env bash
# Build and run the C demo. Host networking is owned by the runtime itself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE=native
NET_SOCKET=""
JIT=0
usage() {
    echo "Usage: $0 [native|rvm|rbf] [--jit] [--socket SOCKET]"
    echo "Build and run a persistent C ping monitor (native by default)."
    echo "The runtime starts host-only Ethernet on the first NIC query."
    echo "Use --socket to attach to an existing Ethernet helper instead."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm|rbf) MODE="$1"; shift ;;
        --jit) JIT=1; shift ;;
        --socket) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; NET_SOCKET="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ "$MODE" == native && "$JIT" == 1 ]]; then
    echo "--jit applies to rvm or rbf; native already runs compiled code." >&2; exit 2
fi
OUT="$ROOT/build/network/monitor/$MODE"
mkdir -p "$OUT"
echo "Building C ping monitor for $MODE (log: $OUT/build.log)..."
if ! NET_DEMO=monitor "$ROOT/scripts/build-net-ping.sh" "$MODE" "$OUT" > "$OUT/build.log" 2>&1; then
    cat "$OUT/build.log" >&2; exit 1
fi
case "$MODE" in
    native)
        if [[ -n "$NET_SOCKET" ]]; then export RCC_NATIVE_NET="$NET_SOCKET"; fi
        exec env RCC_NATIVE_HEADLESS=1 "$OUT/monitor"
        ;;
    rvm)
        bank="$(cat "$OUT/bank-size")"
        command=("$ROOT/target/release/rvm" "$OUT/monitor.bin" --headless --exit-code
            --bank-size "$bank" --memory "$((bank * 8))" --disk "$OUT/disk.img")
        ;;
    rbf) command=("$ROOT/target/release/rbf" "$OUT/monitor.bf" --headless --exit-code) ;;
esac
if [[ -n "$NET_SOCKET" ]]; then command+=(--net "$NET_SOCKET"); fi
if [[ "$JIT" == 1 ]]; then command+=(--jit); fi
exec "${command[@]}"
