"""Real host TCP/HTTP <-> guest stack and independent PTY shell acceptance."""
import http.server
import json
from pathlib import Path
import socket
import threading
import time

PAYLOAD = bytes(range(256)) * 17 + b'Riplix x64 Ethernet\n'


class NetworkCheck:
    def __init__(self, command, port, output, supervised=False):
        self.command, self.port, self.output = command, port, Path(output)
        self.supervised = supervised
        self.sessions = []
        self.results = []

    def __enter__(self):
        payload = PAYLOAD
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *args):
                pass
        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.worker = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.worker.start()
        return self

    def __exit__(self, *args):
        for session in self.sessions:
            session.close()
        self.http.shutdown()
        self.http.server_close()
        self.worker.join()
        if not args[0]:
            (self.output / 'network-result.json').write_text(json.dumps(self.results, indent=2) + '\n')

    def run(self, shell, transcript, deadline, restart, qmp):
        if restart:
            shell('cat /tmp/remote-note', r'\r?\nREMOTE_X64_FILE\r?\n')
        shell('ifconfig', r'eth0  UP  link up.*[\s\S]*inet 192\.168\.64\.250')
        shell('ping -c 1 192.168.64.1', r'1 packets transmitted, 1 received')
        shell('host example.com; echo DNS_STATUS:$?', r'DNS_STATUS:0\r?\n')
        shell(f'netcheck 192.168.64.1 {self.http.server_port} /tmp/net-download; echo HTTP_STATUS:$?',
              r'HTTP_STATUS:0\r?\n')
        shell('wc -c /tmp/net-download', rf'\b{len(PAYLOAD)}\b')
        if not self.supervised:
            shell('telnetd &', r'\[\d+\]|fuse-x#')
        while not self.supervised and 'telnetd: listening on ' not in transcript():
            if time.monotonic() >= deadline:
                raise RuntimeError('guest Telnet server did not start')
            time.sleep(.02)
        def connect():
            until = min(deadline, time.monotonic() + 25)
            while True:
                try:
                    sock = socket.create_connection(('127.0.0.1', self.port), timeout=1)
                    sock.settimeout(1)
                    self.sessions.append(sock)
                    return sock
                except OSError:
                    if time.monotonic() > until:
                        raise RuntimeError('host could not connect to the guest Telnet server')
                    time.sleep(.05)
        def read_until(sock, marker, seconds=35):
            data = bytearray()
            until = min(deadline, time.monotonic() + seconds)
            while marker not in data:
                if time.monotonic() > until:
                    raise RuntimeError(f'remote shell timed out waiting for {marker!r}: {bytes(data)!r}; '
                                       f'console tail: {transcript()[-3000:]}')
                try:
                    part = sock.recv(8192)
                except socket.timeout:
                    continue
                if not part:
                    raise RuntimeError(f'remote shell disconnected: {bytes(data)!r}')
                data.extend(part)
            return bytes(data)
        def remote(sock, line, expected):
            sock.sendall(line.encode() + b'\r\n')
            output = read_until(sock, b'riplix# ')
            plain = output.replace(b'\r', b'').replace(b'\0', b'')
            if expected not in plain:
                raise RuntimeError(f'remote command {line!r} failed: {plain!r}')
            return plain
        first, second = connect(), connect()
        for sock in (first, second):
            sock.sendall(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f')
            read_until(sock, b'riplix# ')
        remote(first, 'echo REMOTE_X64_FILE > /tmp/remote-note', b'riplix# ')
        remote(second, 'cat /tmp/remote-note', b'\nREMOTE_X64_FILE\n')
        remote(first, 'echo native-network | cat', b'\nnative-network\n')
        # Compare every downloaded octet over the actual guest TCP -> PTY ->
        # Telnet -> host path, including zero and escaped IAC bytes.
        first.sendall(b'cat /tmp/net-download\r\n')
        binary = read_until(first, b'riplix# ')
        # Telnet doubles IAC and maps LF to CRLF; literal CR is CR NUL.
        decoded = bytearray(); i = 0
        while i < len(binary):
            b = binary[i]; i += 1
            if b == 255:
                if i < len(binary) and binary[i] == 255:
                    decoded.append(255); i += 1
                elif i < len(binary) and binary[i] in (251,252,253,254):
                    i += 2
                continue
            if b == 13 and i < len(binary) and binary[i] in (0,10):
                b = 13 if binary[i] == 0 else 10; i += 1
            decoded.append(b)
        if PAYLOAD not in decoded:
            raise RuntimeError('binary HTTP download differs over the remote terminal')
        # Exercise an interrupted job while an independent PTY remains usable.
        first.sendall(b'sleep 30\r\n')
        read_until(first, b'sleep 30')
        time.sleep(.2)
        first.sendall(b'\x03')
        read_until(first, b'riplix# ')
        remote(first, 'echo INTERRUPTED:$?', b'\nINTERRUPTED:130\n')
        remote(second, 'echo SECOND_SESSION_OK', b'\nSECOND_SESSION_OK\n')
        for sock in (first, second):
            sock.sendall(b'exit\r\n'); sock.close(); self.sessions.remove(sock)
        for _ in range(3):
            sock = connect();sock.sendall(b'\xff\xfd\x01\xff\xfd\x03')
            read_until(sock, b'riplix# ')
            remote(sock, 'echo RECONNECTED', b'\nRECONNECTED\n')
            sock.sendall(b'exit\r\n');sock.close();self.sessions.remove(sock)
        qmp('set_link', {'name':'riplixnic', 'up':False})
        shell('ifconfig', r'link down')
        qmp('set_link', {'name':'riplixnic', 'up':True})
        shell('ifconfig', r'link up')
        # Link loss terminates the old listening socket. A fresh service must
        # accept connections on the restored hardware interface.
        at = len(transcript())
        if self.supervised:
            shell('svc --runtime /var/run/svcd restart telnetd', r'OK restart requested')
        else:
            shell('telnetd &', r'\[\d+\]|fuse-x#')
        while not self.supervised and 'telnetd: listening on ' not in transcript()[at:]:
            if time.monotonic() >= deadline:
                raise RuntimeError('Telnet listener did not recover after link restoration')
            time.sleep(.02)
        sock=connect();sock.sendall(b'\xff\xfd\x01\xff\xfd\x03');read_until(sock,b'riplix# ')
        remote(sock,'echo LINK_RECOVERED',b'\nLINK_RECOVERED\n')
        sock.sendall(b'exit\r\n');sock.close();self.sessions.remove(sock)
        shell('echo NETWORK_REMOTE_SHELL_OK', r'\r?\nNETWORK_REMOTE_SHELL_OK\r?\n')
        self.results.append(dict(boot=2 if restart else 1, status='passed',
                                 ping=True, dns=True, binary_http_bytes=len(PAYLOAD),
                                 concurrent_remote_shells=2, reconnects=3,
                                 pipelines=True, ctrl_c=True, link_recovery=True))
        print('PASS: x64 Ethernet ping/DNS/HTTP, concurrent Telnet PTYs, Ctrl-C and reconnects', flush=True)
