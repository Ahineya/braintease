#!/usr/bin/env python3
"""Opt-in macOS vmnet test: host ping and TCP Telnet to a native FUZIX kernel.

The existing vmnet helper needs sudo authorization. Uses a temporary guest disk,
no internet service, and no persistent root image. Do not run another guest at
192.168.64.250 on the same network during this check.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from test_support import check_root

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='host',
                       RCC_NATIVE_NET_EAGER='1', RCC_NATIVE_TIMEOUT='100')
    with tempfile.TemporaryDirectory(prefix='fuse-x-host-net-') as temporary:
        directory = Path(temporary)
        image = directory / 'root.img'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image),
                        '--apps', str(executable.parent / 'apps')], check=True)
        log = directory / 'console.log'
        connections = []
        with log.open('wb') as output:
            guest = subprocess.Popen([str(executable), '--run', str(image)], stdin=subprocess.PIPE,
                                     stdout=output, stderr=subprocess.STDOUT, env=environment)
            try:
                guest.stdin.write(b'ifconfig\nping -c 1 192.168.64.1\necho HOST_PING:$?\ntelnetd &\n')
                guest.stdin.flush()
                deadline = time.monotonic() + 35
                while b'telnetd: listening on ' not in log.read_bytes():
                    assert guest.poll() is None and time.monotonic() < deadline, log.read_text()
                    time.sleep(.1)
                assert b'HOST_PING:0\r\n' in log.read_bytes(), log.read_text()
                subprocess.run(['/sbin/ping', '-c', '1', '-W', '3000', '192.168.64.250'],
                               timeout=10, check=True)

                def until(connection, token):
                    received = bytearray()
                    deadline = time.monotonic() + 15
                    while token not in received:
                        assert guest.poll() is None and time.monotonic() < deadline, (log.read_text(), received)
                        try:
                            data = connection.recv(65536)
                            assert data, received
                            received.extend(data.replace(b'\r', b''))
                        except socket.timeout:
                            pass
                    return received

                def connect():
                    connection = socket.create_connection(('192.168.64.250', 23), timeout=10)
                    connections.append(connection)
                    connection.settimeout(.1)
                    connection.sendall(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f'
                                       b'\xff\xfa\x1f\0d\0(\xff\xf0')
                    until(connection, b'fuzix# ')
                    return connection

                first, second = connect(), connect()
                first.sendall(b'echo HOST_NATIVE_TCP > /host-note\r\n')
                until(first, b'fuzix# ')
                second.sendall(b'cat /host-note\r\n')
                assert b'HOST_NATIVE_TCP\n' in until(second, b'fuzix# ')
                first.sendall(b'exit\r\n')
                first.close()
                third = connect()
                third.sendall(b'stty -a\r\n')
                assert b'40 rows 100 columns' in until(third, b'fuzix# ')
                second.sendall(b'echo OTHER_HOST_SESSION\r\n')
                assert b'OTHER_HOST_SESSION\n' in until(second, b'fuzix# ')
                guest.stdin.write(b'shutdown\n'); guest.stdin.flush()
                assert guest.wait(timeout=15) == 0, log.read_text()
                assert b'session stopped' in log.read_bytes(), log.read_text()
            finally:
                for connection in connections:
                    connection.close()
                if guest.poll() is None:
                    guest.kill(); guest.wait()
                Path('/tmp/fuse-x-network-host-console.log').write_bytes(log.read_bytes())
        check_root(image)
        result = subprocess.run([str(executable), '--run', str(image)],
            input='cat /host-note\nshutdown\n', text=True, capture_output=True, timeout=20,
            env=dict(environment, RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='15'))
        assert result.returncode == 0 and 'HOST_NATIVE_TCP' in result.stdout, result.stdout + result.stderr
    print('Fuse-X: real host ping, concurrent TCP Telnet, reconnect, sizing and persistence passed')


if __name__ == '__main__':
    main()
