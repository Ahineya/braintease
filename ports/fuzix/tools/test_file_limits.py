#!/usr/bin/env python3
"""File/stdio/select capacity on disposable kernels; never installs an image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'rvm'], default='native')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--build', type=Path, help='private seed build with matching kernel/apps/libraries (required for RVM)')
    parser.add_argument('--program', required=True, type=Path)
    parser.add_argument('--libraries', type=Path)
    parser.add_argument('--select-program', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fd-limits')
    args = parser.parse_args()
    if args.target == 'native' and not args.kernel: parser.error('--kernel is required for native')
    if args.target == 'rvm' and not args.build: parser.error('--build is required for RVM')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    (output / 'result.json').unlink(missing_ok=True)
    if args.target == 'native': os.environ['FUSE_X_TEST_KERNEL'] = str(args.kernel.resolve())
    files = [dict(name='/lib/fuse-x/' + p.name, path=str(p.resolve()))
             for p in args.libraries.glob('*.fxl.[0-9]')] if args.libraries else []
    files += [dict(name='/etc/passwd', text='root:x:0:0:root:/root:/bin/sh\n'),
              dict(name='/dev/ptyp0', mode=0o20600, device=0x300),
              dict(name='/dev/ttyp0', mode=0o20600, device=0x202)]
    apps = {'fdcheck': args.program}
    if args.select_program: apps['selectcheck'] = args.select_program
    with tempfile.TemporaryDirectory(prefix='file-limits-', dir='/tmp') as directory:
        guest = Guest(directory, args.target, apps, files=files, build_override=args.build)
        try:
            if args.select_program:
                text = guest.shell('selectcheck; echo SELECT_STATUS:$?', 180)
                print(text, flush=True)
                assert 'SELECT_OK' in text and 'SELECT_STATUS:0' in text, text
            for _ in range(2):
                text = guest.shell('fdcheck; echo FD_STATUS:$?', 180)
                print(text, flush=True)
                assert 'FD_LIMITS_OK' in text and 'FD_STATUS:0' in text, text
            assert 'FD_ALIVE' in guest.shell('echo FD_ALIVE')
        finally:
            (output / 'guest.log').write_bytes(guest.log)
            try:
                guest.close()
            finally:
                (output / 'guest.log').write_bytes(guest.log)
    (output / 'result.json').write_text(json.dumps(dict(status='passed',
        per_process=128, global_open_files=1024, launches=2,
        kernel_sha256=hashlib.sha256((args.kernel if args.target == 'native'
                                    else args.build/'FUZIX.SYS').read_bytes()).hexdigest(),
        program_sha256=hashlib.sha256(args.program.read_bytes()).hexdigest(),
        select_regression=bool(args.select_program),
        coverage=['dynamic streams', 'freopen', 'descriptor exhaustion/recovery',
                  'high pipe/socket select', 'legacy select copy boundaries',
                  'blocked high-FD wakeup', 'flush failure and exit flushing',
                  'fork/exec descriptor flags', '20 concurrent popen streams',
                  'global file-table exhaustion/recovery', 'clean filesystem']), indent=2) + '\n')


if __name__ == '__main__':
    main()
