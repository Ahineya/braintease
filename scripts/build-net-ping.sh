#!/usr/bin/env bash
# Build the same freestanding protocol implementation for every execution host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-rvm}"
DEMO="${NET_DEMO:-ping}"
case "$DEMO" in ping|monitor|dns-ping|dhcp-ping|dhcp-monitor|tcp-echo|http-get|http-server) ;; *) echo "Unknown NET_DEMO: $DEMO" >&2; exit 2 ;; esac
OUT="${2:-$ROOT/build/network/$MODE}"
if [[ $# -ge 2 ]]; then shift 2; else shift "$#"; fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RCC="$ROOT/target/release/rcc"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"
sources=("${NET_SOURCE:-$ROOT/c-test/examples/network/$DEMO.c}")
library_sources=("$ROOT"/runtime/net/*.c "$ROOT/runtime/string.c")
if [[ "${NET_FAKE_NIC:-0}" != 1 ]]; then library_sources+=("$ROOT/runtime/vm_devices.c" "$ROOT/runtime/vm_network.c"); fi
case "$MODE" in
    native)
        sources+=("${library_sources[@]}")
        objects=()
        for source in "${sources[@]}"; do
            unit="$(basename "${source%.c}")"
            "$RCC" compile "$source" --target native --emit-ir -I "$ROOT/runtime/include" \
                -DRIPPLE_MMIO_NO_INLINE=1 "$@" > "$OUT/$unit.ir"
            objects+=("$OUT/$unit.ir")
        done
        "$RCC" link-native "${objects[@]}" -o "$OUT/$DEMO"
        echo "$OUT/$DEMO"
        ;;
    rvm|rbf)
        bank="${BANK_SIZE:-4096}"
        echo "$bank" > "$OUT/bank-size"
        objects=("$OUT/crt0.pobj")
        "$RASM" assemble "$ROOT/runtime/crt0.asm" -o "$OUT/crt0.pobj" --bank-size "$bank" --max-immediate 65535
        if [[ "${NET_PRIVATE_RUNTIME:-0}" == 1 ]]; then
            # Isolate guest-library builds from concurrent compiler/runtime work.
            sources+=("${library_sources[@]}" "$ROOT/runtime/putchar.c" "$ROOT/runtime/puts.c")
        else
            make -s -C "$ROOT/runtime" BANK_SIZE="$bank" libruntime.par
            objects+=("$ROOT/runtime/libruntime.par")
        fi
        for source in "${sources[@]}"; do
            unit="$(basename "${source%.c}")"
            "$RCC" compile "$source" -o "$OUT/$unit.asm" -I "$ROOT/runtime/include" --bank-size "$bank" "$@"
            "$RASM" assemble "$OUT/$unit.asm" -o "$OUT/$unit.pobj" --bank-size "$bank" --max-immediate 65535
            objects+=("$OUT/$unit.pobj")
        done
        "$RLINK" "${objects[@]}" -f binary --bank-size "$bank" -o "$OUT/$DEMO.bin"
        if [[ "$MODE" == rbf ]]; then
            "$RLINK" "${objects[@]}" -f macro --standalone --bf-host --memory-banks 8 --bank-size "$bank" -o "$OUT/$DEMO.bfm"
            "$ROOT/src/bf-macro-expander/target/release/bfm" expand "$OUT/$DEMO.bfm" --strip-comments -o "$OUT/$DEMO.bf"
        fi
        echo "$OUT/$DEMO.$([[ "$MODE" == rbf ]] && echo bf || echo bin) (bank size $bank)"
        ;;
    *) echo "Usage: $0 rvm|rbf|native [output-directory] [rcc -D options ...]" >&2; exit 2 ;;
esac
