#!/usr/bin/env python3
"""Real asynchronous TCP and HTTP server on host/Fuse-X/RVM; private test roots."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT / 'ports/fuzix/tools'))
from network_peer import Peer


class Echo:
    def __init__(self):
        self.socket = socket.socket(); self.socket.bind(('127.0.0.1', 0)); self.socket.listen()
        self.port = self.socket.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True); self.thread.start()
    def serve(self):
        try:
            while True:
                conn, _ = self.socket.accept()
                def echo(conn):
                    try:
                        with conn:
                            while data := conn.recv(2048): conn.sendall(data)
                    except OSError: pass
                threading.Thread(target=echo, args=(conn,), daemon=True).start()
        except OSError: pass
    def close(self): self.socket.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=('host', 'native', 'ripple'), required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-server-tests')
    a = p.parse_args(); binary = a.binary.resolve(); build = a.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    report = build / (a.target + '-server-report.json'); report.unlink(missing_ok=True)
    records, guest, process, peer, echo = [], None, None, None, None
    finished = False
    with tempfile.TemporaryDirectory(prefix='qjs-server-') as temp:
        directory = Path(temp)
        try:
            if a.target == 'host':
                echo = Echo()
                process = subprocess.Popen([str(binary), '--server', str(PORT / 'tests/server.mjs'),
                    '127.0.0.1', str(echo.port), '127.0.0.1'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log = bytearray()
                def pump():
                    if select.select([process.stdout], [], [], .005)[0]: log.extend(os.read(process.stdout.fileno(), 65536))
            else:
                peer = Peer(directory / 'nic')
                guest = Guest(directory, a.target, {'js': binary}, peer=peer, blocks=65536,
                    files=[dict(name='/tmp/server.mjs', path=str(PORT / 'tests/server.mjs'))])
                log = guest.log
                os.write(guest.master, b'js --server /tmp/server.mjs 192.168.64.1 7001 0.0.0.0; echo SERVER_STATUS:$?\r')
                def pump(): guest.pump()
            def wait(condition, message, timeout=180):
                until = time.monotonic() + timeout
                while not condition():
                    pump()
                    if not condition() and ((process and process.poll() is not None) or
                        (message == 'server startup' and re.search(rb'\nSERVER_STATUS:[1-9]', log))):
                        raise AssertionError(message + '\n' + log[-6000:].decode(errors='replace'))
                    if time.monotonic() > until: raise AssertionError(message + '\n' + log[-6000:].decode(errors='replace'))
            wait(lambda: re.search(rb'\nSERVER_READY (\d+)', log), 'server startup')
            assert b'RAW_NET_OK' in log, log
            port = int(re.search(rb'\nSERVER_READY (\d+)', log)[1]); local = 42000
            records.append('raw TCP: binary echo, partial writes, timer fairness, accept timeout/cancel, stale handles, read cancellation')
            def start(data):
                nonlocal local
                local += 1
                if guest:
                    conn = peer.connect(port, local); conn.chunk = 137; conn.queue(data)
                else:
                    conn = socket.create_connection(('127.0.0.1', port)); conn.sendall(data); conn.setblocking(False)
                return conn
            def finish(conn, timeout=30):
                if guest:
                    wait(lambda: conn.fin or conn.reset, 'HTTP response completion', timeout)
                    data = bytes(conn.received)
                    # Acknowledge the server FIN and let it finish its close.
                    for _ in range(3): pump()
                else:
                    data = bytearray(); until = time.monotonic() + timeout
                    while True:
                        pump()
                        try:
                            part = conn.recv(65536)
                            if not part: break
                            data.extend(part)
                        except BlockingIOError:
                            if time.monotonic() > until: raise AssertionError('HTTP recv timeout')
                    conn.close(); data = bytes(data)
                return data
            def parse(data):
                head, body = data.split(b'\r\n\r\n', 1)
                lines = head.split(b'\r\n'); status = int(lines[0].split()[1])
                headers = dict(line.split(b':', 1) for line in lines[1:])
                headers = {k.decode().lower(): v.strip().decode() for k,v in headers.items()}
                assert headers['connection'] == 'close'
                return status, headers, body
            def request(path='/', method='GET', body=b'', extra=b''):
                data = method.encode() + b' ' + path.encode() + b' HTTP/1.1\r\nHost: test\r\n' + extra
                if body: data += b'Content-Length: ' + str(len(body)).encode() + b'\r\n'
                return parse(finish(start(data + b'\r\n' + body)))
            status, headers, body = request('/hello/%D0%BC%D0%B8%D1%80?q=one+two&q=%F0%9F%8C%8D')
            value = json.loads(body); assert status == 200 and value == dict(hello='мир', unicode='Привет 🌍', middleware=True, hops=24, query='one two', all=['one two','🌍'])
            assert int(headers['content-length']) == len(body)
            records.append('Unicode route params, repeated query values, middleware and octet Content-Length')
            payload = json.dumps(dict(message='Привет 🌍')).encode()
            reply = request('/echo', 'POST', payload)
            assert reply[0] == 201 and json.loads(reply[2]) == json.loads(payload)
            records.append('fragmented JSON POST')
            assert request('/hello/world', 'HEAD')[2] == b''
            assert request('/empty')[0::2] == (204, b'')
            assert request('/missing')[0] == 404
            assert request('/echo')[0] == 405 and request('/echo')[1]['allow'] == 'POST'
            assert request('/error')[0] == 500
            assert request('/injection')[0] == 500
            assert request('/echo', 'POST', b'{')[0] == 400
            records.append('HEAD, 204, 404, 405 Allow, handler failure, header injection and invalid JSON')
            for extra, expected in [
                (b'Content-Length: 1\r\nContent-Length: 1\r\n',400),
                (b'Content-Length: 1\r\nTransfer-Encoding: chunked\r\n',400),
                (b'Transfer-Encoding: chunked\r\n',501),
                (b'Content-Length: 999999\r\n',413),
                (b'Expect: 100-continue\r\n',417),
                (b'X-Test: bad\nvalue\r\n',400),
                (b'Host: other\r\n',400),
                (b'X-Long: '+b'x'*1100+b'\r\n',431)]:
                assert request('/echo','POST',extra=extra)[0] == expected, (extra,expected)
            records.append('strict request framing, duplicate headers, unsupported chunking/Expect and size bounds')
            slow = start(b'GET /slow HTTP/1.1\r\nHost: test\r\n\r\n')
            quick = request('/ticks'); slow_reply = parse(finish(slow))
            assert quick[0] == 200 and slow_reply[0] == 200 and json.loads(slow_reply[2])['ticks'] > json.loads(quick[2])['ticks']
            records.append('simultaneous async handler and responsive timers/second connection')
            assert request('/timeout')[0] == 408
            stalled = start(b'GET /hello/world HTTP/1.1\r\nHost: test\r\n')
            assert finish(stalled) == b''
            records.append('handler and partial-header deadlines')
            assert request('/large')[2] == b'x'*32768
            records.append('response larger than TCP buffers')
            dropped = start(b'GET /hello/world HTTP/1.1\r\nHost: test\r\n')
            if guest: dropped.closing = True
            else: dropped.close()
            assert request('/ticks')[0] == 200
            records.append('disconnect recovery')
            assert request('/stop')[0::2] == (200,b'STOPPED')
            wait(lambda: b'\nSERVER_CLOSED ' in log, 'graceful server shutdown')
            wait(lambda: re.search(rb'\nFORCE_READY (\d+)', log), 'second listener startup')
            port = int(re.search(rb'\nFORCE_READY (\d+)', log)[1])
            idle = [start(b'GET / HTTP/1.1\r\nHost: test\r\n') for _ in range(3)]
            if guest: wait(lambda: all(conn.established for conn in idle), 'saturated connections established')
            force = start(b'GET /force HTTP/1.1\r\nHost: test\r\n\r\n')
            finish(force)
            for conn in idle: finish(conn)
            wait(lambda: b'\nFORCE_CLOSED' in log, 'force shutdown')
            records.append('force close wakes saturated reads and shuts down all clients')
            if guest:
                guest.read(guest.prompt,60)
                assert b'SERVER_STATUS:0' in log, log[-4000:]
            else:
                process.wait(timeout=10); assert process.returncode == 0, log
            finished = True
            records.append('graceful listener close preserves in-flight reply')
        finally:
            (build / (a.target + '-server.log')).write_bytes(log if 'log' in locals() else b'')
            if guest:
                if not finished:
                    os.write(guest.master, b'\x03')
                    guest.read(guest.prompt, 60)
                guest.close()
            if peer: peer.close()
            if process and process.poll() is None: process.kill(); process.wait()
            if echo: echo.close()
    report.write_text(json.dumps(dict(status='passed',target=a.target,cases=records,
        executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        filesystem_check='passed' if guest else 'not applicable'),indent=2)+'\n')
    print(a.target + ': ' + str(len(records)) + ' server groups passed', flush=True)


if __name__ == '__main__': main()
