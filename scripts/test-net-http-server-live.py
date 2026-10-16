#!/usr/bin/env python3
"""Fetch the guest's HTTP page with curl and ordinary host TCP sockets.

Guest DHCP, TCP listen/accept and HTTP run in freestanding C. macOS vmnet only
carries Ethernet. No host server is involved. Repeated requests exceed the TCB
pool size and exercise compact TIME_WAIT reuse without aborting normal closes.
"""
import argparse
import ast
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def expected_page():
    source = (ROOT / 'c-test/examples/network/http-page.h').read_text()
    part = source.split('static const unsigned char page[] =', 1)[1].split(';\nstatic const unsigned char missing', 1)[0]
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', part)).encode('ascii')


def decode(data, status, expected=None):
    header, body = data.split(b'\r\n\r\n', 1)
    lines = header.split(b'\r\n')
    assert lines[0].startswith(f'HTTP/1.0 {status} '.encode()), lines[0]
    fields = {}
    for line in lines[1:]:
        key, value = line.split(b':', 1)
        key = key.lower()
        assert key not in fields, header
        fields[key] = value.strip()
    assert int(fields[b'content-length']) == len(body), (fields, len(body))
    assert fields[b'connection'] == b'close' and fields[b'content-type'] == b'text/html; charset=us-ascii'
    if status == 405:
        assert fields[b'allow'] == b'GET'
    if expected is not None:
        assert body == expected, 'body differs from the guest page'
    return len(body)


def exchange(address, request, status, expected=None, split=False, half_close=False, hold=0):
    with socket.create_connection(address, timeout=10) as client:
        client.settimeout(12)
        if split:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            for octet in request:
                client.sendall(bytes([octet]))
                time.sleep(0.003)
        else:
            client.sendall(request)
        if half_close:
            client.shutdown(socket.SHUT_WR)
        response = bytearray()
        while True:
            part = client.recv(193)
            if not part:
                break
            response += part
            assert len(response) < 65536, 'unbounded response'
        size = decode(bytes(response), status, expected)
        if hold:
            time.sleep(hold)  # Withhold our FIN after acknowledging the server FIN.
        return size


def run(args, mode):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if mode == 'rvm' and args.jit else '')
    log, diagnostic = build / (label + '-host.log'), build / (label + '-host-stderr.log')
    result = {'success': False, 'mode': mode, 'checks': []}
    env = os.environ.copy()
    if mode == 'native':
        command = [str(build / 'http-server')]
        env.update(RCC_NATIVE_NET='host', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_PACED='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
    else:
        bank = int((build / 'bank-size').read_text())
        command = [str(ROOT / 'target/release/rvm'), str(build / 'http-server.bin'), '--net', 'host', '--clock', 'paced',
                   '--headless', '--exit-code', '--bank-size', str(bank), '--memory', str(bank * 8),
                   '--timeout', str(args.timeout), '--disk', str(build / 'server-live.disk')]
        if args.jit:
            command.append('--jit')
    started = time.monotonic()
    with log.open('w') as output, diagnostic.open('w') as errors:
        guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=errors)
        directory = Path(f'/tmp/ripple-vmnet-{guest.pid}-0')
        try:
            while True:
                ready = re.search(r'HTTP SERVER ready http://([0-9.]+):([0-9]+)/', log.read_text())
                if ready:
                    break
                if guest.poll() is not None or time.monotonic() - started > args.timeout:
                    raise RuntimeError('guest readiness failed: ' + log.read_text() + diagnostic.read_text())
                time.sleep(0.03)
            address = str(ipaddress.IPv4Address(ready[1])), int(ready[2])
            result['guest_address'] = list(address)
            page = expected_page()
            headers, body = build / (label + '-curl.headers'), build / (label + '-curl.html')
            curl = subprocess.run(['curl', '--noproxy', '*', '--silent', '--show-error', '--fail', '--connect-timeout', '10',
                                   '--max-time', '15', '-D', str(headers), '-o', str(body), f'http://{address[0]}:{address[1]}/'],
                                  capture_output=True, timeout=18)
            assert curl.returncode == 0, curl.stderr
            decode(headers.read_bytes() + body.read_bytes(), 200, page)
            result['checks'].append('curl-page')
            good = b'GET / HTTP/1.1\r\nHost: guest.test\r\n\r\n'
            tests = [
                ('split', good, 200, {'expected': page, 'split': True}),
                ('half-close', b'GET / HTTP/1.0\r\n\r\n', 200, {'expected': page, 'half_close': True}),
                ('missing', b'GET /missing HTTP/1.1\r\nHost: guest\r\n\r\n', 404, {}),
                ('method', b'POST / HTTP/1.1\r\nHost: guest\r\nContent-Length: 0\r\n\r\n', 405, {}),
                ('host', b'GET / HTTP/1.1\r\n\r\n', 400, {}),
                ('duplicate-host', b'GET / HTTP/1.1\r\nHost: guest\r\nHost: guest\r\n\r\n', 400, {}),
                ('length', b'GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: -1\r\n\r\n', 400, {}),
                ('body', b'GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: 1\r\n\r\nx', 413, {}),
                ('chunked', b'GET / HTTP/1.1\r\nHost: guest\r\nTransfer-Encoding: chunked\r\n\r\n', 501, {}),
                ('expect', b'POST / HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\n\r\n', 417, {}),
                ('header-limit', b'GET / HTTP/1.0\r\nX: ' + b'a' * 300 + b'\r\n\r\n', 431, {}),
                ('target-limit', b'GET /' + b'a' * 300 + b' HTTP/1.0\r\n\r\n', 414, {}),
                ('version', b'GET / HTTP/2.0\r\n\r\n', 505, {}),
                ('early', b'GET / HTTP/1.1\r\nHost:', 400, {'half_close': True}),
                ('stall', b'GET / HTTP/1.1\r\nHost:', 408, {}),
                ('close-stall', good, 200, {'expected': page, 'hold': 6}),
            ]
            for name, request, status, options in tests:
                exchange(address, request, status, **options)
                result['checks'].append(name)
                assert guest.poll() is None, log.read_text()
            # Reset one accepted stream, then prove normal requests still work.
            with socket.create_connection(address, timeout=10) as client:
                client.sendall(b'GET / HTTP/1.1\r\nHost:')
                time.sleep(0.2)
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            for i in range(8):
                exchange(address, good, 200, expected=page)
            result['checks'] += ['reset-recovery', 'eight-reuses']
            completed = 1 + len(tests) + 1 + 8
            deadline = time.monotonic() + 5
            while log.read_text().count('HTTP SERVER complete ') < completed:
                assert guest.poll() is None and time.monotonic() < deadline, log.read_text()
                time.sleep(0.03)
            result['connections_completed'] = completed
            assert guest.poll() is None, log.read_text()
            assert 'HTTP SERVER DHCP acquiring' in log.read_text(), 'live test must use guest DHCP'
            result.update(success=True, page_octets=len(page))
            print(f'{label}: curl page + {len(result["checks"]) - 1} host HTTP/stream checks passed, {len(page)} page octets', flush=True)
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
            deadline = time.monotonic() + 9
            while directory.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if directory.exists():
                result['success'] = False
                raise RuntimeError(f'owned helper remains: {directory}')
            result['elapsed_seconds'] = time.monotonic() - started
            (build / (label + '-host.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/http-server')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('live tests require macOS vmnet')
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['native', 'rvm']:
        run(args, mode)


if __name__ == '__main__':
    main()
