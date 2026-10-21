#!/bin/sh
# Dedicated user LaunchAgent. Never alter other services or SSD permissions.
set -eu
host=${1:-mini.local}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "$host" in -*|*[!a-zA-Z0-9.@_-]*) echo 'Invalid SSH host' >&2; exit 2;; esac
ssh -o BatchMode=yes "$host" /bin/sh -s <<'REMOTE'
set -eu
volume=/Volumes/Ahineya
expected=E3129F95-3ACD-4BD1-B261-35D449BB4A1D
actual=$(/usr/sbin/diskutil info -plist "$volume" | /usr/bin/plutil -extract VolumeUUID raw -)
[ "$actual" = "$expected" ] || { echo 'Wrong or absent SSD; refusing setup' >&2; exit 1; }
/bin/ls -A "$volume" >/dev/null
[ "$(id -un)" = ahineya ] || { echo 'Expected repository account ahineya' >&2; exit 1; }
[ -x /opt/homebrew/bin/caddy ]
/bin/launchctl print gui/501 >/dev/null
mkdir -p "$volume/RiplixRepo/public" "$volume/RiplixRepo/private"
chmod 700 "$volume/RiplixRepo/private"
mkdir -p "$HOME/.local/share/riplix-repo/logs" "$HOME/Library/LaunchAgents"
printf 'Riplix package repository\n' > "$volume/RiplixRepo/public/health.txt"
REMOTE
scp "$here/Caddyfile" "$here/serve.sh" "$host:.local/share/riplix-repo/"
scp "$here/org.riplix.packages.plist" "$host:Library/LaunchAgents/org.riplix.packages.plist"
ssh -o BatchMode=yes "$host" /bin/sh -s <<'REMOTE'
set -eu
base=$HOME/.local/share/riplix-repo
/opt/homebrew/bin/caddy validate --config "$base/Caddyfile" --adapter caddyfile
/usr/bin/plutil -lint "$HOME/Library/LaunchAgents/org.riplix.packages.plist"
if /bin/launchctl print gui/501/org.riplix.packages >/dev/null 2>&1; then
    /bin/launchctl bootout gui/501/org.riplix.packages
fi
/bin/launchctl bootstrap gui/501 "$HOME/Library/LaunchAgents/org.riplix.packages.plist"
/bin/launchctl kickstart gui/501/org.riplix.packages
REMOTE
