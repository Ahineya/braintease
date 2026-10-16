#!/usr/bin/env python3
"""Hold an exclusive image lock across image updates and VM execution."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--disk', required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--fresh', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('vm_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    image = Path(args.disk).resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    lock = os.open(str(image) + '.lock', os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(f'Disk is already in use: {image}')
    if args.update:
        if image.exists() and not args.fresh:
            # The caller already owns the image lock. The checker validates
            # all ownership and directory links before repairing metadata.
            checked = subprocess.run(
                ['node', str(Path(__file__).with_name('fsck.mjs')),
                 '--locked', str(image), '--repair'],
                capture_output=True, text=True)
            if checked.returncode:
                sys.stderr.write(checked.stderr)
                sys.exit(checked.returncode)
            if '; repaired' in checked.stdout:
                print('Recovered the filesystem; a backup was saved.', flush=True)
        cmd = ['node', str(Path(__file__).with_name('disk.mjs')), args.build, str(image), '--install']
        if args.fresh:
            cmd.append('--fresh')
        updated = subprocess.run(cmd)
        if updated.returncode:
            sys.exit(updated.returncode)
    if args.build_only:
        return
    vm_args = args.vm_args[1:] if args.vm_args[:1] == ['--'] else args.vm_args
    if '--guest-ctrl-c' not in vm_args:
        vm_args = ['--guest-ctrl-c', *vm_args]
    if not any(v in ("--memory", "-m") or v.startswith("--memory=") for v in vm_args):
        vm_args = ["--memory", "4608000", *vm_args]
    os.set_inheritable(lock, True)
    os.execv(args.vm, [args.vm, str(Path(args.build) / 'bios.bin'), '--disk', str(image), *vm_args])


if __name__ == '__main__':
    main()
