#!/usr/bin/env bash
# Build and boot Riplix, preserving the writable filesystem.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${RIPLIX_BUILD:-${FUZIX_BUILD:-$ROOT/build/fuzix}}"
run_only=0
build_only=0
rebuild=0
fresh=0
install_apps=0
jit=1
disk=""
args=()
usage() {
    cat <<'HELP'
Usage: scripts/run-riplix.sh [options] [host options...]

Boot Riplix. Try help, ls, cat README, echo hello > note, hello,
forkcheck, preemptcheck, telegram, music /music, lynx http://wiby.me/, and shutdown.
Use shutdown before closing the VM.
Files persist across launches and kernel rebuilds.
Ctrl+C interrupts the foreground guest program. Ctrl+] stops the VM abruptly.

  --incremental    Build only changed inputs (default)
  --rebuild        Force rebuilding OS/programs; preserve the disk
  --build-only     Build/update the default disk without booting
  --run-only       Boot existing artifacts without building or updating
  --rvm            Run RVM (the only supported backend; also accepts positional rvm)
  --no-jit         Use the interpreter; the JIT is enabled by default
  --net POLICY     shared (NAT/internet, default), host (host-only), off, or NIC socket
  --fresh          Reformat the default disk (saves the old disk as a backup)
  --disk IMAGE     Use an existing image without updating its contents
  --install-apps   Also update an explicit --disk image, preserving saved files
  --visual         Enable graphics; run startx for the Nano-X desktop
  --headless       Disable interactive terminal input
  --input TEXT     Preload commands, e.g. --input $'ls\nshutdown\n'
  --timeout N      Stop after N host seconds (not a clean guest shutdown)
  --audio MODE     auto, on (interactive default), or off
  --audio-wav FILE Capture rendered PCM in a WAV file

The first build imports no network dependencies beyond the normal Rust tools.
Normal launches use content fingerprints plus Make dependencies to reuse unchanged
compilers, kernels and applications. The first launch after this upgrade can
rebuild older artifacts once. Normal builds update the kernel and managed Unix executables on the default
disk, preserving user files. Compatible roots gain space and inodes automatically.
C16 V2 roots upgrade to V3 (1 GiB) with a backup. V1 disks require --fresh.
Image updates keep one previous disk in DISK.bak (RIPPLE_IMAGE_BACKUPS overrides).
Normal launches check the disk and back up and repair recoverable shutdown
metadata and abandoned anonymous pipes before updating. Structural damage stops
the update. --disk skips updates unless --install-apps is supplied. Set
RIPLIX_BUILD for another build directory (FUZIX_BUILD is also accepted). Root disks use the Ripple C16 filesystem ABI.
Riplix requires the MMU. Non-MMU builds and the RBF backend are retired.
For the separate boot-only Brainfuck experiment, use scripts/run-riplix-bf.sh.
HELP
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --rvm) shift ;;
        --rbf) echo 'error: non-MMU Riplix/RBF is retired; use RVM' >&2; exit 1 ;;
        rvm|rbf)
            # A bare target precedes forwarded host options; their values may
            # themselves be "rvm" or "rbf" (for example --input rbf).
            if [[ ${#args[@]} == 0 ]]; then
                if [[ "$1" == rbf ]]; then
                    echo 'error: non-MMU Riplix/RBF is retired; use RVM' >&2; exit 1
                fi
            else args+=("$1"); fi
            shift ;;
        --run-only) run_only=1; shift ;;
        --build-only) build_only=1; shift ;;
        --incremental) shift ;;
        --rebuild) rebuild=1; shift ;;
        --fresh) fresh=1; shift ;;
        --install-apps) install_apps=1; shift ;;
        --jit) jit=1; shift ;;
        --no-jit) jit=0; shift ;;
        --disk) disk="${2:?--disk requires an existing image}"; shift 2 ;;
        --disk=*) disk="${1#--disk=}"; shift ;;
        --) shift; args+=("$@"); break ;;
        *) args+=("$1"); shift ;;
    esac
done
if [[ "$run_only" == 1 && ( "$fresh" == 1 || "$rebuild" == 1 || "$build_only" == 1 || "$install_apps" == 1 ) ]]; then
    echo 'error: --run-only cannot be combined with --fresh, --rebuild, --build-only, or --install-apps' >&2; exit 1
fi
if [[ -n "$disk" && ( "$fresh" == 1 || ! -f "$disk" ) ]]; then
    echo 'error: --disk requires an existing image and cannot be combined with --fresh' >&2; exit 1
fi
if [[ "$run_only" == 0 ]]; then
    # Some macOS installations select older command-line linker/SDK pairs.
    if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
        export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    fi
    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    build_arguments=(fuzix --build "$OUT")
    if [[ "$rebuild" == 1 ]]; then build_arguments+=(--rebuild); fi
    printf 'Checking changed Riplix inputs (log: %s/build.log)...\n' "$OUT" >&2
    if ! python3 "$ROOT/scripts/build_progress.py" --log "$OUT/build.log" -- \
        python3 "$ROOT/scripts/build-os.py" "${build_arguments[@]}"; then
        exit 1
    fi
fi
if [[ ! -f "$OUT/.mmu" || "$(cat "$OUT/.mmu")" != 1 ]]; then
    echo 'error: non-MMU or unmarked Riplix artifacts are unsupported; rebuild without --run-only (MMU=0 is retired)' >&2
    exit 1
fi
update=0
if [[ -z "$disk" ]]; then
    disk="$OUT/fuzix.img"
    if [[ "$run_only" == 0 ]]; then update=1; fi
fi
if [[ "$install_apps" == 1 ]]; then update=1; fi
for required in "$OUT/bios.bin" "$ROOT/target/release/rvm"; do
    if [[ ! -f "$required" ]]; then echo "error: missing $required; run without --run-only first" >&2; exit 1; fi
done
if [[ "$update" == 0 && ! -f "$disk" ]]; then echo "error: disk not found: $disk" >&2; exit 1; fi
launch=(--build "$OUT" --disk "$disk" --backend rvm --vm "$ROOT/target/release/rvm")
if [[ "$update" == 1 ]]; then launch+=(--update); fi
if [[ "$fresh" == 1 ]]; then launch+=(--fresh); fi
if [[ "$build_only" == 1 ]]; then launch+=(--build-only); fi
if [[ "$jit" == 1 ]]; then launch+=(-- --jit); else launch+=(--); fi
if [[ ${#args[@]} -gt 0 ]]; then launch+=("${args[@]}"); fi
exec python3 "$ROOT/ports/fuzix/tools/launch.py" "${launch[@]}"
