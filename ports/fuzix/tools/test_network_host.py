#!/usr/bin/env python3
"""Opt-in macOS vmnet integration, using a private disk and real host TCP/IP.

Requires the existing vmnet helper's sudo authorization. Does not use the
internet or the normal persistent image. Do not run alongside another guest
using the default 192.168.64.250 address.
"""
import argparse
from launch import require_mmu
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)
memory = '16384000'
with tempfile.TemporaryDirectory(prefix='fuzix-host-net-') as directory:
    tmp = Path(directory)
    names = ['init', 'sh', 'echo', 'cat', 'stty', 'ifconfig', 'ping', 'telnetd']
    for name in [*(n + '.rxe' for n in names), 'FUZIX.SYS', 'bios.bin']:
        (tmp / name).symlink_to(build / name)
    (tmp / 'apps.json').write_text(json.dumps(names))
    image = tmp / 'host.img'
    subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(tmp), str(image)], check=True)
    log = Path('/tmp/fuzix-network-host.log')
    command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
               '--memory', memory, '--net', 'host', '--net-eager', '--jit', '--headless',
               '--input', 'ifconfig\nping -c 1 192.168.64.1\necho PING:$?\ntelnetd &\n',
               '--timeout', '180']
    with log.open('w') as out:
        guest = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT)
        connection = None
        try:
            deadline = time.monotonic() + 90
            while 'telnetd: listening on ' not in log.read_text():
                assert guest.poll() is None and time.monotonic() < deadline, log.read_text()
                time.sleep(0.1)
            assert 'PING:0\n' in log.read_text().replace('\r', ''), log.read_text()
            subprocess.run(['/sbin/ping', '-c', '1', '-W', '3000', '192.168.64.250'],
                           timeout=15, check=True)
            connection = socket.create_connection(('192.168.64.250', 23), timeout=15)
            connection.settimeout(0.1)
            connection.sendall(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f'
                               b'\xff\xfa\x1f\0d\0(\xff\xf0')
            transcript = bytearray()

            def until(token):
                stop = time.monotonic() + 30
                while token not in transcript:
                    assert guest.poll() is None and time.monotonic() < stop, (log.read_text(), transcript)
                    try:
                        data = connection.recv(65536)
                        assert data, transcript
                        transcript.extend(data.replace(b'\r', b''))
                    except socket.timeout:
                        pass

            until(b'riplix# ')
            transcript.clear();connection.sendall(b'stty -a\r\n')
            until(b'40 rows 100 columns')
            until(b'riplix# ')
            transcript.clear();connection.sendall(b'echo HOST_TCP_OK > /host-note\r\n')
            until(b'riplix# ')
            transcript.clear();connection.sendall(b'cat /host-note\r\n')
            until(b'HOST_TCP_OK\n')
            until(b'riplix# ')
            connection.sendall(b'shutdown\r\n')
            assert guest.wait(timeout=30) == 0, log.read_text()
            assert 'disk synced; power off.' in log.read_text(), log.read_text()
        finally:
            if connection is not None:
                connection.close()
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    guest.kill();guest.wait()
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
    reboot = subprocess.run([str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
        '--disk', str(image), '--memory', memory, '--net', 'off', '--headless', '--jit',
        '--input', 'cat /host-note\nshutdown\n', '--timeout', '60'],
        capture_output=True, text=True, timeout=70)
    assert reboot.returncode == 0 and 'HOST_TCP_OK\n' in reboot.stdout.replace('\r', ''), reboot.stdout
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
    print('Real host ping in both directions, TCP Telnet shell, window sizing and persistence passed')
