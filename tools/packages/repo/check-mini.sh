#!/bin/sh
# Check the real LAN service with the trusted key and a disposable host root.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
url=${1:-http://mini.local:8088}
engine=$root/build/pkg/host/pkg-engine
work=$(mktemp -d)
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/etc/pkg"
cp "$here/mini.pub" "$work/etc/pkg/repo.pub"
printf 'url=%s\n' "$url" > "$work/etc/pkg/repo.conf"
"$engine" --root "$work" update
"$engine" --root "$work" add pkghello
"$work/usr/bin/pkghello"
"$engine" --root "$work" remove pkghello
"$engine" --root "$work" clean
