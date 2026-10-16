#!/usr/bin/env bash
# Guest DHCP + TCP listener + HTTP page. The host only transports Ethernet.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode=native
port=8080
policy=host
dhcp=1
jit=0
build_only=0
test_live=0
usage() {
    echo "Usage: $0 [native|rvm|rbf] [--jit] [--port PORT] [--static] [--socket PATH] [--test] [--build-only]"
    echo "Starts a guest HTTP server and prints its ready URL. Open that URL in your browser; Ctrl-C stops it."
    echo "--test runs curl, malformed/split requests, timeouts and reuse tests, then stops the guest."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm|rbf) mode="$1"; shift ;;
        --jit) jit=1; shift ;;
        --static) dhcp=0; shift ;;
        --build-only) build_only=1; shift ;;
        --test) test_live=1; shift ;;
        --port|--socket)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            case "$1" in --port) port="$2" ;; --socket) policy="$2" ;; esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ "$mode" == native && "$jit" == 1 ]]; then echo "--jit applies to the VMs." >&2; exit 2; fi
port="$(python3 -c 'import sys; n=int(sys.argv[1]); assert 1<=n<=65535; print(n)' "$port")"
if [[ "$test_live" == 1 && ( "$dhcp" != 1 || "$policy" != host ) ]]; then
    echo "The automatic host tests require guest DHCP and host-only networking." >&2; exit 2
fi
if [[ "$mode" == rbf && "$test_live" == 1 ]]; then
    echo "Automatic HTTP tests cover native/RVM. Run rbf without --test for an explicit slower server run." >&2; exit 2
fi
out="$ROOT/build/network/http-server/$mode"
mkdir -p "$out"
echo "Building guest HTTP server for $mode (log: $out/build.log)..."
if ! NET_DEMO=http-server NET_PRIVATE_RUNTIME=1 BANK_SIZE=16384 "$ROOT/scripts/build-net-ping.sh" "$mode" "$out" \
    "-DHTTP_SERVER_PORT=$port" "-DHTTP_SERVER_DHCP=$dhcp" > "$out/build.log" 2>&1; then
    cat "$out/build.log" >&2; exit 1
fi
if [[ "$build_only" == 1 ]]; then echo "Built in $out"; exit 0; fi
if [[ "$test_live" == 1 ]]; then
    command=(python3 "$ROOT/scripts/test-net-http-server-live.py" --mode "$mode")
    if [[ "$jit" == 1 ]]; then command+=(--jit); fi
    exec "${command[@]}"
fi
if [[ "$mode" == native ]]; then
    exec env RCC_NATIVE_NET="$policy" RCC_NATIVE_HEADLESS=1 RCC_NATIVE_PACED=1 "$out/http-server"
fi
extension=bin
if [[ "$mode" == rbf ]]; then extension=bf; fi
command=("$ROOT/target/release/$mode" "$out/http-server.$extension" --net "$policy" --clock paced --headless --exit-code)
if [[ "$mode" == rvm ]]; then
    bank="$(cat "$out/bank-size")"
    command+=(--bank-size "$bank" --memory "$((bank * 8))" --disk "$out/server.disk")
fi
if [[ "$jit" == 1 ]]; then command+=(--jit); fi
exec "${command[@]}"
