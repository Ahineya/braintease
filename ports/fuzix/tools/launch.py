#!/usr/bin/env python3
"""Hold an exclusive image lock across image updates and VM execution."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import termios
import tty


def run_tty(command, lock):
    """Bridge the host terminal to RBF's byte stream, retaining guest controls."""
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    child = None
    handlers = {}

    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    try:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            handlers[signum] = signal.signal(signum, interrupted)
        tty.setraw(fd)
        settings = termios.tcgetattr(fd)
        # Retain host newline rendering; guest input must remain byte-for-byte.
        settings[1] = saved[1]
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        child = subprocess.Popen(command, stdin=subprocess.PIPE, start_new_session=True,
                                 pass_fds=(lock,))
        while child.poll() is None:
            if not select.select([fd], [], [], 0.05)[0]:
                continue
            data = os.read(fd, 4096)
            if not data:
                child.stdin.close()
                return child.wait()
            before, stop, _after = data.partition(b'\x1d')
            if before:
                try:
                    child.stdin.write(before)
                    child.stdin.flush()
                except BrokenPipeError:
                    return child.wait()
            if stop:
                child.terminate()
                return child.wait(timeout=5)
        return child.returncode
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, saved)
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for signum, handler in handlers.items():
            signal.signal(signum, handler)


def host_command(args, image):
    vm_args = args.vm_args[1:] if args.vm_args[:1] == ['--'] else args.vm_args
    interactive = False
    if args.backend == 'rvm':
        program = 'bios.bin'
        if '--guest-ctrl-c' not in vm_args:
            vm_args = ['--guest-ctrl-c', *vm_args]
        if not any(v in ('--memory', '-m') or v.startswith('--memory=') for v in vm_args):
            vm_args = ['--memory', '5120000', *vm_args]
    else:
        program = 'bios.bf'
        # The export declares both RAM and code reservation. Do not impose RBF's
        # smaller generic default, or pass RVM-specific --memory/--guest-ctrl-c.
        if not any(v == '--max-tape-cells' or v.startswith('--max-tape-cells=') for v in vm_args):
            with open(Path(args.build) / program) as source:
                header = source.readline()
            match = re.search(r'\bTAPE_CELLS (\d+) END_BFHOST', header)
            if not match:
                sys.exit('Missing BFHOST tape declaration in bios.bf; rebuild it first')
            vm_args = ['--max-tape-cells', match[1], *vm_args]
        explicit_input = any(v.split('=', 1)[0] in (
            '--headless', '--visual', '--terminal', '--tty-stdin', '--control-stdin',
            '--input-script', '--screenshot', '--raw-frame', '--text-frame') for v in vm_args)
        interactive = sys.stdin.isatty() and sys.stdout.isatty() and not explicit_input
        if interactive:
            vm_args = ['--tty-stdin', *vm_args]
    return [args.vm, str(Path(args.build) / program), '--disk', str(image), *vm_args], interactive


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--disk', required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--backend', choices=('rvm', 'rbf'), default='rvm')
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
    command, interactive = host_command(args, image)
    if interactive:
        # Both processes hold the lock, including if the terminal parent dies.
        status = run_tty(command, lock)
        sys.exit(status if status >= 0 else 128 - status)
    os.set_inheritable(lock, True)
    os.execv(args.vm, command)


if __name__ == '__main__':
    main()
