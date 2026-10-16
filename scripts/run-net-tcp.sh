#!/usr/bin/env bash
# Default: guest DHCP + TCP, ordinary host echo service, automatic vmnet setup.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode=native
port=7777
peer=""
policy=host
dhcp=1
server=1
jit=0
build_only=0
timeout=""
usage() {
    echo "Usage: $0 [native|rvm|rbf] [--jit] [--port PORT] [--peer IPv4] [--socket PATH] [--static] [--no-server] [--shared] [--timeout SECONDS] [--build-only]"
    echo "Default: acquire a guest lease and echo 4097 binary octets through an ordinary host TCP server."
    echo "--peer or --socket selects an external service/packet peer and disables the temporary host server."
    echo "Automatic echo timeout: 60 seconds, or 1800 for RBF; --timeout overrides it."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm|rbf) mode="$1"; shift ;;
        --jit) jit=1; shift ;;
        --static) dhcp=0; shift ;;
        --no-server) server=0; shift ;;
        --shared) policy=shared; shift ;;
        --build-only) build_only=1; shift ;;
        --peer|--port|--socket|--timeout)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            case "$1" in --peer) peer="$2"; server=0 ;; --port) port="$2" ;; --socket) policy="$2"; server=0 ;; --timeout) timeout="$2" ;; esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ "$mode" == native && "$jit" == 1 ]]; then echo "--jit applies to the VMs." >&2; exit 2; fi
port="$(python3 -c 'import sys; p=int(sys.argv[1]); assert 1<=p<=65535; print(p)' "$port")"
if [[ -n "$timeout" ]]; then
    timeout="$(python3 -c 'import sys; t=int(sys.argv[1]); assert t>0; print(t)' "$timeout")"
fi
if [[ "$server" == 1 && "$port" -lt 1024 ]]; then echo "The automatic host echo needs an unprivileged port (1024..65535)." >&2; exit 2; fi
if [[ "$server" == 1 && ( "$dhcp" != 1 || "$policy" != host ) ]]; then
    echo "Automatic host echo requires host-only networking and DHCP. Use --no-server for other setups." >&2; exit 2
fi
flags=("-DTCP_DHCP=$dhcp" "-DTCP_PORT=$port")
if [[ -n "$peer" ]]; then
    peer="$(python3 -c 'import ipaddress,sys; print(str(ipaddress.IPv4Address(sys.argv[1])).replace(".",","))' "$peer")"
    flags+=("-DTCP_PEER_IP=$peer")
fi
out="$ROOT/build/network/tcp-echo/$mode"
mkdir -p "$out"
echo "Building guest TCP echo for $mode (log: $out/build.log)..."
if ! NET_DEMO=tcp-echo NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 "$ROOT/scripts/build-net-ping.sh" "$mode" "$out" "${flags[@]}" > "$out/build.log" 2>&1; then
    cat "$out/build.log" >&2; exit 1
fi
if [[ "$build_only" == 1 ]]; then echo "Built in $out"; exit 0; fi
if [[ "$server" == 1 ]]; then
    command=(python3 "$ROOT/scripts/test-net-tcp-live.py" --mode "$mode" --port "$port" --verbose)
    if [[ "$jit" == 1 ]]; then command+=(--jit); fi
    if [[ -n "$timeout" ]]; then command+=(--timeout "$timeout"); fi
    exec "${command[@]}"
fi
if [[ "$mode" == native ]]; then
    if [[ -n "$timeout" ]]; then export RCC_NATIVE_TIMEOUT="$timeout"; fi
    exec env RCC_NATIVE_NET="$policy" RCC_NATIVE_HEADLESS=1 RCC_NATIVE_PACED=1 "$out/tcp-echo"
fi
extension=bin
if [[ "$mode" == rbf ]]; then extension=bf; fi
command=("$ROOT/target/release/$mode" "$out/tcp-echo.$extension" --net "$policy" --clock paced --headless --exit-code)
if [[ "$mode" == rvm ]]; then
    bank="$(cat "$out/bank-size")"
    command+=(--bank-size "$bank" --memory "$((bank * 8))" --disk "$out/disk.img")
fi
if [[ "$jit" == 1 ]]; then command+=(--jit); fi
if [[ -n "$timeout" ]]; then command+=(--timeout "$timeout"); fi
exec "${command[@]}"
