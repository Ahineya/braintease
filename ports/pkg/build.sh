#!/bin/sh
# Build the shared portable engine as a conventional host executable.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=${1:-$root/build/pkg/host/pkg-engine}
mkdir -p "$(dirname -- "$out")"
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -O2 -Wall -Wextra \
    ${SDKROOT:+-isysroot "$SDKROOT"} \
    "$here/main.c" "$here/sha256.c" "$here/format.c" "$here/store.c" "$here/repo.c" "$here/network.c" "$here/verify.c" -o "$out"
printf 'Built %s\n' "$out"
