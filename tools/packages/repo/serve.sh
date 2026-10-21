#!/bin/sh
set -eu
# launchd's diagnostic file is separate from Caddy's rotating logs.
log=/Users/ahineya/.local/share/riplix-repo/logs/launchd.log
if [ -f "$log" ] && [ "$(wc -c < "$log")" -gt 1048576 ]; then
    cp "$log" "$log.1"
    : > "$log"
fi
volume=/Volumes/Ahineya
expected=E3129F95-3ACD-4BD1-B261-35D449BB4A1D
actual=$(/usr/sbin/diskutil info -plist "$volume" | /usr/bin/plutil -extract VolumeUUID raw -)
if [ "$actual" != "$expected" ] || [ ! -r "$volume/RiplixRepo/public/health.txt" ]; then
    echo 'Riplix repository: expected SSD unavailable or unreadable' >&2
    exit 78
fi
exec /opt/homebrew/bin/caddy run --config /Users/ahineya/.local/share/riplix-repo/Caddyfile --adapter caddyfile
