#!/usr/bin/env bash
# Build and boot FUZIX/Ripple, preserving the writable filesystem.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${FUZIX_BUILD:-$ROOT/build/fuzix}"
run_only=0
build_only=0
fresh=0
install_apps=0
jit=1
target=rvm
disk=""
args=()
usage() {
    cat <<'HELP'
Usage: scripts/run-fuzix.sh [rvm|rbf] [options] [host options...]

Boot FUZIX/Ripple. Try help, ls, cat README, echo hello > note, hello,
forkcheck, preemptcheck, and shutdown. Use shutdown before closing the VM.
Files persist across launches and kernel rebuilds.
Ctrl+C interrupts the foreground guest program. Ctrl+] stops the VM abruptly.

  --build-only     Build/update the default disk without booting
  --run-only       Boot existing artifacts without building or updating
  --rbf            Run the Brainfuck CPU (also accepts positional rbf)
  --rvm            Run the native VM (default; also accepts positional rvm)
  --no-jit         Use the interpreter; the JIT is enabled by default
  --fresh          Reformat the default disk (saves the old disk as a backup)
  --disk IMAGE     Use an existing image without updating its contents
  --install-apps   Also update an explicit --disk image, preserving saved files
  --headless       Disable interactive terminal input
  --input TEXT     Preload commands, e.g. --input $'ls\nshutdown\n'
  --timeout N      Stop after N host seconds (not a clean guest shutdown)

The first build imports no network dependencies beyond the normal Rust tools.
Normal builds update the kernel and managed Unix executables on the default
disk, preserving user files. Compatible roots gain space and inodes automatically.
Older C16 V1 disks require --fresh for the larger C16 V2 block layout.
Normal launches check the disk and back up and repair recoverable shutdown
metadata before updating. Structural damage stops the update. --disk skips
updates unless --install-apps is supplied. Set FUZIX_BUILD for another build
directory. Root disks use the Ripple C16 filesystem ABI.
RBF uses the same disk and needs about 547 MiB for its BF tape alone. Booting
can take much longer; --telemetry-lite shows progress on RBF.
HELP
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --rvm) target=rvm; shift ;;
        --rbf) target=rbf; shift ;;
        rvm|rbf)
            # A bare target precedes forwarded host options; their values may
            # themselves be "rvm" or "rbf" (for example --input rbf).
            if [[ ${#args[@]} == 0 ]]; then target="$1"; else args+=("$1"); fi
            shift ;;
        --run-only) run_only=1; shift ;;
        --build-only) build_only=1; shift ;;
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
if [[ "$run_only" == 1 && ( "$fresh" == 1 || "$build_only" == 1 || "$install_apps" == 1 ) ]]; then
    echo 'error: --run-only cannot be combined with --fresh, --build-only, or --install-apps' >&2; exit 1
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
    cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p "$target"
    cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink
    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    mmu=1
    if [[ "$target" == rbf ]]; then mmu=0; fi
    make -C "$ROOT/ports/fuzix" "BUILD=$OUT" "MMU=$mmu" -j4 images >&2
    if [[ "$target" == rbf ]]; then
        make -C "$ROOT/ports/fuzix" "BUILD=$OUT" MMU=0 -j4 bf >&2
    fi
fi
if [[ "$target" == rbf && -f "$OUT/.mmu" && "$(cat "$OUT/.mmu")" != 0 ]]; then
    echo 'error: current artifacts use the MMU; run without --run-only to build the Brainfuck profile' >&2
    exit 1
fi
update=0
if [[ -z "$disk" ]]; then
    disk="$OUT/fuzix.img"
    if [[ "$run_only" == 0 ]]; then update=1; fi
fi
if [[ "$install_apps" == 1 ]]; then update=1; fi
program=bios.bin
if [[ "$target" == rbf ]]; then program=bios.bf; fi
for required in "$OUT/$program" "$ROOT/target/release/$target"; do
    if [[ ! -f "$required" ]]; then echo "error: missing $required; run without --run-only first" >&2; exit 1; fi
done
if [[ "$update" == 0 && ! -f "$disk" ]]; then echo "error: disk not found: $disk" >&2; exit 1; fi
launch=(--build "$OUT" --disk "$disk" --backend "$target" --vm "$ROOT/target/release/$target")
if [[ "$update" == 1 ]]; then launch+=(--update); fi
if [[ "$fresh" == 1 ]]; then launch+=(--fresh); fi
if [[ "$build_only" == 1 ]]; then launch+=(--build-only); fi
if [[ "$jit" == 1 ]]; then launch+=(-- --jit); else launch+=(--); fi
if [[ ${#args[@]} -gt 0 ]]; then launch+=("${args[@]}"); fi
exec python3 "$ROOT/ports/fuzix/tools/launch.py" "${launch[@]}"
