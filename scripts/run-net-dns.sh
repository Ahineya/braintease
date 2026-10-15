#!/usr/bin/env bash
# The guest performs DNS and ICMP; the runtime supplies raw Ethernet only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode=native
net_policy=shared
net_name=syma-lang.org
net_ip=192.168.64.250
net_dns=1.1.1.1
jit=0
build_only=0
usage() {
    echo "Usage: $0 [native|rvm|rbf] [hostname] [--jit] [--ip IPv4] [--dns IPv4] [--socket SOCKET] [--build-only]"
    echo "Build and run guest DNS followed by three pings (defaults: native, syma-lang.org)."
    echo "On macOS the runtime starts shared-mode vmnet/NAT, without packet capture."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        native|rvm|rbf) mode="$1"; shift ;;
        --jit) jit=1; shift ;;
        --build-only) build_only=1; shift ;;
        --ip|--dns|--socket)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            case "$1" in --ip) net_ip="$2" ;; --dns) net_dns="$2" ;; --socket) net_policy="$2" ;; esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --*) usage >&2; exit 2 ;;
        *) net_name="$1"; shift ;;
    esac
done
if [[ "$mode" == native && "$jit" == 1 ]]; then echo "--jit applies to rvm/rbf." >&2; exit 2; fi
if [[ ! "$net_name" =~ ^[a-zA-Z0-9.-]+$ || ${#net_name} -gt 254 ]]; then echo "Expected an ASCII hostname." >&2; exit 2; fi
net_ip="$(python3 -c 'import ipaddress,sys; print(str(ipaddress.IPv4Address(sys.argv[1])).replace(".", ","))' "$net_ip")"
net_dns="$(python3 -c 'import ipaddress,sys; print(str(ipaddress.IPv4Address(sys.argv[1])).replace(".", ","))' "$net_dns")"
out="$ROOT/build/network/dns/$mode"
mkdir -p "$out"
echo "Building guest DNS/ping for $mode (log: $out/build.log)..."
if ! NET_DEMO=dns-ping BANK_SIZE=8192 "$ROOT/scripts/build-net-ping.sh" "$mode" "$out" \
    "-DDNS_NAME=\"$net_name\"" "-DNET_LOCAL_IP=$net_ip" "-DNET_DNS_IP=$net_dns" > "$out/build.log" 2>&1; then
    cat "$out/build.log" >&2; exit 1
fi
if [[ "$build_only" == 1 ]]; then echo "$out/dns-ping$([[ "$mode" == native ]] || echo .$([[ "$mode" == rvm ]] && echo bin || echo bf))"; exit 0; fi
if [[ "$mode" == native ]]; then
    exec env RCC_NATIVE_NET="$net_policy" RCC_NATIVE_PACED=1 RCC_NATIVE_HEADLESS=1 "$out/dns-ping"
fi
command=("$ROOT/target/release/$mode" "$out/dns-ping.$([[ "$mode" == rvm ]] && echo bin || echo bf)" --net "$net_policy" --headless --exit-code)
if [[ "$mode" == rvm ]]; then
    bank="$(cat "$out/bank-size")"
    command+=(--bank-size "$bank" --memory "$((bank * 8))" --clock paced --disk "$out/disk.img")
else
    command+=(--clock paced)
fi
if [[ "$jit" == 1 ]]; then command+=(--jit); fi
exec "${command[@]}"
