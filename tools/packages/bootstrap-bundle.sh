#!/bin/sh
# Prepare the small target-specific bootstrap folders in the existing host share.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=${1:-$root/fuse-x-shared/packages/bootstrap}
for target in fuse-x x64; do
    directory=$out/$target
    mkdir -p "$directory"
    if [ "$target" = fuse-x ]; then source=$root/build/pkg/pkg-engine.fxe
    else source=$root/build/pkg/pkg-engine-x64.rxe; fi
    cp "$source" "$directory/pkg-engine"
    cp "$here/repo/mini.pub" "$directory/repo.pub"
    cp "$here/bootstrap.sh" "$directory/install.sh"
    chmod 755 "$directory/pkg-engine"
    chmod 644 "$directory/repo.pub" "$directory/install.sh"
done
printf 'Bootstrap bundles ready in %s\n' "$out"
