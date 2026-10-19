#!/usr/bin/env bash
# Build and boot the DOS-like rOS on the Rust Ripple VM.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROS_BUILD:-$ROOT/build/ros}"
build_only=0
run_only=0
use_jit=1
fresh=0
update_disk=0
disk=""
args=()

usage() {
    cat <<'EOF'
Usage: scripts/run-ros.sh [--build-only | --run-only] [--fresh | --disk IMAGE] [rvm options...]

Build and boot rOS in the terminal using the Rust VM. At the C:\> prompt,
try HELP, DIR, HELLO, SIERP, BF, TYPE README.TXT, or EXIT.
No graphics window is needed.

  --build-only       Build/update images without starting the VM
  --run-only         Boot existing BIOS/disk; skip Cargo, Make and disk updates
  --no-jit           Use the interpreter (the JIT is enabled by default)
  --fresh            Recreate the default disk with current OS code; erases files
  --disk IMAGE       Boot an existing FAT16 rOS disk; never rebuild that image
  --headless         Disable live terminal input (use --input for commands)
  --input TEXT       Preload shell commands; use $'DIR\nHELLO\nEXIT\n'
  --frequency HZ     Limit guest instruction speed, e.g. 2MHz
  --timeout SECONDS  Stop the VM after the given host wall time

Other arguments are passed to rvm; target/release/rvm --help lists them.
Normal launches check the toolchain and rebuild changed sources incrementally.
The OS, bundled HELLO, SIERP and BF programs, and README.TXT are kept current
in build/ros/ros.img, preserving other files. Before changing an existing
disk, it keeps one previous image alongside it (ros.img.bak).
Use --fresh to reformat the default disk and remove all existing files.
Set ROS_BUILD to use a different build directory. Any image supplied with
--disk is left intact by the build; the running guest can still write to it.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --build-only) build_only=1; shift ;;
        --run-only) run_only=1; shift ;;
        --jit) use_jit=1; shift ;;
        --no-jit) use_jit=0; shift ;;
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

if [[ "$run_only" == 1 && ( "$build_only" == 1 || "$fresh" == 1 ) ]]; then
    echo 'error: --run-only cannot be combined with --build-only or --fresh' >&2
    exit 1
fi
if [[ "$fresh" == 1 && -n "$disk" ]]; then
    echo 'error: --fresh only resets the default disk; it cannot be used with --disk' >&2
    exit 1
fi
if [[ -n "$disk" && ! -f "$disk" ]]; then
    echo "error: disk image not found: $disk" >&2
    exit 1
fi

if [[ "$run_only" == 0 ]]; then
    printf 'Checking release compiler, assembler and Rust VM...\n' >&2
    cargo build --quiet --release --manifest-path "$ROOT/Cargo.toml" -p rcc-driver -p rvm
    cargo build --quiet --release --manifest-path "$ROOT/src/ripple-asm/Cargo.toml" --bin rasm --bin rlink

    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    make_args=(-C "$ROOT/c-test/examples/rOS" "BUILD=$OUT" BANK_SIZE=64000 SEGMENTED_STACK=0)
    if [[ -z "$disk" ]]; then
        update_disk=1
        disk="$OUT/ros.img"
        if [[ "$fresh" == 1 || ! -f "$disk" ]]; then
            if [[ -f "$disk" ]]; then
                printf 'Recreating default rOS disk (all existing files are removed): %s\n' "$disk" >&2
            fi
            make_args+=(disk "DISK=$disk")
        else
            make_args+=(images)
            printf 'Updating rOS files in existing disk: %s (other files are preserved)\n' "$disk" >&2
        fi
    else
        make_args+=(bios)
    fi

    # The Makefile tracks sources, headers, tools and build configuration.
    printf 'Checking rOS build (64,000-cell banks)...\n' >&2
    make "${make_args[@]}" >&2
    if [[ "$fresh" == 0 && "$update_disk" == 1 ]]; then
        node "$ROOT/c-test/examples/rOS/mkfat16.mjs" --update --backup -o "$disk" \
            "$OUT/IO.SYS:IO.SYS" "$OUT/KERNEL.SYS:KERNEL.SYS" \
            "$OUT/COMMAND.COM:COMMAND.COM" "$OUT/HELLO.RXE:HELLO.RXE" \
            "$OUT/SIERP.RXE:SIERP.RXE" "$OUT/BF.RXE:BF.RXE" \
            "$ROOT/c-test/examples/rOS/README.TXT:README.TXT" >&2
    fi
else
    disk="${disk:-$OUT/ros.img}"
fi

for required in "$ROOT/target/release/rvm" "$OUT/bios.bin" "$disk"; do
    if [[ ! -f "$required" ]]; then
        printf 'error: missing %s; run scripts/run-ros.sh without --run-only to build it\n' "$required" >&2
        exit 1
    fi
done
if [[ "$build_only" == 1 ]]; then exit 0; fi

printf 'Booting rOS; type HELP for commands or EXIT to stop.\n' >&2
run_args=("$OUT/bios.bin" --disk "$disk")
if [[ "$use_jit" == 1 ]]; then
    # Also allow an explicit --jit in options passed after the -- separator.
    if [[ ${#args[@]} -gt 0 ]]; then
        for arg in "${args[@]}"; do
            if [[ "$arg" == --jit ]]; then use_jit=0; break; fi
        done
    fi
    if [[ "$use_jit" == 1 ]]; then run_args+=(--jit); fi
fi
if [[ ${#args[@]} -gt 0 ]]; then run_args+=("${args[@]}"); fi
exec "$ROOT/target/release/rvm" "${run_args[@]}"
