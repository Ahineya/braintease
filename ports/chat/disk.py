#!/usr/bin/env python3
"""Transfer Fuse Chat sign-in files for a stopped FUZIX or Fuse-X guest.

Checks image locks and open handles before transfer. No tokens go to stdout.
"""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=('request', 'credentials'))
p.add_argument('image', type=Path)
p.add_argument('file', type=Path)
p.add_argument('--state', default='/.chat', help='guest chat directory (default root user HOME)')
a = p.parse_args()
if not a.image.is_file():
    sys.exit('Guest disk does not exist')
image = a.image.resolve()
fd = os.open(str(image) + '.lock', os.O_CREAT | os.O_RDWR, 0o600)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit('Guest disk is in use. Shut down the guest before transferring sign-in files.')
# Fuse-X locks the image itself; older RVM storage has no advisory lock.
# Retain both locks through the atomic replacement and check live open handles.
image_fd = os.open(image, os.O_RDWR)
try:
    fcntl.flock(image_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit('Guest disk is in use. Shut down the guest before transferring sign-in files.')
try:
    opened = subprocess.run(['lsof', '-t', '--', str(image)], capture_output=True, text=True)
    if opened.returncode not in (0, 1):
        sys.exit('Cannot verify that the guest disk is stopped')
    if any(int(pid) != os.getpid() for pid in opened.stdout.split()):
        sys.exit('Guest disk is in use. Shut down the guest before transferring sign-in files.')
except FileNotFoundError:
    # The C16 checker below also rejects dirty/mounted roots before any write.
    pass
os.umask(0o077)
sys.exit(subprocess.call(['node', str(Path(__file__).with_suffix('.mjs')), '--locked', a.action,
                         str(image), str(a.file.resolve()), a.state]))
