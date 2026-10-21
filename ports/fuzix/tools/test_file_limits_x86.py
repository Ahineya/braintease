#!/usr/bin/env python3
"""Run the same capacity probe under a private freestanding x86 kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'rcc-portable/qbe'))
from test_x86 import file, run_guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fd-limits/x64')
    parser.add_argument('--program', type=Path, default=ROOT / 'build/fd-limits/fdcheck-x86.fxe')
    parser.add_argument('--old-program', type=Path)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out / 'file-limits-result.json').unlink(missing_ok=True)
    config = out / 'resolv.conf'; config.write_text('nameserver 192.168.64.1\n')
    files = [file('/bin/fdcheck', args.program), file('/etc/resolv.conf', config)]
    commands = [('fdcheck; echo FD_STATUS:$?', 'FD_LIMITS_OK\nFD_STATUS:0')]*2
    if args.old_program:
        files.append(file('/bin/old-sdk', args.old_program))
        commands.append(('old-sdk hello world; echo OLD_STATUS:$?',
                         'SDK_MAIN_OK hello world\nSDK_ATEXIT_OK\nOLD_STATUS:0'))
    _, checks = run_guest(out, 'file-limits', files, commands, 600)
    (out / 'file-limits-result.json').write_text(json.dumps(dict(
        status='passed', commands=checks, per_process=128, global_open_files=1024,
        kernel_sha256=hashlib.sha256((out/'normal/kernel.elf').read_bytes()).hexdigest(),
        program_sha256=hashlib.sha256(args.program.read_bytes()).hexdigest(),
        disk='disposable filesystem checked after clean shutdown'), indent=2) + '\n')


if __name__ == '__main__':
    main()
