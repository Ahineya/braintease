#!/usr/bin/env python3
"""Build and run the isolated Riplix Brainfuck shell on a fresh disposable disk."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import shutil
import tempfile
import termios
import tty

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(prog='scripts/run-riplix-bf.sh',
        allow_abbrev=False, description=__doc__, epilog=(
        'Other options are passed to rbf. The default starts the tiny /init shell; '
        '--boot-check verifies PID 1, root mount and file reads, then halts. '
        'The maintained MMU/RVM build and its disks are untouched.'))
    parser.add_argument('--build', type=Path,
                        default=Path(os.environ.get('RIPLIX_BF_BUILD', os.environ.get('FUZIX_BF_BUILD', ROOT / 'build/fuzix-rbf'))))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build-only', action='store_true')
    mode.add_argument('--run-only', action='store_true')
    parser.add_argument('--boot-check', action='store_true', help='run the original boot-and-halt milestone')
    parser.add_argument('--no-jit', action='store_true', help='use the generic BF interpreter')
    telemetry = parser.add_mutually_exclusive_group()
    telemetry.add_argument('--telemetry', action='store_true', help='show lightweight BF progress')
    telemetry.add_argument('--no-telemetry', action='store_true', help=argparse.SUPPRESS)
    args, forwarded = parser.parse_known_args()
    if forwarded[:1] == ['--']:
        forwarded = forwarded[1:]
    if any(v == '--disk' or v.startswith('--disk=') for v in forwarded):
        parser.error('this boot profile uses its own tiny raw disk; --disk is not supported')
    if args.no_jit and '--jit' in forwarded:
        parser.error('--jit conflicts with --no-jit')
    build = args.build.resolve()
    if not args.run_only:
        build.mkdir(parents=True, exist_ok=True)
        # Serialize tool/kernel builds before truncating the shared log or
        # replacing objects. Two terminal launches may otherwise race Make.
        lock = (build / '.build.lock').open('a')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('Waiting for another Riplix/BF build...', file=sys.stderr, flush=True)
            fcntl.flock(lock, fcntl.LOCK_EX)
        env = os.environ.copy()
        xcode = '/Applications/Xcode.app/Contents/Developer'
        if 'DEVELOPER_DIR' not in env and Path(xcode).is_dir():
            env['DEVELOPER_DIR'] = xcode
        log = build / 'build.log'
        print(f'Building minimal Riplix/BF (log: {log})...', file=sys.stderr, flush=True)
        result = subprocess.run([
            sys.executable, str(ROOT / 'scripts/build_progress.py'), '--log', str(log), '--',
            sys.executable, str(Path(__file__).with_name('build.py')), '--build', str(build),
        ], cwd=ROOT, env=env)
        if result.returncode:
            return result.returncode
        lock.close()
    profile = build if args.boot_check else build / 'shell'
    required = [profile / 'kernel.bf', profile / 'root.img', ROOT / 'target/release/rbf']
    for artifact in required:
        if not artifact.is_file():
            parser.error(f'missing {artifact}; run without --run-only first')
    if args.build_only:
        print(f'Built {profile / "kernel.bf"}')
        return 0
    def supplied(option):
        return any(v == option or v.startswith(option + '=') for v in forwarded)

    defaults = []
    if not any(supplied(v) for v in ('--headless', '--visual', '--terminal')):
        defaults.append('--headless')
    if not supplied('--net'):
        defaults += ['--net', 'off']
    if not supplied('--exit-code'):
        defaults.append('--exit-code')
    if not args.no_jit and '--jit' not in forwarded:
        defaults.append('--jit')
    if args.telemetry and not supplied('--telemetry-lite'):
        defaults.append('--telemetry-lite')
    if not args.boot_check and not any(supplied(v) for v in (
            '--input', '--tty-stdin', '--control-stdin', '--terminal', '--visual', '--headless')):
        defaults.append('--tty-stdin')
    if args.boot_check and not any(v == '--timeout' or v.startswith('--timeout=') for v in forwarded):
        defaults += ['--timeout', '60']
    if not any(v == '--report' or v.startswith('--report=') for v in forwarded):
        defaults += ['--report', str(profile / ('boot.json' if args.boot_check else 'shell.json'))]
    vm = ROOT / 'target/release/rbf'
    command = [str(vm), str(profile / 'kernel.bf'), '--disk', str(profile / 'root.img'),
               *defaults, *forwarded]
    if args.boot_check:
        os.execv(vm, command)
    # Each launch gets an independent copy, including --run-only. Neither a
    # crash nor unsynced shutdown affects the template or the next session.
    with tempfile.TemporaryDirectory(prefix='session-', dir=profile) as directory:
        disk = Path(directory) / 'root.img'
        shutil.copyfile(profile / 'root.img', disk)
        command[command.index('--disk') + 1] = str(disk)
        if '--tty-stdin' in command and sys.stdin.isatty():
            fd = sys.stdin.fileno()
            previous = termios.tcgetattr(fd)
            try:
                tty.setcbreak(fd)
                return subprocess.run(command).returncode
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, previous)
        return subprocess.run(command).returncode



if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
