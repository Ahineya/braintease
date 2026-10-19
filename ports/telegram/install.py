#!/usr/bin/env python3
"""Copy Telegram and a saved login from a stopped private guest into another image."""
import argparse
from contextlib import ExitStack
import fcntl
import os
from pathlib import Path
import subprocess

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-image', type=Path, required=True, help='stopped native private guest image')
    parser.add_argument('--target', choices=['native', 'ripple'], required=True)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--executable', type=Path)
    args = parser.parse_args()
    source, disk = args.source_image.resolve(strict=True), args.disk.resolve(strict=True)
    if os.path.samefile(source, disk):
        parser.error('source and destination must be different images')
    executable = (args.executable or ROOT / ('fuse-x/build/apps/telegram.fxe' if args.target == 'native'
                                            else 'build/fuzix/telegram.rxe')).resolve(strict=True)
    with executable.open('rb') as stream:
        if not stream.read(8).startswith(b'FUSEX001' if args.target == 'native' else b'RXE1'):
            parser.error('executable does not match the selected target')
    if source.stat().st_mode & 0o077:
        parser.error('source image must be private (mode 0600)')
    # Native hosts lock the image; RVM launchers lock its sidecar. Hold both
    # conventions for both images across validation, backup and replacement.
    with ExitStack() as stack:
        for image in sorted([source, disk]):
            for path in (Path(str(image) + '.lock'), image):
                stream = stack.enter_context(path.open('a+b' if path != image else 'r+b'))
                if path != image:
                    os.chmod(path, 0o600)
                try:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    raise SystemExit(f'Disk is in use; shut down its guest first: {image}') from None
        subprocess.run(['node', str(PORT / 'install.mjs'), args.target, str(source), str(disk),
                        str(executable)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
