#!/usr/bin/env bash
# Build and run hosted FUZIX and its native applications, preserving the root image.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${FUSE_X_BUILD:-$ROOT/fuse-x/build}"
run_only=0
build_only=0
fresh=0
install_apps=0
disk=""
ripple_disk=""
share=""
share_set=0
session_action=""
detach=0
headless=0
telnet=0
one_shot=0
option_count=$#
mode=--run
mode_set=0
value=""
timeout="${RCC_NATIVE_TIMEOUT:-}"
default_network=off
if [[ "$(uname -s)" == Darwin ]]; then default_network=shared; fi
network="${RCC_NATIVE_NET:-${RIPPLE_NET:-$default_network}}"

usage() {
    cat <<'HELP'
Usage: scripts/run-fuse-x.sh [options]

Build Fuse-X, prepare its root image, and start the native FUZIX shell.
Try help, ls, native-hello, check, and shutdown. Files persist across launches.
Use shutdown for a clean exit. The Bourne shell supports scripts, pipelines,
redirection and job control. Ripple RXE executables require native recompilation.

  --run-only           Use existing executable and image without building
  --build-only         Build and prepare the image without running
  --disk IMAGE         Use an existing raw C16 V2 root image without installing apps
  --install-apps       Install native programs on an explicit --disk
  --share DIRECTORY    Mount another Mac folder read/write at /mnt/host
  --no-share           Disable the default shared folder for this session
  --detach             Start in the background; leave the console available
  --attach             Reconnect to the running session without building
  --stop               Orderly shutdown of the running session without building
  --headless           Start in the background without a local shell
  --telnet             Start the guest Telnet service (explicit opt-in)
  --one-shot           Exit when the first shell exits; no reconnectable session
  --from-ripple IMAGE  Extract ROOTFS.IMG from an existing Ripple FAT disk
                       into a separate default image; the source is unchanged
  --fresh              Replace the default root image, saving the old as .bak
  --run                Start native /init and shell (default)
  --boot-check         Check kernel initialization and clocks, read-only
  --write-check VALUE  Write VALUE to /tmp/fuse-x-check and verify it
  --read-check VALUE   Verify previously saved VALUE, opening the image read-only
  --net MODE          shared (default on macOS), host, auto, off, or a socket path
                       shared adds NAT for internet access; host is host-only.
                       Other platforms default to off. Interactive consoles attach at startup
                       so the vmnet sudo prompt precedes guest input handling.
  --timeout N          Limit execution to N host seconds (checks default to 10;
                       interactive sessions have no default time limit)
  -h, --help           Show this help

Default image: fuse-x/build/ROOTFS.IMG; preserved across launches and rebuilds.
Default shared folder: fuse-x-shared/ at the repo root, mounted at /mnt/host.
It is created automatically and ignored by Git. Check modes do not mount it.
Ctrl-] detaches an interactive console. Exiting its shell starts a new shell;
use shutdown or --stop to stop the kernel. --attach replays recent console output.
Set FUSE_X_BUILD for another build directory. Build output goes to build.log
there. Shell sessions and --write-check open the selected image read/write.
Normal builds update bootstrap manifests and native executables on the default
image, preserving other files and saving a backup when the image changes.
Examples:
  scripts/run-fuse-x.sh
  scripts/run-fuse-x.sh --run-only
  scripts/run-fuse-x.sh --run-only --net shared
  scripts/run-fuse-x.sh --run-only --net off
  scripts/run-fuse-x.sh --write-check 'hello from Fuse-X'
  scripts/run-fuse-x.sh --read-check 'hello from Fuse-X'
HELP
}
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --run-only) run_only=1; shift ;;
        --build-only) build_only=1; shift ;;
        --fresh) fresh=1; shift ;;
        --install-apps) install_apps=1; shift ;;
        --detach) detach=1; shift ;;
        --headless) headless=1; detach=1; shift ;;
        --telnet) telnet=1; shift ;;
        --one-shot) one_shot=1; shift ;;
        --attach|--stop) session_action="${1#--}"; shift ;;
        --no-share)
            [[ "$share_set" == 0 ]] || fail 'choose only one of --share and --no-share'
            share_set=1; shift ;;
        --disk|--from-ripple|--timeout|--write-check|--read-check|--net|--share)
            [[ $# -ge 2 ]] || fail "$1 requires a value"
            case "$1" in
                --disk) [[ -n "$2" ]] || fail '--disk requires an image'; disk="$2" ;;
                --from-ripple) [[ -n "$2" ]] || fail '--from-ripple requires an image'; ripple_disk="$2" ;;
                --timeout) timeout="$2" ;;
                --net) [[ -n "$2" ]] || fail '--net requires a mode or socket path'; network="$2" ;;
                --share)
                    [[ "$share_set" == 0 ]] || fail 'choose only one of --share and --no-share'
                    [[ -d "$2" ]] || fail "shared directory not found: $2"
                    share="$(cd "$2" && pwd -P)"; share_set=1 ;;
                --write-check|--read-check)
                    [[ "$mode_set" == 0 ]] || fail 'choose only one check mode'
                    mode="$1"; value="$2"; mode_set=1 ;;
            esac
            shift 2 ;;
        --run|--boot-check)
            [[ "$mode_set" == 0 ]] || fail 'choose only one check mode'
            mode="$1"; mode_set=1; shift ;;
        *) fail "unknown option: $1 (use --help)" ;;
    esac
done
if [[ -n "$session_action" ]]; then
    [[ "$option_count" == 1 ]] || fail '--attach and --stop must be used alone (set FUSE_X_BUILD for another session)'
    exec python3 "$ROOT/fuse-x/session.py" "$session_action" --state "$OUT/session"
fi
if [[ "$mode" != --run && ( "$detach" == 1 || "$headless" == 1 || "$telnet" == 1 || "$one_shot" == 1 ) ]]; then
    fail 'session options require --run'
fi
if [[ "$one_shot" == 1 && "$detach" == 1 ]]; then fail '--one-shot cannot be combined with --detach or --headless'; fi
if [[ -n "$share" && "$mode" != --run ]]; then fail '--share requires --run'; fi
if [[ -n "$timeout" ]]; then
    [[ "$timeout" =~ ^[0-9]+$ && "$timeout" =~ [1-9] ]] || fail '--timeout must be a positive whole number'
elif [[ "$mode" != --run ]]; then timeout=10
fi
if [[ "$run_only" == 1 && ( "$build_only" == 1 || "$fresh" == 1 || -n "$ripple_disk" || "$install_apps" == 1 ) ]]; then
    fail '--run-only cannot be combined with --build-only, --fresh, --from-ripple, or --install-apps'
fi
explicit_disk=0
if [[ -n "$disk" ]]; then
    explicit_disk=1
    [[ -f "$disk" ]] || fail "disk not found: $disk"
    [[ "$fresh" == 0 && -z "$ripple_disk" ]] || fail '--disk cannot be combined with --fresh or --from-ripple'
else
    disk="$OUT/ROOTFS.IMG"
fi
if [[ -n "$ripple_disk" ]]; then
    [[ -f "$ripple_disk" ]] || fail "Ripple disk not found: $ripple_disk"
    [[ ! -e "$disk" || "$fresh" == 1 ]] || fail 'default root image already exists; use --fresh to replace it with the extracted root'
fi
if python3 "$ROOT/fuse-x/session.py" status --state "$OUT/session"; then
    fail 'Fuse-X is already running; use --attach or --stop before building or opening its image'
fi
if [[ -f "$disk" ]] && command -v lsof >/dev/null 2>&1 && lsof -t -- "$disk" >/dev/null 2>&1; then
    fail "root image is already open: $disk; stop that session first"
fi

if [[ "$run_only" == 0 ]]; then
    if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
        export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    fi
    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    log="$OUT/build.log"
    printf 'Building Fuse-X (log: %s)...\n' "$log" >&2
    if ! make -C "$ROOT/fuse-x" tools >"$log" 2>&1; then cat "$log" >&2; exit 1; fi
    if ! make -C "$ROOT/fuse-x" "BUILD=$OUT" -j4 >>"$log" 2>&1; then cat "$log" >&2; exit 1; fi
fi
[[ -x "$OUT/fuse-x" ]] || fail "missing executable: $OUT/fuse-x; run without --run-only first"

if [[ "$run_only" == 0 && ( ! -e "$disk" || "$fresh" == 1 ) ]]; then
    temporary="$(mktemp -d "$OUT/.fuse-x-image.XXXXXX")"
    trap 'rm -rf "$temporary"' EXIT
    if [[ -n "$ripple_disk" ]]; then
        node "$ROOT/fuse-x/image.mjs" extract "$ripple_disk" "$temporary/ROOTFS.IMG" >/dev/null
    else
        node "$ROOT/fuse-x/image.mjs" create "$temporary/ROOTFS.IMG" --apps "$OUT/apps" >/dev/null
    fi
    if [[ -e "$disk" ]]; then
        backup="$disk.bak"
        suffix=1
        while [[ -e "$backup" || -L "$backup" ]]; do backup="$disk.bak.$suffix"; suffix=$((suffix + 1)); done
        mv "$disk" "$backup"
        printf 'Previous root image saved to %s\n' "$backup" >&2
    fi
    mv "$temporary/ROOTFS.IMG" "$disk"
    rmdir "$temporary"
    trap - EXIT
    printf 'Prepared root image: %s\n' "$disk" >&2
fi
if [[ "$run_only" == 0 && ( "$explicit_disk" == 0 || "$install_apps" == 1 ) ]]; then
    node "$ROOT/fuse-x/image.mjs" install "$disk" --apps "$OUT/apps" >&2
fi
[[ -f "$disk" ]] || fail "disk not found: $disk; run without --run-only first"
if [[ "$build_only" == 1 ]]; then
    printf 'Fuse-X ready: %s\nRoot image: %s\n' "$OUT/fuse-x" "$disk"
    exit 0
fi
printf 'Running Fuse-X %s using %s\n' "$mode" "$disk" >&2
export RCC_NATIVE_HEADLESS=1 RCC_NATIVE_NET="$network"
if [[ -n "$timeout" ]]; then export RCC_NATIVE_TIMEOUT="$timeout"; else unset RCC_NATIVE_TIMEOUT; fi
if [[ "$mode" == --run ]]; then
    if [[ "$share_set" == 0 ]]; then
        mkdir -p "$ROOT/fuse-x-shared"
        share="$(cd "$ROOT/fuse-x-shared" && pwd -P)"
    fi
    arguments=("$OUT/fuse-x" "$mode" "$disk")
    if [[ -n "$share" ]]; then arguments+=(--share "$share"); fi
    if [[ "$headless" == 1 ]]; then arguments+=(--headless); fi
    if [[ "$telnet" == 1 ]]; then arguments+=(--telnet); fi
    if [[ "$one_shot" == 1 ]]; then
        exec python3 "$ROOT/fuse-x/console.py" "${arguments[@]}" --one-shot
    fi
    session_options=()
    if [[ "$detach" == 1 ]]; then session_options+=(--detach); fi
    exec python3 "$ROOT/fuse-x/session.py" start --state "$OUT/session" "${session_options[@]}" -- "${arguments[@]}"
elif [[ "$mode" == --boot-check ]]; then
    exec "$OUT/fuse-x" "$mode" "$disk"
else
    exec "$OUT/fuse-x" "$mode" "$disk" "$value"
fi
