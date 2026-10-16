#!/usr/bin/env python3
"""Check or repair a stopped guest's filesystem, with an exclusive image lock."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image', type=Path)
p.add_argument('--repair', action='store_true', help='rebuild free lists and mark clean after structural validation; backs up the disk first')
a = p.parse_args()
if not a.image.is_file():
    sys.exit(f'Image not found: {a.image}')
fd = os.open(str(a.image.resolve())+'.lock', os.O_CREAT | os.O_RDWR, 0o600)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit('Disk is in use; shut down the VM before checking it')
cmd = ['node', str(Path(__file__).with_suffix('.mjs')), '--locked', str(a.image)]
if a.repair:
    cmd.append('--repair')
sys.exit(subprocess.call(cmd))
