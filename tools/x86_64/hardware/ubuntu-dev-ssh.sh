#!/usr/bin/env bash
# Ubuntu-only development listener requested by the machine owner.
# Retains temporary-root-ssh.sh's passwordless root authentication on port2222.
set -Eeuo pipefail
(( EUID == 0 )) || { echo 'Run this listener as root.' >&2; exit 1; }
port=${1:-2222}
[[ "$port" =~ ^[0-9]+$ ]] && (( port >= 1024 && port <= 65535 )) || exit 1
exec 9>/run/temporary-root-ssh.lock
flock -n 9 || { echo 'Another temporary root listener is running.' >&2; exit 1; }
mkdir -p /run/sshd
options=(-f /dev/null -p "$port"
    -o HostKey=/etc/ssh/ssh_host_ed25519_key
    -o UsePAM=no -o PermitRootLogin=yes
    -o PasswordAuthentication=yes -o PermitEmptyPasswords=yes
    -o AuthenticationMethods=none -o AllowUsers=root -o PidFile=none)
/usr/sbin/sshd -t "${options[@]}"
state=/var/lib/riplix-dev-ssh
install -d -m 0700 "$state"
# Keep the original setting on the HP across an interrupted boot. Never log it.
saved="$state/root-password.original"
if [[ ! -e "$saved" ]]; then
    original_hash=$(getent shadow root | cut -d: -f2)
    printf '%s\n' "$original_hash" | /usr/bin/python3 -I -c '
import os, pathlib, sys, tempfile
p = pathlib.Path(sys.argv[1]); data = sys.stdin.buffer.read(4097)
if len(data) > 4096 or data.count(b"\n") != 1 or not data.endswith(b"\n") or b"\0" in data:
    raise SystemExit("Invalid original root setting")
fd, name = tempfile.mkstemp(prefix=".root-password-", dir=p.parent)
try:
    with os.fdopen(fd, "wb") as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
    os.link(name, p)  # Atomic publication without replacing an existing baseline.
    os.unlink(name)
    directory = os.open(p.parent, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(directory)
    finally: os.close(directory)
finally:
    if os.path.exists(name): os.unlink(name)
' "$saved"
fi
[[ -f "$saved" && ! -L "$saved" && $(stat -c %u "$saved") == 0 && $(stat -c %a "$saved") == 600 ]] || exit 1
original_hash=$(<"$saved")
server_pid=''
cleanup() {
    local result=$?
    trap - EXIT INT TERM HUP
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if usermod -p "$original_hash" root && sync -f /etc/shadow; then
        rm "$saved"
        sync -f "$state"
        echo 'Development listener stopped; original root password setting restored.'
    else
        echo 'Failed to restore the original root password setting.' >&2
        result=1
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
passwd -d root
echo "Development root SSH listener starting on port $port."
/usr/sbin/sshd -D -e "${options[@]}" 9>&- &
server_pid=$!
wait "$server_pid"
