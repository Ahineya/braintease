"""Install only compiler files on a stopped disk; preserve kernel and user files."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess


def install(image, osname, package):
    if not image.is_file():raise RuntimeError(f'Disk not found: {image}')
    with Path(str(image)+'.lock').open('a') as lock:
        try:fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:raise RuntimeError('Disk is in use; shut down the guest before installing')
        if shutil.which('lsof'):
            opened=subprocess.run(['lsof', '-t', '--', str(image)], capture_output=True)
            if opened.stdout.strip():raise RuntimeError('Disk is open; shut down the guest before installing')
        with image.open('rb') as actual:
            try:fcntl.flock(actual, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:raise RuntimeError('Disk is locked by a running guest; shut it down before installing')
            subprocess.run(['node', str(Path(__file__).with_suffix('.mjs')), osname, str(image), str(package)], check=True)
