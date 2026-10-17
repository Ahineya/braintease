#!/usr/bin/env python3
"""Install a built Lynx and its defaults into a stopped native or RVM guest disk."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess

from probe import PORT, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], required=True)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--executable', type=Path)
    args = parser.parse_args()
    image = args.disk.resolve(strict=True)
    executable = (args.executable or PORT / ('build/lynx.fxe' if args.target == 'native' else 'build/lynx.rxe')).resolve(strict=True)
    with executable.open('rb') as source:
        magic = source.read(8)
    if not magic.startswith(b'FUSEX001' if args.target == 'native' else b'RXE1'):
        parser.error('executable does not match the selected target')
    # RVM launchers lock the sidecar; native execution locks the image itself.
    # Hold both across the read/validate/backup/atomic-replace transaction.
    locks = []
    try:
        for path in (Path(str(image) + '.lock'), image):
            fd = os.open(path, os.O_RDWR | (os.O_CREAT if path != image else 0), 0o600)
            locks.append(fd)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise SystemExit(f'Disk is in use; stop its guest before installing: {image}')
        script = PORT / 'install.mjs'
        subprocess.run(['node', str(script), args.target, str(image), str(executable)], cwd=ROOT, check=True)
    finally:
        for fd in reversed(locks): os.close(fd)


if __name__ == '__main__': main()
