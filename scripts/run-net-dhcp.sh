#!/usr/bin/env bash
# Guest DHCP supplies IP, mask, router and DNS; the host supplies Ethernet only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode=native
policy=""
net_name=syma-lang.org
demo=dhcp-ping
jit=0
build_only=0
usage() {
    echo "Usage: $0 [native|rvm|rbf] [hostname] [--monitor] [--jit] [--socket SOCKET] [--host-only] [--build-only]"
    echo "Acquire a guest DHCP lease, resolve a hostname and ping it three times."
    echo "--monitor instead keeps the lease alive and logs incoming host pings."
    echo "macOS attachment defaults to shared for DNS/ping, host-only for monitor; capture disabled."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm|rbf) mode="$1"; shift ;;
        --monitor) demo=dhcp-monitor; shift ;;
        --jit) jit=1; shift ;;
        --build-only) build_only=1; shift ;;
        --host-only) policy=host; shift ;;
        --socket) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; policy="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --*) usage >&2; exit 2 ;;
        *) net_name="$1"; shift ;;
    esac
done
if [[ "$mode" == native && "$jit" == 1 ]]; then echo "--jit applies to rvm/rbf." >&2; exit 2; fi
if [[ ! "$net_name" =~ ^[a-zA-Z0-9.-]+$ || ${#net_name} -gt 254 ]]; then echo "Expected an ASCII hostname." >&2; exit 2; fi
if [[ -z "$policy" ]]; then
    if [[ "$demo" == dhcp-monitor ]]; then policy=host; else policy=shared; fi
fi
out="$ROOT/build/network/$demo/$mode"
mkdir -p "$out"
echo "Building $demo for $mode (log: $out/build.log)..."
if ! NET_DEMO="$demo" NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 "$ROOT/scripts/build-net-ping.sh" "$mode" "$out" \
    "-DDNS_NAME=\"$net_name\"" > "$out/build.log" 2>&1; then
    cat "$out/build.log" >&2; exit 1
fi
if [[ "$build_only" == 1 ]]; then echo "Built in $out"; exit 0; fi
if [[ "$mode" == native ]]; then
    exec env RCC_NATIVE_NET="$policy" RCC_NATIVE_PACED=1 RCC_NATIVE_HEADLESS=1 "$out/$demo"
fi
extension=bin
if [[ "$mode" == rbf ]]; then extension=bf; fi
command=("$ROOT/target/release/$mode" "$out/$demo.$extension" --net "$policy" --headless --exit-code --clock paced)
if [[ "$mode" == rvm ]]; then
    bank="$(cat "$out/bank-size")"
    command+=(--bank-size "$bank" --memory "$((bank * 8))" --disk "$out/disk.img")
fi
if [[ "$jit" == 1 ]]; then command+=(--jit); fi
exec "${command[@]}"
