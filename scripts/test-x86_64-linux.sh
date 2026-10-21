#!/usr/bin/env bash
# Compile and run real x86_64 Linux code, including on an ARM development Mac.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    echo 'Usage: scripts/test-x86_64-linux.sh [OUTPUT_DIRECTORY]'
    echo 'Requires Docker. Builds isolated tools and runs disposable hosted Riplix checks.'
    exit 0
fi
[[ $# -le 1 ]] || { echo 'Expected at most one output directory' >&2; exit 2; }
command -v docker >/dev/null || { echo 'Docker is required' >&2; exit 1; }
docker info >/dev/null
OUT="${1:-$ROOT/build/x86_64-linux}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
IMAGE=braintease-x86_64-linux:rust-1.97.1
docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/tools/x86_64/linux"
# Source and developer disks are read-only. A separate target volume avoids
# mixing ELF objects with the Mac's existing Mach-O build products.
docker run --rm --platform linux/amd64 \
    --mount "type=bind,source=$ROOT,target=/repo,readonly" \
    --mount "type=bind,source=$OUT,target=/out" \
    --mount type=volume,source=braintease-x86_64-target,target=/repo/target \
    --mount type=volume,source=braintease-x86_64-cargo,target=/usr/local/cargo/registry \
    "$IMAGE" 2>&1 | tee "$OUT/validation.log"
