#!/usr/bin/env python3
"""Native FUZIX sockets and independent Telnet shells over real Ethernet frames."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
from test_support import check_root, record_runtime

HERE = Path(__file__).resolve().parent
PORT = HERE.parent / 'ports/fuzix'
sys.path.insert(0, str(PORT / 'tools'))
from network_peer import Peer, wire

BINARY = bytes(range(256)) * 4 + b'Native FUZIX networking\n'


class Services(Peer):
    def __init__(self, path):
        super().__init__(path)
        self.tcp_services = {80: self.http}
        self.dns_queries = 0

    def packet(self, frame):
        if len(frame) >= 42 and frame[12:14] == b'\x08\x00' and frame[23] == 17:
            size = struct.unpack('!H', frame[16:18])[0]
            at = 14 + (frame[14] & 15) * 4
            packet = frame[at:14 + size]
            source, target, length, _ = struct.unpack('!HHHH', packet[:8])
            assert length == len(packet)
            assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, length) + packet) == 0
            data = packet[8:]
            if target == 53:
                self.dns_queries += 1
                at = 12
                while data[at]:
                    at += data[at] + 1
                end = at + 5
                assert data[at + 1:end] == b'\0\x01\0\x01', data
                data = (data[:2] + struct.pack('!HHHHH', 0x8180, 1, 1, 0, 0)
                        + data[12:end] + b'\xc0\x0c'
                        + struct.pack('!HHIH', 1, 1, 60, 4) + wire.HOST_IP)
            elif target == 123:
                response = bytearray(48)
                response[0:2] = b'\x24\x01'
                response[24:32] = data[40:48]
                response[40:44] = struct.pack('!I', (1900000000 + 2208988800) & 0xffffffff)
                data = response
            else:
                return super().packet(frame)
            response = bytearray(struct.pack('!HHHH', target, source, len(data) + 8, 0) + data)
            check = wire.checksum(wire.HOST_IP + wire.GUEST_IP
                                  + struct.pack('!BBH', 0, 17, len(response)) + response)
            response[6:8] = struct.pack('!H', check or 65535)
            self.send(self.ip(17, response))
            return
        super().packet(frame)

    def http(self, stream):
        if getattr(stream, 'answered', False) or b'\r\n\r\n' not in stream.received:
            return
        stream.answered = True
        stream.queue(b'HTTP/1.1 200 OK\r\nContent-Length: '
                     + str(len(BINARY)).encode() + b'\r\n\r\n' + BINARY)

    def pump(self):
        super().pump()
        # Close after the response is acknowledged. The shared fixture's eager
        # close can send FIN when a fast native client advertises a zero window
        # even though response bytes are still queued.
        for stream in self.streams.values():
            if (stream.local == 80 and getattr(stream, 'answered', False)
                    and not stream.pending and not stream.flight):
                stream.closing = True
                stream.progress()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    build = executable.parent
    environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT='150',
                       RCC_NATIVE_NET_EAGER='1')
    # Keep Unix socket names below macOS's short AF_UNIX path limit.
    with tempfile.TemporaryDirectory(prefix='fxnet-', dir='/tmp') as temporary:
        directory = Path(temporary)
        image = directory / 'root.img'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image),
                        '--apps', str(build / 'apps')], check=True)
        check = directory / 'netcheck.fxe'
        subprocess.run(['python3', str(HERE / 'cc.py'), str(PORT / 'tools/netcheck.c'),
                        '--compiler', str(args.compiler.resolve()), '--build', str(build),
                        '-o', str(check)], check=True)
        subprocess.run(['node', str(HERE / 'image.mjs'), 'put', str(image), '/bin/netcheck', str(check)], check=True)
        peer = Services(directory / 'nic')
        environment['RCC_NATIVE_NET'] = str(directory / 'nic')
        log = directory / 'console.log'
        with log.open('wb') as output:
            guest = subprocess.Popen([str(executable), '--run', str(image)], stdin=subprocess.PIPE,
                                     stdout=output, stderr=subprocess.STDOUT, env=environment)

            def console():
                return log.read_bytes().replace(b'\r', b'')

            def until(condition, seconds=25):
                deadline = time.monotonic() + seconds
                while not condition():
                    assert guest.poll() is None and time.monotonic() < deadline, (
                        console().decode(errors='replace')[-6000:],
                        {str(k): bytes(v.received)[-2000:] for k, v in peer.streams.items()})
                    peer.pump()

            def send(command):
                guest.stdin.write(command.encode() + b'\n')
                guest.stdin.flush()

            def spin(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    peer.pump()

            def received(stream):
                return bytes(stream.received).replace(b'\r', b'').replace(b'\0', b'')

            def line(stream, command):
                at = len(received(stream))
                stream.queue(command + b'\r\n')
                until(lambda: b'fuzix# ' in received(stream)[at:])
                return received(stream)[at:]

            def connect(port):
                stream = peer.connect(23, port)
                until(lambda: stream.established)
                stream.queue(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f')
                until(lambda: b'fuzix# ' in received(stream))
                return stream

            try:
                until(lambda: b'fuse-x# ' in console())
                send('netcheck; echo NET_STATUS:$?; ifconfig; ping -c 1 192.168.64.1; echo PING_STATUS:$?')
                until(lambda: b'PING_STATUS:0\n' in console(), 45)
                assert b'NET_STATUS:0\n' in console(), console()
                assert b'inet 192.168.64.250' in console(), console()
                until(lambda: peer.ping_replies > 0)
                assert peer.ping_requests
                print('Native socket ABI, UDP/TCP, fork/dup and bidirectional ICMP passed', flush=True)
                for command in [
                    'host fuse-x.test', 'nslookup fuse-x.test', 'dig fuse-x.test A +short',
                    'echoping 192.168.64.1 7000 NATIVE_ECHO; echo ECHO_STATUS:$?',
                    'ntpdate -d 192.168.64.1; echo NTP_STATUS:$?',
                    'htget http://fuse-x.test/binary /tmp/download; echo HTTP_STATUS:$?',
                    'cksum /tmp/download',
                ]:
                    send(command)
                checksum = subprocess.check_output(['cksum'], input=BINARY).split()[0]
                until(lambda: checksum + b' ' + str(len(BINARY)).encode() + b' /tmp/download\n' in console())
                assert b'fuse-x.test has address 192.168.64.1' in console(), console()
                for token in [b'ECHO_STATUS:0\n', b'NTP_STATUS:0\n', b'HTTP_STATUS:0\n']:
                    assert token in console(), console()
                assert peer.dns_queries >= 4
                print('Native DNS clients, UDP echo/NTP and binary HTTP download passed', flush=True)
                send('telnetd &')
                until(lambda: b'telnetd: listening on ' in console())
                first, second = connect(50000), connect(50001)
                assert b'/dev/ttyp0\n' in line(first, b'tty')
                assert b'/dev/ttyp1\n' in line(second, b'tty')
                first.queue(b'\xff\xfa\x1f\0d\0(\xff\xf0')
                assert b'40 rows 100 columns' in line(first, b'stty -a')
                line(first, b'echo REMOTE_FILE > /remote-note')
                assert b'REMOTE_FILE\n' in line(second, b'cat /remote-note')
                at = len(received(first)); first.queue(b'sleep 30\r\n')
                until(lambda: b'sleep 30\n' in received(first)[at:])
                spin(.2)
                first.queue(b'\x1a')
                until(lambda: b'Stopped sleep 30' in received(first)[at:])
                at = len(received(first)); first.queue(b'fg\r\n')
                until(lambda: b'fg\n' in received(first)[at:])
                spin(.2)
                first.queue(b'\x03')
                until(lambda: b'fuzix# ' in received(first)[at:])
                assert b'INTERRUPTED:130\n' in line(first, b'echo INTERRUPTED:$?')
                assert b'OTHER_SESSION_OK\n' in line(second, b'echo OTHER_SESSION_OK')
                # An existing full-screen application must also work on a remote PTY.
                at = len(received(first)); first.queue(b'levee /network-edit\r\n')
                until(lambda: b'\x1b[' in received(first)[at:])
                spin(.2)
                first.queue(b'iSaved over native telnet.\x1b:wq\r\n')
                until(lambda: b'fuzix# ' in received(first)[at:])
                assert b'Saved over native telnet.' in line(second, b'cat /network-edit')
                at = len(received(first)); first.queue(b'telnet 192.168.64.1 2323\r\n')
                until(lambda: b'REMOTE_ECHO_READY\n' in received(first)[at:])
                first.queue(b'CLIENT_WIRE\r\n')
                until(lambda: b'CLIENT_WIRE_OK\n' in received(first)[at:])
                first.queue(b'\x1e')
                until(lambda: b'Connection closed.' in received(first)[at:]
                      and b'fuzix# ' in received(first)[at:])
                first.queue(b'exit\r\n'); until(lambda: first.closed)
                for index in range(6):
                    session = connect(50100 + index)
                    assert b'REUSED\n' in line(session, b'echo REUSED')
                    session.queue(b'exit\r\n'); until(lambda: session.closed)
                assert b'STILL_ALIVE\n' in line(second, b'echo STILL_ALIVE')
                # Lose the actual RNET worker, then reconnect the same MAC with
                # a fresh host socket. The kernel must invalidate old TCP/ARP
                # state and let new applications connect without a restart.
                peer.send(b'RNET\x03')
                until(lambda: b'network backend: network helper reported a link failure' in console())
                peer.streams.clear()
                peer.address = None
                until(lambda: b'network: Ethernet attachment restored' in console())
                send('htget http://fuse-x.test/binary /tmp/recovered; echo RECOVERED_HTTP:$?')
                until(lambda: b'RECOVERED_HTTP:0\n' in console())
                # telnetd intentionally exits when its blocking accept reports
                # ENETDOWN. Start a fresh service on the recovered interface.
                at = len(console())
                send('telnetd &')
                until(lambda: b'telnetd: listening on ' in console()[at:])
                recovered = connect(50200)
                assert b'RECOVERED_TELNET\n' in line(recovered, b'echo RECOVERED_TELNET')
                recovered.queue(b'exit\r\n'); until(lambda: recovered.closed)
                print('Native link failure, fresh HTTP and Telnet after automatic recovery passed', flush=True)
                send('shutdown')
                until(lambda: b'session stopped' in console())
                assert guest.wait(timeout=10) == 0, console()
                print('Native concurrent Telnet PTYs, jobs, editor, client and reconnect passed', flush=True)
            finally:
                if guest.poll() is None:
                    guest.kill(); guest.wait()
                peer.close()
                # Preserve useful failure context outside the temporary image.
                Path('/tmp/fuse-x-network-console.log').write_bytes(log.read_bytes())
        check_root(image)
        result = subprocess.run([str(executable), '--run', str(image)],
            input='cat /remote-note\ncat /network-edit\ncksum /tmp/download\nshutdown\n',
            text=True, capture_output=True, timeout=30,
            env=dict(environment, RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='25'))
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'REMOTE_FILE' in result.stdout and 'Saved over native telnet.' in result.stdout, result.stdout
        assert checksum.decode() + ' ' + str(len(BINARY)) + ' /tmp/download' in result.stdout, result.stdout
        check_root(image)
    record_runtime(build, ['ifconfig', 'ping', 'telnet', 'telnetd', 'host', 'nslookup',
                           'dig', 'echoping', 'ntpdate', 'htget'], 'network integration tested')
    print('Fuse-X: Ethernet networking, remote shells and offline persistence checks passed')


if __name__ == '__main__':
    main()
