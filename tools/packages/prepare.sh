#!/bin/sh
# Assemble independently installable packages from already-built programs.
# Format creation is always performed by the shared C engine.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
engine=$root/build/pkg/host/pkg-engine
out=$root/build/pkg/packages
mkdir -p "$out"
[ -x "$engine" ] || sh "$root/ports/pkg/build.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for target in fuse-x-arm64-macos-c16 riplix-x64-c16; do
    stage=$work/$target
    mkdir -p "$stage/usr/bin"
    if [ "$target" = fuse-x-arm64-macos-c16 ]; then
        mkdir -p "$stage/usr/libexec"
        cp "$root/build/pkg/pkg-engine.fxe" "$stage/usr/libexec/pkg-engine"
        cp "$root/build/pkg/pkg.fxe" "$stage/usr/bin/pkg"
        name=pkg-fuse-x
    else
        cp "$root/build/pkg/pkg-engine-x64.rxe" "$stage/usr/bin/pkg"
        name=pkg-x64
    fi
    chmod 755 "$stage/usr/bin/"*
    printf 'name=pkg\nversion=0.2.0\ntarget=%s\ndepends=\n' "$target" > "$work/manifest"
    "$engine" create "$work/manifest" "$stage" "$out/$name.rpk"
    "$engine" inspect "$out/$name.rpk"
done
mkdir -p "$work/hello/usr/bin"
printf '#!/bin/sh\necho Hello from the Riplix package repository!\n' > "$work/hello/usr/bin/pkghello"
chmod 755 "$work/hello/usr/bin/pkghello"
printf 'name=pkghello\nversion=1\ntarget=any\ndepends=\n' > "$work/manifest"
"$engine" create "$work/manifest" "$work/hello" "$out/pkghello.rpk"
printf 'Packages ready in %s\n' "$out"
