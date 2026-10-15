#!/usr/bin/env python3
"""Bounded bidirectional ICMP test against the actual host OS, using a running helper."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True, type=Path)
    parser.add_argument('--mode', choices=['rvm', 'rbf', 'native'], default='rvm')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--guest-ip', default='192.168.64.250')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--jit', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0 or (args.jit and args.mode == 'native'):
        parser.error('timeout must be positive; --jit applies to RVM/RBF only')
    build = (args.build or ROOT / 'build/network' / args.mode).resolve()
    env = os.environ.copy()
    if args.mode == 'native':
        command = [str(build / 'ping')]
        env.update(RCC_NATIVE_NET=str(args.socket), RCC_NATIVE_TIMEOUT=str(args.timeout), RCC_NATIVE_HEADLESS='1')
    else:
        bank_file = build / 'bank-size'
        bank = bank_file.read_text().strip()
        command = [str(ROOT / 'target/release' / args.mode), str(build / ('ping.bf' if args.mode == 'rbf' else 'ping.bin')),
                   '--net', str(args.socket), '--headless', '--timeout', str(args.timeout), '--exit-code',
                   '--report', str(build / 'live-guest.json')]
        if args.mode == 'rvm':
            command += ['--bank-size', bank, '--memory', str(int(bank) * 8), '--disk', str(build / 'live.disk.img')]
        if args.jit:
            command += ['--jit']
    log = build / 'live-guest.log'
    host_log = build / 'live-host-ping.log'
    started = time.monotonic()
    report = {'success': False, 'mode': args.mode, 'jit': args.jit, 'guest_command': command,
              'guest_ip': args.guest_ip, 'timeout_seconds': args.timeout}
    guest = host = None
    try:
        with log.open('w') as output, host_log.open('w') as host_output:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT)
            while 'PING ready ' not in log.read_text():
                if guest.poll() is not None:
                    raise RuntimeError('guest stopped before NIC readiness: ' + log.read_text())
                if time.monotonic() - started > args.timeout:
                    raise RuntimeError('guest readiness timeout')
                time.sleep(0.05)
            ping = ['/sbin/ping' if platform.system() == 'Darwin' else 'ping', '-n', '-c', '1', '-s', '17',
                    '-W', str(args.timeout * 1000 if platform.system() == 'Darwin' else args.timeout), args.guest_ip]
            report['host_command'] = ping
            host = subprocess.Popen(ping, stdout=host_output, stderr=subprocess.STDOUT)
            while guest.poll() is None or host.poll() is None:
                if time.monotonic() - started > args.timeout:
                    raise RuntimeError('bidirectional ping timeout')
                time.sleep(0.05)
            assert guest.returncode == 0, 'guest failed: ' + log.read_text()
            assert host.returncode == 0, 'host ping failed: ' + host_log.read_text()
            assert 'PING reply seq=1 octets=17' in log.read_text(), 'guest did not validate its echo reply'
            assert 'PING OK received=1 responded=1' in log.read_text(), 'guest did not complete both directions'
            report['success'] = True
            print(f'{args.mode}: real host/guest ping passed in both directions ({time.monotonic() - started:.3f}s)')
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        for process in [host, guest]:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        report['elapsed_seconds'] = time.monotonic() - started
        (build / 'live-results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
