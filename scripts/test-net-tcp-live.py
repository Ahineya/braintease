#!/usr/bin/env python3
"""Guest DHCP/TCP against an ordinary macOS host TCP echo socket.

Only vmnet needs authorization. No host TCP or DNS shortcut is offered to the guest.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = bytes((i * 37 + 11) % 256 for i in range(4097))


def serve(args, timeout, stop, errors, counts):
    deadline = time.monotonic() + timeout
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            while not stop.is_set():
                try:
                    server.bind((args.host_ip, args.port))
                    break
                except OSError as error:
                    if error.errno != errno.EADDRNOTAVAIL or time.monotonic() >= deadline:
                        raise
                    stop.wait(0.02)  # The guest's first NIC discovery creates vmnet.
            if stop.is_set():
                return
            server.listen(1)
            server.settimeout(0.1)
            connection = None
            while not stop.is_set() and time.monotonic() < deadline:
                try:
                    connection, address = server.accept()
                    counts['guest_address'] = list(address)
                    break
                except socket.timeout:
                    pass
            if connection is None:
                if not stop.is_set():
                    raise RuntimeError('host echo accept timed out')
                return
            with connection:
                connection.settimeout(timeout)
                data = bytearray()
                while len(data) < len(EXPECTED):
                    part = connection.recv(257)
                    if not part:
                        raise RuntimeError('guest closed before the complete stream')
                    assert part == EXPECTED[len(data):len(data) + len(part)], 'guest stream differs from pattern'
                    data += part
                    connection.sendall(part)
                assert data == EXPECTED
                connection.shutdown(socket.SHUT_WR)
                assert connection.recv(1) == b'', 'guest sent extra bytes or failed to half-close'
                counts.update(received=len(data), echoed=len(data), eof=True)
    except Exception as error:
        errors.append(str(error))


def run(args, mode, timeout):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if mode != 'native' and args.jit else '')
    log = build / (label + '-live.log')
    stderr_log = build / (label + '-live-stderr.log')
    result = {'success': False, 'mode': mode}
    stop, errors, counts = threading.Event(), [], {}
    server = threading.Thread(target=serve, args=(args, timeout, stop, errors, counts), daemon=True)
    server.start()
    began = time.monotonic()
    env = os.environ.copy()
    if mode == 'native':
        command = [str(build / 'tcp-echo')]
        env.update(RCC_NATIVE_NET='host', RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(timeout))
    else:
        extension = 'bf' if mode == 'rbf' else 'bin'
        command = [str(ROOT / 'target/release' / mode), str(build / ('tcp-echo.' + extension)),
                   '--net', 'host', '--clock', 'paced', '--headless', '--exit-code', '--timeout', str(timeout)]
        if mode == 'rvm':
            bank = int((build / 'bank-size').read_text())
            command += ['--bank-size', str(bank), '--memory', str(bank * 8), '--disk', str(build / 'live.disk')]
        else:
            command += ['--telemetry-lite', '--report', str(build / (label + '-report.json'))]
        if args.jit:
            command.append('--jit')
    guest = None
    displayed = displayed_stderr = 0
    try:
        with log.open('w') as output, stderr_log.open('w') as diagnostics:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=diagnostics)
            while guest.poll() is None:
                content = log.read_text()
                diagnostic = stderr_log.read_text()
                if args.verbose and len(content) > displayed:
                    print(content[displayed:], end='', flush=True)
                    displayed = len(content)
                if args.verbose and len(diagnostic) > displayed_stderr:
                    print(diagnostic[displayed_stderr:], end='', flush=True)
                    displayed_stderr = len(diagnostic)
                if errors:
                    raise RuntimeError('host echo: ' + errors[0])
                if time.monotonic() - began > timeout:
                    raise RuntimeError('guest TCP timeout: ' + content + diagnostic)
                stop.wait(0.02)
            content = log.read_text()
            if args.verbose:
                print(content[displayed:], end='', flush=True)
                print(stderr_log.read_text()[displayed_stderr:], end='', flush=True)
            server.join(timeout=3)
            assert guest.returncode == 0, content + stderr_log.read_text()
            assert not errors, errors
            assert counts.get('received') == 4097 and counts.get('echoed') == 4097 and counts.get('eof'), counts
            assert 'TCP OK written=4097 acknowledged=4097 received=4097' in content, content
            assert 'TCP DHCP acquiring' in content, 'live demo must acquire its configuration in guest C'
            result.update(success=True, host=counts)
            print(f'{label}: guest DHCP + 4097 binary TCP octets echoed by the host kernel, EOF and close verified', flush=True)
    finally:
        if guest and guest.poll() is None:
            guest.terminate()
            try:
                guest.wait(timeout=3)
            except subprocess.TimeoutExpired:
                guest.kill()
                guest.wait()
        stop.set()
        server.join(timeout=3)
        if guest:
            directory = Path(f'/tmp/ripple-vmnet-{guest.pid}-0')
            cleanup = time.monotonic() + 9
            while directory.exists() and time.monotonic() < cleanup:
                time.sleep(0.1)
            if directory.exists():
                result['success'] = False
                raise RuntimeError(f'owned network helper remains: {directory}')
        result['elapsed_seconds'] = time.monotonic() - began
        (build / (label + '-live.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm', 'rbf'], action='append')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/tcp-echo')
    parser.add_argument('--host-ip', default='192.168.64.1')
    parser.add_argument('--port', type=int, default=7777)
    parser.add_argument('--timeout', type=int, help='wall seconds; default 60, or 1800 for explicit RBF runs')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('automatic live host echo currently requires macOS vmnet; use the packet fixture elsewhere')
    if (args.timeout is not None and args.timeout <= 0) or not 1024 <= args.port <= 65535:
        parser.error('timeout must be positive; host echo port must be 1024..65535')
    for mode in args.mode or ['native', 'rvm']:
        run(args, mode, args.timeout if args.timeout is not None else (1800 if mode == 'rbf' else 60))


if __name__ == '__main__':
    main()
