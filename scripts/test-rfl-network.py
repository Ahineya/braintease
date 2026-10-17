#!/usr/bin/env python3
"""Build and verify RFL HTTP against independent Ethernet peers (no vmnet/Internet).

Both targets use the guest C DNS/TCP/HTTP stack. Server tests open six successive
connections, exceeding the four live slots, with loss, reordering and zero-window
backpressure. Outputs and packet checksums are checked, not just process exit.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/network/rfl-http'
BANK = 64000


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


client = module('rfl_client_peer', 'scripts/test-net-http.py')
server = module('rfl_server_peer', 'scripts/test-net-http-server.py')
dhcp = module('rfl_dhcp_peer', 'scripts/test-net-dhcp.py')
for protocol in (client, server):
    protocol.wire.GUEST_IP = bytes([192, 168, 64, 42])
server.PAGE = b'<h1>Async RFL</h1>\n'
server.RESPONSE = (f'HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=us-ascii\r\nContent-Length: {len(server.PAGE)}\r\nConnection: close\r\n\r\n').encode() + server.PAGE


class DhcpPeer(client.Peer):
    def __init__(self, sock):
        super().__init__(sock)
        self.discovered = self.requested = False
        self.probes = 0

    def packet(self, frame):
        if frame[12:14] == b'\x08\x06' and frame[28:32] == b'\0' * 4:
            self.probes += 1
            return
        if frame[12:14] == b'\x08\x00' and frame[23] == 17 and frame[36:38] == b'\0C':
            total = struct.unpack('!H', frame[16:18])[0]
            packet = frame[34:14 + total]
            assert dhcp.wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, len(packet)) + packet) == 0
            query = packet[8:]
            options = dhcp.options(query)
            kind = options[53][0]
            assert kind in (1, 3)
            self.discovered |= kind == 1
            self.requested |= kind == 3
            guest = dict(mac=client.wire.GUEST_MAC, ip=client.wire.GUEST_IP)
            reply = dhcp.dhcp_reply(guest, query, 2 if kind == 1 else 5)
            data = bytearray(reply[42:])
            offset = 240
            while data[offset] != 255:
                code, length = data[offset:offset + 2]
                if code == 6:
                    data[offset + 2:offset + 2 + length] = b'\1' * 4
                offset += length + 2
            self.send(dhcp.udp(guest, data, 67, 68, dhcp.BROADCAST))
            return
        super().packet(frame)


class ServerPeer(server.Peer):
    def __init__(self, sock, case, index):
        super().__init__(sock, case)
        self.port = 50000 + index

    def frame(self, *args, **kwargs):
        frame = bytearray(super().frame(*args, **kwargs))
        frame[34:36] = struct.pack('!H', self.port)
        frame[50:52] = b'\0\0'
        total = struct.unpack('!H', frame[16:18])[0]
        segment = frame[34:14 + total]
        pseudo = frame[26:34] + struct.pack('!BBH', 0, 6, len(segment))
        check = server.wire.checksum(pseudo + segment) ^ int(kwargs.get('corrupt', False))
        frame[50:52] = struct.pack('!H', check)
        return bytes(frame)

    def packet(self, frame):
        # The shared oracle checks a fixed client port. Normalize only that
        # field and its checksum before forwarding, preserving all other checks.
        if frame[12:14] == b'\x08\x00' and frame[23] == 6:
            assert struct.unpack('!H', frame[36:38])[0] == self.port
            total = struct.unpack('!H', frame[16:18])[0]
            segment = frame[34:14 + total]
            assert server.wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 6, len(segment)) + segment) == 0
            frame = bytearray(frame)
            frame[36:38] = struct.pack('!H', 50000)
            frame[50:52] = b'\0\0'
            total = struct.unpack('!H', frame[16:18])[0]
            segment = frame[34:14 + total]
            pseudo = frame[26:34] + struct.pack('!BBH', 0, 6, len(segment))
            frame[50:52] = struct.pack('!H', server.wire.checksum(pseudo + segment))
        super().packet(frame)


def build(mode, kind):
    directory = BUILD / mode
    directory.mkdir(parents=True, exist_ok=True)
    source = ROOT / f'rfl/tests/fixtures/network/{kind}.rfl'
    output = directory / f'http-{kind}'
    command = [str(ROOT / 'target/release/rfl'), 'compile', str(source)]
    if mode == 'native':
        command += ['--target', 'native', '-o', str(output)]
    else:
        command += ['-o', str(output.with_suffix('.asm'))]
    with (directory / f'{kind}-build.log').open('w') as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        if mode == 'rvm':
            subprocess.run(['make', '-C', str(ROOT / 'runtime'), '-j4', f'BANK_SIZE={BANK}', 'libruntime.par', 'crt0.pobj'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            asm = ROOT / 'src/ripple-asm/target/release'
            subprocess.run([str(asm / 'rasm'), 'assemble', str(output.with_suffix('.asm')), '--bank-size', str(BANK), '--max-immediate', '65535', '-o', str(output.with_suffix('.pobj'))], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run([str(asm / 'rlink'), str(ROOT / 'runtime/crt0.pobj'), str(output.with_suffix('.pobj')), str(ROOT / 'runtime/libruntime.par'), '-f', 'binary', '--bank-size', str(BANK), '-o', str(output.with_suffix('.bin'))], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)


def run(args, mode, kind):
    directory = BUILD / mode
    executable = directory / f'http-{kind}'
    label = f'{kind}-' + ('jit' if mode == 'rvm' and args.jit else mode)
    log = directory / f'{label}.log'
    diagnostic = directory / f'{label}-stderr.log'
    with tempfile.TemporaryDirectory(prefix='rflnet-') as temporary, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        path = str(Path(temporary) / 'nic.sock')
        sock.bind(path)
        sock.settimeout(.01)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(executable)]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
        else:
            command = [str(ROOT / 'target/release/rvm'), str(executable.with_suffix('.bin')), '--net', path, '--headless', '--exit-code', '--timeout', str(args.timeout), '--bank-size', str(BANK), '--memory', str(BANK * 32), '--disk', str(directory / f'{label}.disk')]
            if args.jit:
                command += ['--jit']
        peer = DhcpPeer(sock) if kind == 'dhcp' else client.Peer(sock) if kind == 'client' else ServerPeer(sock, 'success', 0)
        address = None
        completed = 0
        cases = ['success', 'loss', 'reorder', 'zero', 'malformed', 'success']
        started = time.monotonic()
        with log.open('w') as output, diagnostic.open('w') as error:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=error)
            try:
                while guest.poll() is None:
                    if time.monotonic() - started > args.timeout:
                        raise RuntimeError(f'{label} timed out: {log.read_text()}\n{diagnostic.read_text()}')
                    try:
                        frame, source = sock.recvfrom(65536)
                    except socket.timeout:
                        frame = None
                    if frame == b'RNET\1':
                        address = source
                        peer.address = source
                        sock.sendto(b'RNET\1' + client.wire.GUEST_MAC + struct.pack('!H', 1500), source)
                    elif frame is not None:
                        assert source == address
                        peer.packet(frame)
                    if kind in ('client', 'dhcp'):
                        peer.progress()
                    else:
                        ready = 'HTTP ready' in log.read_text()
                        peer.progress(ready)
                        if peer.done and log.read_text().count('SERVED') > completed:
                            assert peer.received == server.RESPONSE
                            if peer.case == 'loss':
                                assert all([peer.syn_dropped, peer.data_dropped, peer.ack_dropped, peer.fin_dropped, peer.last_ack_dropped]) and peer.late_acks
                            if peer.case == 'zero':
                                assert peer.probes >= 2
                            if peer.case == 'reorder':
                                assert peer.reordered
                            completed += 1
                            if completed < len(cases):
                                peer = ServerPeer(sock, cases[completed], completed)
                                peer.address = address
                # A process can exit after sending its final ACK while that
                # datagram is still queued in the host socket. Check it too.
                if kind == 'server' and completed == 5 and not peer.done:
                    until = time.monotonic() + .2
                    while not peer.done and time.monotonic() < until:
                        try:
                            frame, source = sock.recvfrom(65536)
                        except socket.timeout:
                            continue
                        assert source == address
                        if len(frame) >= 54:
                            peer.packet(frame)
                content = log.read_text()
                assert guest.returncode == 0, content + diagnostic.read_text()
                if kind in ('client', 'dhcp'):
                    assert peer.dns and peer.client_fin and peer.request == client.REQUEST
                    expected = f'200\n{len(client.BODY)}\n{sum(client.BODY)}\nRFL HTTP OK\n'
                    if kind == 'dhcp':
                        assert peer.discovered and peer.requested and peer.probes == 3
                        expected = '192.168.64.42\n1.1.1.1\n' + expected
                    assert content == expected, content
                else:
                    # The final ACK and process exit can share the last host turn.
                    assert content.count('SERVED') == 6 and (completed == 6 or completed == 5 and peer.done), content
                print(f'{label}: passed in {time.monotonic() - started:.2f}s', flush=True)
            finally:
                if guest.poll() is None:
                    guest.terminate()
                    try:
                        guest.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        guest.kill()
                        guest.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', action='append', choices=['native', 'rvm'])
    parser.add_argument('--case', action='append', choices=['client', 'server', 'dhcp'])
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['native', 'rvm']:
        for kind in args.case or ['client', 'server', 'dhcp']:
            if not args.skip_build:
                build(mode, kind)
            run(args, mode, kind)


if __name__ == '__main__':
    main()
