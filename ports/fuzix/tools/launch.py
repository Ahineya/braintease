#!/usr/bin/env python3
"""Hold an exclusive image lock across image updates and VM execution."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys


def require_mmu(build):
    """Reject obsolete artifacts before opening or updating any user disk."""
    profile = Path(build) / '.mmu'
    if not profile.is_file() or profile.read_text().strip() != '1':
        raise SystemExit('Non-MMU or unmarked FUZIX artifacts are unsupported; '
                         'rebuild for RVM without --run-only (MMU=0 is retired).')


def require_backend(backend):
    if backend != 'rvm':
        raise SystemExit('Non-MMU FUZIX/RBF is retired; use RVM (--backend rvm).')


def host_command(args, image):
    require_backend(args.backend)
    require_mmu(args.build)
    vm_args = args.vm_args[1:] if args.vm_args[:1] == ['--'] else args.vm_args
    network = None
    for i, option in enumerate(vm_args):
        if option == '--net' and i + 1 < len(vm_args):
            network = vm_args[i + 1]
        elif option.startswith('--net='):
            network = option.split('=', 1)[1]
    if network is None:
        network = os.environ.get('RIPPLE_NET') or 'shared'
        vm_args = ['--net', network, *vm_args]
    if network in ('host', 'shared', 'auto') and '--net-eager' not in vm_args:
        vm_args = ['--net-eager', *vm_args]
    if not any(v == '--audio' or v.startswith('--audio=') for v in vm_args):
        # Unix playback follows host monotonic time rather than NEXT_TICK.
        # Keep automated/headless runs muted unless explicitly enabled.
        vm_args = ['--audio', 'auto' if '--headless' in vm_args else 'on', *vm_args]
    if '--visual' in vm_args and '--guest-window-close' not in vm_args:
        vm_args = ['--guest-window-close', *vm_args]
    if '--guest-ctrl-c' not in vm_args:
        vm_args = ['--guest-ctrl-c', *vm_args]
    if not any(v in ('--memory', '-m') or v.startswith('--memory=') for v in vm_args):
        vm_args = ['--memory', '16384000', *vm_args]
    return [args.vm, str(Path(args.build) / 'bios.bin'), '--disk', str(image), *vm_args]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--disk', required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--backend', default='rvm', help='RVM only; the non-MMU RBF port is retired')
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--fresh', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('vm_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    require_backend(args.backend)
    require_mmu(args.build)
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
            # all ownership and directory links before recovering abandoned
            # zero-link pipes and repairing shutdown/free-list metadata.
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
    if not args.update:
        checked = subprocess.run(['node', str(Path(__file__).with_name('check-abi.mjs')), str(image)])
        if checked.returncode:
            sys.exit(checked.returncode)
    if args.build_only:
        return
    command = host_command(args, image)
    os.set_inheritable(lock, True)
    os.execv(args.vm, command)


if __name__ == '__main__':
    main()
