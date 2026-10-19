#!/usr/bin/env python3
"""A slow console must apply backpressure without losing input or attachment."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from session import BACKPRESSURE, connect, running

HERE = Path(__file__).resolve().parent
PRODUCER = r'''
import os, pathlib, signal, sys, threading, time
work = pathlib.Path(sys.argv[1])
def stop(*_):
    os.write(1, b'ORDERLY SHUTDOWN')
    os._exit(0)
signal.signal(signal.SIGTERM, stop)
while not (work / 'go').exists():
    time.sleep(.01)
def produce():
    block = bytes(range(256)) * 128
    for _ in range(256):
        view = memoryview(block)
        while view:
            view = view[os.write(1, view):]
thread = threading.Thread(target=produce)
thread.start()
assert os.read(0, 1) == b'\x03'
(work / 'input').touch()
thread.join()
os.write(1, b'COMPLETE')
'''


def until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, 'console broker timed out'
        time.sleep(.01)


def exercise(stop_while_blocked):
    with tempfile.TemporaryDirectory(prefix='fx-broker-', dir='/tmp') as temporary:
        work = Path(temporary)
        state = work / 'session'
        with (work / 'broker.log').open('wb') as log:
            broker = subprocess.Popen([sys.executable, str(HERE / 'session.py'),
                                       'serve', '--state', str(state), '--',
                                       sys.executable, '-c', PRODUCER, str(work)],
                                      stdout=log, stderr=log)
            connection = None
            try:
                until(lambda: running(state))
                connection = connect(state, b'A')
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
                assert connection.recv(3) == b'OK\n'
                (work / 'go').touch()
                until(lambda: (state / 'console.log').stat().st_size >= BACKPRESSURE)
                # Stop draining long enough for the old broker to drop us.
                time.sleep(.3)
                assert running(state), 'output blocked control requests'
                with connect(state, b'A') as second:
                    assert second.recv(256).startswith(b'ERROR console already attached'), \
                        'a slow reader was forcibly detached'
                if stop_while_blocked:
                    if stop_while_blocked == 'signal':
                        broker.terminate()
                    else:
                        with connect(state, b'S') as control:
                            assert control.recv(256) == b'OK\n'
                    assert broker.wait(timeout=5) == 0
                    reason = 'host SIGTERM' if stop_while_blocked == 'signal' else 'console stop request (--stop)'
                    assert reason in (work / 'broker.log').read_text()
                else:
                    connection.sendall(b'\x03')
                    until(lambda: (work / 'input').exists())
                    connection.settimeout(5)
                    received = bytearray()
                    while True:
                        block = connection.recv(65536)
                        if not block:
                            break
                        received.extend(block)
                    assert bytes(received) == bytes(range(256)) * 32768 + b'COMPLETE', \
                        f'console output was lost or changed ({len(received)} bytes)'
                    assert broker.wait(timeout=5) == 0
            finally:
                if connection is not None:
                    connection.close()
                if broker.poll() is None:
                    broker.terminate()
                    broker.wait(timeout=5)


def previous_console():
    with tempfile.TemporaryDirectory(prefix='fx-console-history-', dir='/tmp') as temporary:
        work = Path(temporary)
        state = work / 'session'
        state.mkdir()
        (state / 'console.log').write_bytes(b'previous session: Halted.\n')
        (state / 'console.log.1').write_bytes(b'older rotation')
        with (work / 'broker.log').open('wb') as log:
            broker = subprocess.Popen([sys.executable, str(HERE / 'session.py'),
                                       'serve', '--state', str(state), '--', sys.executable,
                                       '-c', 'print("fresh session")'], stdout=log, stderr=log)
            assert broker.wait(timeout=5) == 0
        assert (state / 'console.log.1').read_bytes() == b'previous session: Halted.\n'
        assert b'fresh session' in (state / 'console.log').read_bytes()
        events = (work / 'broker.log').read_text()
        assert 'kernel started pid=' in events and 'status=0; broker_stop_requested=False' in events
        assert sorted(p.name for p in state.glob('console.log*')) == ['console.log', 'console.log.1']


def guest_random_output(executable):
    with tempfile.TemporaryDirectory(prefix='fx-random-', dir='/tmp') as temporary:
        work = Path(temporary)
        image, state = work / 'root.img', work / 'session'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image),
                        '--apps', str(executable.parent / 'apps')], check=True)
        with (work / 'broker.log').open('wb') as log:
            broker = subprocess.Popen([sys.executable, str(HERE / 'session.py'),
                                       'serve', '--state', str(state), '--',
                                       str(executable), '--run', str(image)],
                                      stdout=log, stderr=log,
                                      env=dict(os.environ, RCC_NATIVE_NET='off',
                                               RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT='40'))
            connection = None
            try:
                until(lambda: running(state))
                connection = connect(state, b'A')
                connection.settimeout(15)
                def prompt():
                    received = bytearray()
                    while b'fuse-x# ' not in received:
                        block = connection.recv(65536)
                        assert block, 'guest console detached during random output'
                        received.extend(block)
                        assert len(received) < 16 * BACKPRESSURE, 'Ctrl-C did not stop cat'
                prompt()
                connection.sendall(b'cat /dev/urandom\n')
                until(lambda: (state / 'console.log').stat().st_size >= BACKPRESSURE, 15)
                time.sleep(.3)
                with connect(state, b'A') as duplicate:
                    assert duplicate.recv(256).startswith(b'ERROR console already attached')
                connection.sendall(b'\x03')
                prompt()
                connection.sendall(b'echo RANDOM_OUTPUT_INTERRUPTED\nshutdown\n')
                while connection.recv(65536):
                    pass
                assert broker.wait(timeout=5) == 0
                assert b'RANDOM_OUTPUT_INTERRUPTED\r\n' in (state / 'console.log').read_bytes()
                from test_support import check_root
                check_root(image)
            finally:
                if connection is not None:
                    connection.close()
                if broker.poll() is None:
                    broker.terminate()
                    broker.wait(timeout=10)
    print('Fuse-X: real guest cat /dev/urandom stays attached, Ctrl-C returns to shell, clean shutdown')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path)
    args = parser.parse_args()
    exercise(False)
    exercise(True)
    exercise('signal')
    previous_console()
    print('Fuse-X: slow console preserves attachment, binary output, input, status and stop')
    if args.executable:
        guest_random_output(args.executable.resolve())
