#!/usr/bin/env python3
"""Real FUZIX sockets, ping and Telnet shells against an independent Ethernet peer."""
import argparse
from launch import require_mmu
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from network_peer import Peer
from launch import host_command

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('jit', 'interpreter', 'both'), default='jit')
p.add_argument('--backend', choices=('rvm',), default='rvm')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']


def vm_command(image, commands, jit, network, timeout):
    command = host_command(argparse.Namespace(build=build, backend=a.backend,
        vm=str(ROOT / 'target/release' / a.backend), vm_args=[
            '--net', str(network), '--headless', '--input', commands, '--timeout', str(timeout),
            *(['--jit'] if jit else [])]), image)
    return command
libs = subprocess.check_output([
    'make', '-s', '--no-print-directory', '-C', str(PORT), f'BUILD={build}',
    '-f', 'Makefile', '-f', '-', 'network-libs'
], input='network-libs:\n\t@echo $(LIBC_OBJS)\n', text=True).split()


with tempfile.TemporaryDirectory(prefix='fuzix-network-') as directory:
    tmp = Path(directory)
    env = os.environ.copy()
    cc = ['cc']
    if Path('/Applications/Xcode.app/Contents/Developer').is_dir():
        env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
        cc = ['xcrun', '--sdk', 'macosx', 'clang']
    codec = tmp / 'telnet-codec'
    subprocess.run([*cc, '-Wall', '-Wextra', '-Wno-unused-function',
                    str(PORT / 'tools/test_telnet_protocol.c'), '-o', str(codec)], env=env, check=True)
    subprocess.run([str(codec)], check=True)
    asm, obj, exe = [tmp / ('netcheck' + suffix) for suffix in ('.asm', '.pobj', '.rxe')]
    subprocess.run([
        str(ROOT / 'target/release/rcc'), 'compile', str(PORT / 'tools/netcheck.c'),
        '-o', str(asm), '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
        *[f'-I{path}' for path in (PORT / 'ripple/include', PORT / 'upstream/Library/include',
                                 ROOT / 'runtime/include', PORT / 'ripple')]
    ], check=True)
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rasm'), 'assemble', str(asm),
                    '-o', str(obj), '--bank-size', '64000', '--max-immediate', '65535'], check=True)
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rlink'),
                    str(build / 'user/entry.pobj'), str(obj), str(build / 'user/userlib.pobj'),
                    *libs, '-f', 'rxe', '--bank-size', '64000', '-o', str(exe)], check=True)
    names = ['init', 'sh', 'echo', 'cat', 'stty', 'tty', 'ttycheck', 'vi', 'levee',
             'ifconfig', 'ping', 'telnet', 'telnetd']
    for name in [*(name + '.rxe' for name in names), 'FUZIX.SYS', 'bios.bin']:
        (tmp / name).symlink_to(build / name)
    (tmp / 'apps.json').write_text(json.dumps([*names, 'netcheck']))
    for jit in engines:
        label = 'mmu-' + ('jit' if jit else 'interpreter')
        image = tmp / (label + '.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(tmp), str(image)], check=True)
        # /tmp keeps AF_UNIX paths under macOS's small path-length limit.
        with tempfile.TemporaryDirectory(prefix='fnet-', dir='/tmp') as sockets:
            path = Path(sockets) / 'nic'
            peer = Peer(path)
            log = Path('/tmp/fuzix-network-' + label + '.log')
            commands = ('netcheck\necho NET_STATUS:$?\nifconfig\necho IF_STATUS:$?\n'
                        'ping -c 1 192.168.64.1\necho PING_STATUS:$?\n'
                        'telnetd &\necho NETWORK_BOOT_OK\n')
            command = vm_command(image, commands, jit, path, 420)
            with log.open('w') as out:
                guest = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT)

                def until(condition, timeout=60):
                    deadline = time.monotonic() + timeout
                    while not condition():
                        assert guest.poll() is None, log.read_text()[-8000:]
                        assert time.monotonic() < deadline, ('network test timeout', log.read_text()[-4000:],
                            {str(k): bytes(v.received)[-5000:] for k, v in peer.streams.items()})
                        peer.pump()

                def spin(seconds):
                    end = time.monotonic() + seconds
                    while time.monotonic() < end:
                        peer.pump()

                def received(stream):
                    return bytes(stream.received).replace(b'\r', b'').replace(b'\0', b'')

                def line(stream, command):
                    at = len(received(stream))
                    stream.queue(command + b'\r\n')
                    until(lambda: b'fuzix# ' in received(stream)[at:])
                    return received(stream)[at:]

                def connect(local):
                    stream = peer.connect(23, local)
                    until(lambda: stream.established)
                    stream.queue(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f')
                    until(lambda: b'fuzix# ' in received(stream))
                    return stream

                try:
                    until(lambda: 'telnetd: listening on ' in log.read_text(), 100)
                    text = log.read_text().replace('\r', '')
                    assert 'NET_STATUS:0\n' in text and 'PING_STATUS:0\n' in text, text
                    assert 'IF_STATUS:0\n' in text and 'inet 192.168.64.250' in text, text
                    assert peer.ping_requests and peer.ping_replies, 'ICMP must work in both directions'
                    print('Socket ABI, UDP/TCP streams and bidirectional ping passed', flush=True)
                    first = connect(50000)
                    print('First remote shell connected', flush=True)
                    # Split subnegotiation across TCP segments and reject an unknown option.
                    first.chunk = 1
                    first.queue(b'\xff\xfa\x1f\0d\0(\xff\xf0\xff\xfd\x63')
                    until(lambda: not first.pending and not first.flight)
                    first.chunk = 256
                    assert b'40 rows 100 columns' in line(first, b'stty -a')
                    assert b'\xff\xfc\x63' in first.received
                    assert b'/dev/ttyp0\n' in line(first, b'tty')
                    line(first, b'echo REMOTE_FILE > /remote-note')
                    assert b'REMOTE_FILE\n' in line(first, b'cat /remote-note')
                    second = connect(50001)
                    assert b'/dev/ttyp1\n' in line(second, b'tty')
                    assert b'SECOND_OK\n' in line(second, b'echo SECOND_OK')
                    at = len(received(first))
                    first.queue(b'ttycheck --wait\r\n')
                    until(lambda: b'TTY_WAIT_READY\n' in received(first)[at:])
                    first.queue(b'\x03')
                    until(lambda: b'fuzix# ' in received(first)[at:])
                    assert b'INTERRUPTED:130\n' in line(first, b'echo INTERRUPTED:$?')
                    assert b'STILL_ALIVE\n' in line(second, b'echo STILL_ALIVE')
                    print('Concurrent PTYs, Telnet negotiation and Ctrl-C passed', flush=True)
                    # Levee, stop/continue, redraw and save over the remote PTY.
                    at = len(received(first));first.queue(b'vi /network-edit\r\n')
                    until(lambda: b'\x1b[?1049h' in received(first)[at:])
                    spin(0.3);first.queue(b'iSaved over telnet.\x1b');spin(0.3)
                    first.queue(b'\x1a')
                    until(lambda: b'fuzix# ' in received(first)[at:])
                    assert b'Stopped vi /network-edit' in received(first)[at:]
                    at = len(received(first));first.queue(b'fg\r\n')
                    until(lambda: b'\x1b[?1049h' in received(first)[at:])
                    first.queue(b':wq\r\n')
                    until(lambda: b'fuzix# ' in received(first)[at:])
                    assert b'Saved over telnet.' in line(second, b'cat /network-edit')
                    print('Remote Levee suspend/resume and save passed', flush=True)
                    # The guest telnet client opens a second TCP stream through the same NIC.
                    at = len(received(first));first.queue(b'telnet 192.168.64.1 2323\r\n')
                    until(lambda: b'REMOTE_ECHO_READY\n' in received(first)[at:])
                    first.queue(b'CLIENT_WIRE\r\n')
                    until(lambda: b'CLIENT_WIRE_OK\n' in received(first)[at:])
                    first.queue(b'\x1e')
                    until(lambda: b'Connection closed.' in received(first)[at:] and b'fuzix# ' in received(first)[at:])
                    print('Guest Telnet client passed', flush=True)
                    # Exit/reconnect beyond the passive pool capacity, retaining old tuples.
                    first.queue(b'exit\r\n');until(lambda: first.closed)
                    for i in range(6):
                        spin(0.2)
                        session = connect(50100 + i)
                        assert b'REUSED\n' in line(session, b'echo REUSED')
                        session.queue(b'exit\r\n');until(lambda: session.closed)
                    # Ignore oversized subnegotiation without corrupting the following input.
                    second.queue(b'\xff\xfa\x1f' + b'x' * 100 + b'\xff\xf0')
                    assert b'BOUNDED_OK\n' in line(second, b'echo BOUNDED_OK')
                    second.queue(b'shutdown\r\n')
                    until(lambda: 'disk synced; power off.' in log.read_text())
                    assert guest.wait(timeout=10) == 0
                finally:
                    Path('/tmp/fuzix-network-' + label + '-streams.json').write_text(json.dumps(
                        {str(k): {'received': bytes(v.received).decode('latin1'), 'closed': v.closed,
                                  'reset': v.reset} for k, v in peer.streams.items()}, indent=2))
                    if guest.poll() is None:
                        guest.terminate()
                        try:
                            guest.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            guest.kill();guest.wait()
                    peer.close()
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        # Files written from the remote shell survive a completely offline reboot.
        result = subprocess.run(vm_command(image, 'cat /remote-note\ncat /network-edit\nshutdown\n',
            jit, 'off', 90),
            capture_output=True, text=True, timeout=100)
        assert result.returncode == 0 and 'REMOTE_FILE' in result.stdout and 'Saved over telnet.' in result.stdout, result.stdout
        assert 'disk synced; power off.' in result.stdout, result.stdout
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(f'FUZIX sockets, bidirectional ping, Telnet client/server, remote jobs/editor and persistence passed (MMU=1, JIT={jit})')
