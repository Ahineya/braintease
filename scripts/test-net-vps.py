#!/usr/bin/env python3
"""Build and test guest ICMP against one external IPv4 target through an attached NIC.

Start the vmnet helper in shared mode separately, without a capture argument.
Hostname resolution here is explicitly host-side; this does not test guest DNS.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def run_case(args, mode, target, resolution):
    build = ROOT / 'build/network/vps' / mode
    build.mkdir(parents=True, exist_ok=True)
    result = {'success': False, 'hostname': args.hostname, 'ipv4': target,
              'resolution': resolution, 'mode': mode, 'jit': args.jit and mode != 'native',
              'count': args.count, 'payload_octets': 17, 'timeout_seconds': args.timeout,
              'started_utc': datetime.now(timezone.utc).isoformat(),
              'script_requests_packet_capture': False}
    started = time.monotonic()
    try:
        flags = ['-DNET_TARGET_IP=' + target.replace('.', ','),
                 '-DPING_REQUESTS=' + str(args.count), '-DPING_RESPONSES=0']
        build_command = [str(ROOT / 'scripts/build-net-ping.sh'), mode, str(build)] + flags
        result['build_command'] = build_command
        with (build / 'vps-build.log').open('w') as output:
            compiled = subprocess.run(build_command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, timeout=120)
        if compiled.returncode:
            raise RuntimeError('build failed; see ' + str(build / 'vps-build.log'))
        env = os.environ.copy()
        if mode == 'native':
            executable = build / 'ping'
            command = [str(executable)]
            env.update(RCC_NATIVE_NET=str(args.socket), RCC_NATIVE_TIMEOUT=str(args.timeout), RCC_NATIVE_HEADLESS='1')
        else:
            executable = build / ('ping.bf' if mode == 'rbf' else 'ping.bin')
            command = [str(ROOT / 'target/release' / mode), str(executable), '--net', str(args.socket),
                       '--headless', '--exit-code', '--timeout', str(args.timeout),
                       '--report', str(build / 'vps-guest.json')]
            if mode == 'rvm':
                bank = int((build / 'bank-size').read_text())
                command += ['--bank-size', str(bank), '--memory', str(bank * 8), '--disk', str(build / 'disk.img')]
            if args.jit:
                command += ['--jit']
        result.update(guest_command=command, guest_sha256=digest(executable))
        execution_start = time.monotonic()
        log = build / 'vps-guest.log'
        with log.open('w') as output:
            guest = subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout + 5)
        result['execution_seconds'] = time.monotonic() - execution_start
        if guest.returncode:
            raise RuntimeError(f'guest exited {guest.returncode}; see {log}')
        text = log.read_text()
        replies = [int(value) for value in re.findall(r'^PING reply seq=(\d+) octets=17$', text, re.M)]
        if replies != list(range(1, args.count + 1)) or f'PING OK received={args.count} responded=0 ' not in text:
            raise RuntimeError('guest did not validate every requested echo reply; see ' + str(log))
        counters_line = next((line for line in text.splitlines() if line.startswith('NET counters ')), '')
        counters = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', counters_line)}
        if not counters or counters.get('tx_failed') != 0 or counters.get('backend_errors') != 0:
            raise RuntimeError('missing counters or backend failure; see ' + str(log))
        result.update(success=True, replies=replies, counters=counters)
        print(f'{mode}: {args.count}/{args.count} validated guest replies from {args.hostname} ({target}); '
              f'{result["execution_seconds"]:.3f}s execution including startup')
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        result['total_seconds'] = time.monotonic() - started
        (build / 'vps-results.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--hostname', default='syma-lang.org')
    parser.add_argument('--ipv4', help='use a configured IPv4 address instead of host-side resolution')
    parser.add_argument('--mode', choices=['rvm', 'native', 'rbf'], action='append',
                        help='runtime to test (repeatable; defaults to RVM and native; RBF is opt-in)')
    parser.add_argument('--count', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--jit', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.count <= 65535 or args.timeout <= 0:
        parser.error('count must be 1..65535 and timeout must be positive')
    args.socket = args.socket.resolve()
    if args.ipv4:
        target = str(ipaddress.IPv4Address(args.ipv4))
        resolution = {'source': 'configured IPv4; guest DNS not exercised'}
    else:
        addresses = sorted({item[4][0] for item in socket.getaddrinfo(args.hostname, None, socket.AF_INET, socket.SOCK_DGRAM)})
        if not addresses:
            raise RuntimeError('no IPv4 addresses for ' + args.hostname)
        target = addresses[0]
        resolution = {'source': 'host OS resolver; guest DNS not exercised', 'addresses': addresses,
                      'resolved_utc': datetime.now(timezone.utc).isoformat()}
    for mode in args.mode or ['rvm', 'native']:
        run_case(args, mode, target, resolution)


if __name__ == '__main__':
    main()
