#!/bin/sh
# Publish local .rpk files using SSH. Signing keys never leave the SSD.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
host=${PKG_REPO_SSH:-mini.local}
case "$host" in -*|*[!a-zA-Z0-9.@_-]*) echo 'Invalid SSH host' >&2; exit 2;; esac
[ "$#" -gt 0 ] || { echo 'Usage: sh tools/packages/repo/publish.sh FILE.rpk ...' >&2; exit 2; }
# A conventional host executable also runs on the mini's older macOS.
MACOSX_DEPLOYMENT_TARGET=13.0 sh "$root/ports/pkg/build.sh"
engine=$root/build/pkg/host/pkg-engine
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
n=0
for package in "$@"; do
    "$engine" inspect "$package"
    cp "$package" "$work/$n.rpk"; n=$((n+1))
done
incoming=$(ssh -o BatchMode=yes "$host" /bin/sh -s <<'REMOTE'
set -eu
actual=$(/usr/sbin/diskutil info -plist /Volumes/Ahineya | /usr/bin/plutil -extract VolumeUUID raw -)
[ "$actual" = E3129F95-3ACD-4BD1-B261-35D449BB4A1D ]
mktemp -d /Volumes/Ahineya/RiplixRepo/private/incoming.XXXXXX
REMOTE
)
case "$incoming" in /Volumes/Ahineya/RiplixRepo/private/incoming.*) ;; *) echo 'Invalid remote staging path' >&2; exit 1;; esac
scp "$engine" "$here/publish-remote.sh" "$host:.local/share/riplix-repo/"
scp "$work/"*.rpk "$host:$incoming/"
ssh -o BatchMode=yes "$host" /bin/sh .local/share/riplix-repo/publish-remote.sh "$incoming"
# Pin through authenticated SSH, rather than downloading a trust key over HTTP.
scp "$host:/Volumes/Ahineya/RiplixRepo/public/repo.pub" "$root/tools/packages/repo/mini.pub"
printf 'Repository: http://mini.local:8088/ (guest LAN IP: 192.168.100.179)\n'
