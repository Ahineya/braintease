#!/usr/bin/env bash
# Build and boot FUZIX/Ripple, preserving the writable filesystem.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${FUZIX_BUILD:-$ROOT/build/fuzix}"
run_only=0
build_only=0
fresh=0
jit=1
disk=""
args=()
usage() {
    cat <<'HELP'
Usage: scripts/run-fuzix.sh [options] [rvm options...]

Boot FUZIX/Ripple. Try help, ls, cat README, echo hello > note, hello,
forkcheck, preemptcheck, and shutdown. Use shutdown before closing the VM.
Files persist across launches and kernel rebuilds.

  --build-only     Build/update the default disk without booting
  --run-only       Boot existing artifacts without building or updating
  --no-jit         Use the interpreter; the JIT is enabled by default
  --fresh          Reformat the default disk (saves the old disk as a backup)
  --disk IMAGE     Use an existing image without updating its contents
  --headless       Disable interactive terminal input
  --input TEXT     Preload commands, e.g. --input $'ls\nshutdown\n'
  --timeout N      Stop after N host seconds (not a clean guest shutdown)

The first build imports no network dependencies beyond the normal Rust tools.
Normal builds update FUZIX.SYS and preserve ROOTFS.IMG exactly. Newly built
userspace programs are installed when creating a fresh disk. Set FUZIX_BUILD
to choose another build directory. Root disks use the Ripple C16 filesystem ABI.
HELP
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --run-only) run_only=1; shift ;;
        --build-only) build_only=1; shift ;;
        --fresh) fresh=1; shift ;;
        --jit) jit=1; shift ;;
        --no-jit) jit=0; shift ;;
        --disk) disk="${2:?--disk requires an existing image}"; shift 2 ;;
        --disk=*) disk="${1#--disk=}"; shift ;;
        --) shift; args+=("$@"); break ;;
        *) args+=("$1"); shift ;;
    esac
done
if [[ "$run_only" == 1 && ( "$fresh" == 1 || "$build_only" == 1 ) ]]; then
    echo 'error: --run-only cannot be combined with --fresh or --build-only' >&2; exit 1
fi
if [[ -n "$disk" && ( "$fresh" == 1 || ! -f "$disk" ) ]]; then
    echo 'error: --disk requires an existing image and cannot be combined with --fresh' >&2; exit 1
fi
if [[ "$run_only" == 0 ]]; then
    # Some macOS installations select older command-line linker/SDK pairs.
    if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
        export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    fi
    printf 'Checking compiler, assembler and VM...\n' >&2
    cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rvm
    cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink
    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    make -C "$ROOT/ports/fuzix" "BUILD=$OUT" -j4 images >&2
fi
update=0
if [[ -z "$disk" ]]; then
    disk="$OUT/fuzix.img"
    if [[ "$run_only" == 0 ]]; then update=1; fi
fi
for required in "$OUT/bios.bin" "$ROOT/target/release/rvm"; do
    if [[ ! -f "$required" ]]; then echo "error: missing $required; run without --run-only first" >&2; exit 1; fi
done
if [[ "$update" == 0 && ! -f "$disk" ]]; then echo "error: disk not found: $disk" >&2; exit 1; fi
launch=(--build "$OUT" --disk "$disk" --vm "$ROOT/target/release/rvm")
if [[ "$update" == 1 ]]; then launch+=(--update); fi
if [[ "$fresh" == 1 ]]; then launch+=(--fresh); fi
if [[ "$build_only" == 1 ]]; then launch+=(--build-only); fi
if [[ "$jit" == 1 ]]; then launch+=(-- --jit); else launch+=(--); fi
if [[ ${#args[@]} -gt 0 ]]; then launch+=("${args[@]}"); fi
exec python3 "$ROOT/ports/fuzix/tools/launch.py" "${launch[@]}"
