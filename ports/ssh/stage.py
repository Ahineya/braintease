#!/usr/bin/env python3
"""Explicitly stage SSH tools for manual testing; never install or enable them."""
import argparse
from pathlib import Path
import os
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def stage(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if not source.is_file():
        raise SystemExit(f'build the SSH executable first: {source}')
    if source == destination:
        raise SystemExit('source and destination must differ')
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + destination.name + '.', dir=destination.parent)
    try:
        with os.fdopen(fd, 'wb') as output, source.open('rb') as incoming:
            shutil.copyfileobj(incoming, output)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o755)
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Staged {destination}; not installed or enabled')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--all', action='store_true', help='stage server, keygen and clients')
    args = parser.parse_args()
    if args.all:
        if args.source or args.destination:
            parser.error('--all cannot use --source or --destination')
        for name in ('sshd', 'ssh-keygen', 'ssh', 'sftp', 'scp', 'sftp-mount'):
            stage(ROOT / f'build/ssh/{name}.fxe', ROOT / f'fuse-x-shared/ssh/{name}.fxe')
    else:
        stage(args.source or ROOT / 'build/ssh/sshd.fxe',
              args.destination or ROOT / 'fuse-x-shared/ssh/sshd.fxe')


if __name__ == '__main__':
    main()
