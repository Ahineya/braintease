#!/usr/bin/env python3
"""Guest DHCP/HTTP against an ordinary host kernel TCP socket (macOS vmnet).

No host resolver or HTTP shortcut is exposed to the guest. The service inspects
GET/Host and streams a body larger than a guest bank through a 173-cell reader.
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
BODY = (b'<!doctype html>\n<html><body><h1>Hello from the host!</h1>\n' +
        b'<p>Guest HTTP over guest TCP, one octet per cell.</p>\n' * 240 + b'</body></html>\n')
CASES = ['length', 'close', 'split', 'hold-open', 'empty', 'not-found', 'conflict', 'early', 'chunked', 'limit', 'stall']
ERRORS = {'conflict': 32, 'early': 34, 'chunked': 16, 'limit': 33, 'stall': 8}


def response(case):
    body, code = BODY, 200
    if case == 'empty':
        return b'HTTP/1.0 204 No Content\r\nX-Test: empty\r\n\r\n', b'', 204
    if case == 'not-found':
        body, code = b'<html>Nothing here.</html>\n', 404
    header = f'HTTP/1.0 {code} Test\r\nContent-Type: text/html\r\n'.encode()
    if case == 'conflict':
        return header + b'Content-Length: 2\r\nContent-Length: 3\r\n\r\nxx', None, code
    if case == 'chunked':
        return header + b'Transfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n', None, code
    if case == 'limit':
        return header + b'Content-Length: 1048577\r\n\r\n', None, code
    if case == 'early':
        return header + b'Content-Length: 999\r\n\r\nshort', None, code
    if case == 'stall':
        return header + b'Content-Length: 999\r\n\r\npartial', None, code
    if case != 'close':
        header += f'Content-Length: {len(body)}\r\n'.encode()
    return header + b'\r\n' + body, body, code


def serve(args, case, stop, errors, counts):
    deadline = time.monotonic() + args.timeout
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
                    stop.wait(0.02)  # First guest NIC discovery creates vmnet.
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
                    raise RuntimeError('HTTP accept timeout')
                return
            with connection:
                connection.settimeout(0.1)
                request = bytearray()
                while b'\r\n\r\n' not in request:
                    if stop.is_set() or time.monotonic() > deadline:
                        raise RuntimeError('HTTP request timeout')
                    try:
                        part = connection.recv(257)
                    except socket.timeout:
                        continue
                    if not part:
                        raise RuntimeError('guest closed before request headers')
                    request += part
                    assert len(request) <= 768, 'unbounded request'
                expected = (f'GET / HTTP/1.0\r\nHost: {args.host_ip}:{args.port}\r\n'
                            'Connection: close\r\nAccept-Encoding: identity\r\n\r\n').encode()
                assert request == expected, request
                counts['request'] = request.decode()
                data, body, code = response(case)
                connection.settimeout(args.timeout)
                if case == 'split':
                    boundary = data.index(b'\r\n\r\n') + 4
                    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    for octet in data[:boundary]:
                        connection.sendall(bytes([octet]))
                        stop.wait(0.003)
                    data = data[boundary:]
                for start in range(0, len(data), 257):
                    connection.sendall(data[start:start + 257])
                counts['response_octets'] = len(data)
                if case not in ['hold-open', 'stall']:
                    connection.shutdown(socket.SHUT_WR)
                connection.settimeout(0.1)
                while not stop.is_set() and time.monotonic() < deadline:
                    try:
                        more = connection.recv(1)
                    except socket.timeout:
                        continue
                    except ConnectionResetError:
                        assert case in ERRORS, 'unexpected guest RST'
                        counts['reset'] = True
                        break
                    assert not more, 'guest sent more than one request'
                    counts['eof'] = True
                    break
                if case not in ERRORS:
                    assert counts.get('eof'), 'guest did not half-close'
    except OSError as error:
        # An expected guest failure tears down its owned vmnet interface. Darwin
        # may report that disappearance during the server's shutdown/read.
        if case in ERRORS and counts.get('response_octets') and error.errno in [errno.ECONNRESET, errno.EPIPE, errno.ENOTCONN, errno.ENETDOWN, errno.EADDRNOTAVAIL]:
            counts['peer_teardown'] = str(error)
        else:
            errors.append(str(error))
    except Exception as error:
        errors.append(str(error))


def run(args, mode, case):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if mode != 'native' and args.jit else '') + '-' + case
    log, stderr_log = build / (label + '.log'), build / (label + '-stderr.log')
    result = {'success': False, 'mode': mode, 'case': case}
    stop, errors, counts = threading.Event(), [], {}
    server = threading.Thread(target=serve, args=(args, case, stop, errors, counts), daemon=True)
    server.start()
    began = time.monotonic()
    env = os.environ.copy()
    if mode == 'native':
        command = [str(build / 'http-get')]
        env.update(RCC_NATIVE_NET='host', RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
    else:
        bank = int((build / 'bank-size').read_text())
        command = [str(ROOT / 'target/release/rvm'), str(build / 'http-get.bin'), '--net', 'host',
                   '--clock', 'paced', '--headless', '--exit-code', '--timeout', str(args.timeout),
                   '--bank-size', str(bank), '--memory', str(bank * 8), '--disk', str(build / 'http.disk')]
        if args.jit:
            command.append('--jit')
    guest = None
    displayed = 0
    try:
        with log.open('w') as output, stderr_log.open('w') as diagnostic:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=diagnostic)
            while guest.poll() is None:
                content = log.read_text()
                if args.verbose and len(content) > displayed:
                    print(content[displayed:], end='', flush=True)
                    displayed = len(content)
                if errors:
                    raise RuntimeError('host HTTP: ' + errors[0])
                if time.monotonic() - began > args.timeout:
                    raise RuntimeError('guest HTTP timeout: ' + content + stderr_log.read_text())
                stop.wait(0.02)
            content = log.read_text()
            if args.verbose:
                print(content[displayed:], end='', flush=True)
                print(stderr_log.read_text(), end='', flush=True)
            server.join(timeout=3)
            assert not errors, errors
            assert counts.get('request'), counts
            if case in ERRORS:
                assert guest.returncode == 1 and f'HTTP ERROR status={ERRORS[case]} ' in content, content + stderr_log.read_text()
                assert 'HTTP OK' not in content, content
            else:
                _, body, code = response(case)
                assert guest.returncode == 0, content + stderr_log.read_text()
                assert 'HTTP BODY\n' + body.decode() + '\nHTTP END\n' in content, 'streamed body differs'
                assert f'HTTP OK status={code} received={len(body)} checksum={sum(body)}' in content, content
                assert counts.get('eof'), counts
            assert 'HTTP DHCP acquiring' in content, 'live test must configure NIC in guest C'
            result.update(success=True, host=counts)
            print(f'{label}: passed (ordinary host HTTP, guest DHCP/TCP/parsing)', flush=True)
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
                raise RuntimeError(f'owned network helper remains: {directory}')
        result['elapsed_seconds'] = time.monotonic() - began
        (build / (label + '.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/http-get')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--host-ip', default='192.168.64.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('live host service requires macOS vmnet')
    if args.timeout <= 0 or not 1024 <= args.port <= 65535:
        parser.error('timeout must be positive and port must be 1024..65535')
    for mode in args.mode or ['native', 'rvm']:
        for case in args.case or CASES:
            run(args, mode, case)


if __name__ == '__main__':
    main()
