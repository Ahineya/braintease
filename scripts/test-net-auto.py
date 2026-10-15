#!/usr/bin/env python3
"""Real macOS host ping with runtime-owned networking; no launcher or socket setting.

Build the monitor for RVM/native first. Only the embedded vmnet helper uses sudo.
Native execution uses a temporary copy outside the repository to check bundling.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def run_case(args, mode):
    build = args.build_root.resolve() / mode
    log = build / 'auto-guest.log'
    result = {'success': False, 'mode': mode, 'jit': args.jit and mode == 'rvm'}
    started = time.monotonic()
    guest = None
    try:
        with tempfile.TemporaryDirectory(prefix='ripple-auto-', dir='/tmp') as temporary, log.open('w') as output:
            env = os.environ.copy()
            env.pop('RCC_NATIVE_NET', None)
            env.pop('RIPPLE_NET', None)
            if mode == 'native':
                executable = Path(temporary) / 'monitor'
                shutil.copy2(build / 'monitor', executable)
                command = [str(executable)]
                env.update(RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
            else:
                bank = int((build / 'bank-size').read_text())
                command = [str(ROOT / 'target/release/rvm'), str(build / 'monitor.bin'),
                           '--bank-size', str(bank), '--memory', str(bank * 8), '--headless',
                           '--timeout', str(args.timeout), '--disk', str(build / 'auto.disk.img')]
                if args.jit:
                    command.append('--jit')
            result['guest_command'] = command
            guest = subprocess.Popen(command, cwd=temporary, env=env, stdout=output,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            directory = Path(f'/tmp/ripple-vmnet-{guest.pid}-0')
            try:
                while 'Waiting for pings;' not in log.read_text():
                    if guest.poll() is not None:
                        raise RuntimeError('guest stopped before network readiness: ' + log.read_text())
                    if time.monotonic() - started > args.timeout:
                        raise RuntimeError('automatic network readiness timeout: ' + log.read_text())
                    time.sleep(0.05)
                assert directory.is_dir(), 'runtime did not own an embedded helper directory'
                ping = subprocess.run(['/sbin/ping', '-n', '-c', str(args.count), '-W', '3000', args.guest_ip],
                                      capture_output=True, text=True, timeout=args.timeout)
                (build / 'auto-host-ping.log').write_text(ping.stdout + ping.stderr)
                if ping.returncode or f'{args.count} packets received' not in ping.stdout:
                    raise RuntimeError('host ping failed: ' + ping.stdout + ping.stderr)
                deadline = time.monotonic() + 5
                while log.read_text().count('PING reply queued') < args.count:
                    if guest.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('missing guest reply logs: ' + log.read_text())
                    time.sleep(0.05)
                assert log.read_text().count('PING request from=') == args.count, log.read_text()
                assert guest.poll() is None, 'monitor did not stay running'
            finally:
                if guest.poll() is None:
                    # Deliberately signal the process alone: automatic cleanup
                    # must survive loss of Rust destructors, not just terminal signals.
                    guest.terminate()
                    try:
                        guest.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        guest.kill()
                        guest.wait()
                deadline = time.monotonic() + 9
                while directory.exists() and time.monotonic() < deadline:
                    time.sleep(0.1)
                if directory.exists():
                    raise RuntimeError(f'owned network directory remains after guest exit: {directory}')
            result.update(success=True, replies=args.count, cleanup=True,
                          native_copy_outside_repository=mode == 'native')
            print(f'{mode}: automatic NIC startup, {args.count}/{args.count} real host replies, '
                  'guest logs and abrupt-exit helper cleanup verified')
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        result['elapsed_seconds'] = time.monotonic() - started
        (build / 'auto-results.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['rvm', 'native'], action='append')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/monitor')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--count', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=40)
    parser.add_argument('--guest-ip', type=lambda value: str(ipaddress.IPv4Address(value)), default='192.168.64.250')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('automatic live attachment currently requires macOS')
    if args.timeout <= 0 or not 1 <= args.count <= 10:
        parser.error('timeout must be positive; count must be 1..10')
    for mode in args.mode or ['rvm', 'native']:
        run_case(args, mode)


if __name__ == '__main__':
    main()
