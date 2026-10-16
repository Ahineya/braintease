#!/usr/bin/env bash
# Guest HTTP/1.0 GET, with an automatic ordinary host HTTP fixture by default.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode=native
host=192.168.64.1
path=/
port=8080
policy=host
dhcp=1
server=1
jit=0
build_only=0
timeout=60
usage() {
    echo "Usage: $0 [native|rvm] [--jit] [--host NAME_OR_IP] [--path /PATH] [--port PORT] [--shared] [--socket PATH] [--static] [--no-server] [--timeout SECONDS] [--build-only]"
    echo "Default: guest DHCP, automatic host-only networking and a temporary HTTP server on 192.168.64.1:8080."
    echo "--host, --path, --shared or --socket selects an external service and disables the temporary server."
    echo "Example: $0 rvm --jit --shared --host example.com --port 80 --path /"
    echo "Plain HTTP only; hostname resolution is performed by the guest DNS library."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm) mode="$1"; shift ;;
        --jit) jit=1; shift ;;
        --static) dhcp=0; shift ;;
        --no-server) server=0; shift ;;
        --shared) policy=shared; server=0; shift ;;
        --build-only) build_only=1; shift ;;
        --host|--path|--port|--socket|--timeout)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            case "$1" in
                --host) host="$2"; server=0 ;;
                --path) path="$2"; server=0 ;;
                --port) port="$2" ;;
                --socket) policy="$2"; server=0 ;;
                --timeout) timeout="$2" ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ "$mode" == native && "$jit" == 1 ]]; then echo "--jit applies to RVM." >&2; exit 2; fi
# These strings become C literals, not shell commands. Restrict the accepted
# alphabet before passing them as -D values; Python validates port/deadline too.
http_values="$(python3 - "$host" "$path" "$port" "$timeout" "$server" <<'PY'
import re, sys
host, path, port, timeout, server = sys.argv[1:]
assert re.fullmatch(r'[A-Za-z0-9.-]{1,253}', host), 'invalid hostname/IPv4'
assert 1 <= len(path) <= 255 and path.startswith('/'), 'path must start with / and fit 255 characters'
assert all(33 <= ord(c) <= 126 and c not in '\\"#' for c in path), 'path must be an ASCII origin-form without quotes, backslashes or fragment'
assert 1 <= int(port) <= 65535 and 1 <= int(timeout) <= 600, 'invalid port or timeout (1..600 seconds)'
assert server != '1' or int(port) >= 1024, 'automatic server requires an unprivileged port'
print(int(port), int(timeout))
PY
)"
read -r port timeout <<< "$http_values"
if [[ "$server" == 1 && "$dhcp" != 1 ]]; then echo "The automatic host fixture requires DHCP; use --no-server with --static." >&2; exit 2; fi
out="$ROOT/build/network/http-get/$mode"
mkdir -p "$out"
echo "Building guest HTTP client for $mode (log: $out/build.log)..."
request_ms=$((timeout * 500))
if [[ "$request_ms" -gt 30000 ]]; then request_ms=30000; fi
flags=("-DHTTP_DHCP=$dhcp" "-DHTTP_HOST=\"$host\"" "-DHTTP_PATH=\"$path\"" "-DHTTP_PORT=$port" "-DHTTP_TIMEOUT_MS=${request_ms}UL")
if ! NET_DEMO=http-get NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 "$ROOT/scripts/build-net-ping.sh" "$mode" "$out" "${flags[@]}" > "$out/build.log" 2>&1; then
    cat "$out/build.log" >&2; exit 1
fi
if [[ "$build_only" == 1 ]]; then echo "Built in $out"; exit 0; fi
if [[ "$server" == 1 ]]; then
    command=(python3 "$ROOT/scripts/test-net-http-live.py" --mode "$mode" --case length --port "$port" --timeout "$timeout" --verbose)
    if [[ "$jit" == 1 ]]; then command+=(--jit); fi
    exec "${command[@]}"
fi
if [[ "$mode" == native ]]; then
    exec env RCC_NATIVE_NET="$policy" RCC_NATIVE_HEADLESS=1 RCC_NATIVE_PACED=1 RCC_NATIVE_TIMEOUT="$timeout" "$out/http-get"
fi
bank="$(cat "$out/bank-size")"
command=("$ROOT/target/release/rvm" "$out/http-get.bin" --net "$policy" --clock paced --headless --exit-code --timeout "$timeout" \
    --bank-size "$bank" --memory "$((bank * 8))" --disk "$out/disk.img")
if [[ "$jit" == 1 ]]; then command+=(--jit); fi
exec "${command[@]}"
