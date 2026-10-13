#!/usr/bin/env bash
# Build and boot the DOS-like rOS on the Rust Ripple VM.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROS_BUILD:-$ROOT/build/ros}"
build_only=0
fresh=0
disk=""
args=()

usage() {
    cat <<'EOF'
Usage: scripts/run-ros.sh [--build-only] [--fresh | --disk IMAGE] [rvm options...]

Build and boot rOS in the terminal using the Rust VM. At the C:\> prompt,
try HELP, DIR, HELLO, or EXIT. No graphics window is needed.

  --build-only       Build required images without starting the VM
  --fresh            Recreate the default disk with current OS code; erases files
  --disk IMAGE       Boot an existing FAT16 rOS disk; never rebuild that image
  --headless         Disable live terminal input (use --input for commands)
  --input TEXT       Preload shell commands; use $'DIR\nHELLO\nEXIT\n'
  --frequency HZ     Limit guest instruction speed, e.g. 2MHz
  --timeout SECONDS  Stop the VM after the given host wall time

Other arguments are passed to rvm; target/release/rvm --help lists them.
Cargo checks release tool freshness, then rebuilds the BIOS from source.
On first launch it also builds IO.SYS, KERNEL.SYS, COMMAND.COM and HELLO.RXE
and places them in build/ros/ros.img. Existing disks are kept across launches.
Use --fresh to update the default disk's OS files (this resets that disk).
Set ROS_BUILD to use a different build directory. Any image supplied with
--disk is left intact by the build; the running guest can still write to it.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --build-only) build_only=1; shift ;;
        --fresh) fresh=1; shift ;;
        --disk)
            disk="${2:?--disk requires an existing FAT16 image}"
            shift 2
            ;;
        --disk=*)
            disk="${1#--disk=}"
            [[ -n "$disk" ]] || { echo 'error: --disk requires an image' >&2; exit 1; }
            shift
            ;;
        --) shift; args+=("$@"); break ;;
        *) args+=("$1"); shift ;;
    esac
done

if [[ "$fresh" == 1 && -n "$disk" ]]; then
    echo 'error: --fresh only resets the default disk; it cannot be used with --disk' >&2
    exit 1
fi
if [[ -n "$disk" && ! -f "$disk" ]]; then
    echo "error: disk image not found: $disk" >&2
    exit 1
fi

printf 'Checking release compiler, assembler and Rust VM...\n' >&2
cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rvm
cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
make_args=(-C "$ROOT/c-test/examples/rOS" -B "BUILD=$OUT" BANK_SIZE=64000)
if [[ -z "$disk" ]]; then
    disk="$OUT/ros.img"
    if [[ "$fresh" == 1 || ! -f "$disk" ]]; then
        if [[ -f "$disk" ]]; then
            printf 'Recreating default rOS disk (all existing files are removed): %s\n' "$disk" >&2
        fi
        make_args+=(disk "DISK=$disk")
    else
        make_args+=(bios)
        printf 'Keeping existing rOS disk: %s (use --fresh to update/reset it)\n' "$disk" >&2
    fi
else
    make_args+=(bios)
fi

# The rOS Makefile does not track compiler/header changes. Rebuild the selected
# images explicitly; on-disk OS images are replaced only on creation/--fresh.
printf 'Building rOS (64,000-cell banks)...\n' >&2
make "${make_args[@]}" >&2
if [[ "$build_only" == 1 ]]; then exit 0; fi

printf 'Booting rOS; type HELP for commands or EXIT to stop.\n' >&2
run_args=("$OUT/bios.bin" --disk "$disk")
if [[ ${#args[@]} -gt 0 ]]; then run_args+=("${args[@]}"); fi
exec "$ROOT/target/release/rvm" "${run_args[@]}"
