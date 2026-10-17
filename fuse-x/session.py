#!/usr/bin/env python3
"""Own a persistent native kernel and attach one host console at a time."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import tty

REPLAY = 65536
BACKPRESSURE = 1024 * 1024
LOG_LIMIT = 8 * 1024 * 1024


def connect(state, request):
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(2)
    try:
        connection.connect(str(state / 'console.sock'))
        connection.sendall(request)
        return connection
    except BaseException:
        connection.close()
        raise


def running(state):
    try:
        with connect(state, b'Q') as connection:
            return connection.recv(256).startswith(b'OK')
    except OSError:
        return False


def serve(state, command):
    os.umask(0o077)
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Never unlink another live server's endpoint, including during startup.
    with (state / 'owner.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit('Fuse-X session is already running; use --attach')
        endpoint = state / 'console.sock'
        endpoint.unlink(missing_ok=True)
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(endpoint))
        endpoint.chmod(0o600)
        server.listen(8)
        server.setblocking(False)
        master, slave = pty.openpty()
        tty.setraw(slave)

        def terminal():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        environment = dict(os.environ, RCC_NATIVE_NET_EAGER='1')
        child = None
        clients = {}
        requests = {}
        attached = None
        outgoing = bytearray()
        incoming = bytearray()
        replay = bytearray()
        previous_cr = False
        log = (state / 'console.log').open('wb', buffering=0)
        log_bytes = 0
        (state / 'exit.json').unlink(missing_ok=True)

        def stop(_number, _frame):
            if child is not None and child.poll() is None:
                child.terminate()

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)

        def detach():
            nonlocal attached
            if attached is not None:
                attached.close()
                attached = None
            outgoing.clear()

        def output(data):
            nonlocal log, log_bytes
            if log_bytes + len(data) > LOG_LIMIT:
                log.close()
                os.replace(state / 'console.log', state / 'console.log.1')
                log = (state / 'console.log').open('wb', buffering=0)
                log_bytes = 0
            log.write(data)
            log_bytes += len(data)
            replay.extend(data)
            del replay[:-REPLAY]
            if attached is not None:
                outgoing.extend(data)
                if len(outgoing) > BACKPRESSURE:
                    detach()

        try:
            child = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=subprocess.PIPE,
                                     env=environment, preexec_fn=terminal)
            os.close(slave)
            slave = None
            os.set_blocking(master, False)
            os.set_blocking(child.stderr.fileno(), False)
            readers = {master, child.stderr.fileno()}
            while readers or child.poll() is None:
                readable = [server, *readers, *clients]
                if attached is not None and len(incoming) < REPLAY:
                    readable.append(attached)
                writable = ([master] if incoming and master in readers else [])
                if attached is not None and outgoing:
                    writable.append(attached)
                ready, writes, _ = select.select(readable, writable, [], .1)
                for descriptor in ready:
                    if descriptor is server:
                        connection, _ = server.accept()
                        connection.setblocking(False)
                        if len(clients) >= 8:
                            connection.close()
                        else:
                            clients[connection] = time.monotonic()
                            requests[connection] = bytearray()
                    elif descriptor in clients:
                        try:
                            buffer = requests[descriptor]
                            chunk = descriptor.recv(1 if not buffer else 5 - len(buffer))
                            if not chunk:
                                descriptor.close(); del clients[descriptor]; del requests[descriptor]
                                continue
                            buffer.extend(chunk)
                            request = bytes(buffer[:1])
                            if request == b'W' and len(buffer) < 5:
                                continue
                            if request == b'A':
                                if attached is not None:
                                    descriptor.send(b'ERROR console already attached\n')
                                    descriptor.close()
                                else:
                                    attached = descriptor
                                    outgoing.extend(b'OK\n' + replay)
                            elif request == b'W':
                                rows, columns = struct.unpack('!HH', buffer[1:5])
                                if rows and columns:
                                    fcntl.ioctl(master, termios.TIOCSWINSZ,
                                                struct.pack('HHHH', rows, columns, 0, 0))
                                    descriptor.send(b'OK\n')
                                else:
                                    descriptor.send(b'ERROR invalid terminal size\n')
                                descriptor.close()
                            else:
                                if request == b'S':
                                    stop(None, None)
                                descriptor.send(b'OK\n' if request in (b'Q', b'S') else b'ERROR request\n')
                                descriptor.close()
                        except OSError:
                            descriptor.close()
                        del clients[descriptor]
                        del requests[descriptor]
                    elif descriptor is attached:
                        try:
                            data = attached.recv(4096)
                            if data:
                                incoming.extend(data)
                            else:
                                detach()
                        except OSError:
                            detach()
                    else:
                        try:
                            data = os.read(descriptor, 4096)
                        except OSError as error:
                            if error.errno not in (errno.EIO, errno.EBADF):
                                raise
                            data = b''
                        if not data:
                            readers.discard(descriptor)
                        elif descriptor == master:
                            output(data)
                        else:
                            # Host diagnostics are LF; guest tty output is already CRLF.
                            converted = bytearray()
                            for byte in data:
                                if byte == 10 and not previous_cr:
                                    converted.append(13)
                                converted.append(byte)
                                previous_cr = byte == 13
                            output(converted)
                for descriptor in writes:
                    try:
                        if descriptor == master:
                            count = os.write(master, incoming)
                            del incoming[:count]
                        elif descriptor is attached:
                            count = attached.send(outgoing)
                            del outgoing[:count]
                    except BlockingIOError:
                        pass
                    except OSError:
                        if descriptor is attached:
                            detach()
                for connection, started in list(clients.items()):
                    if time.monotonic() - started > 2:
                        connection.close()
                        del clients[connection]
                        del requests[connection]
            result = child.wait()
            (state / 'exit.json').write_text(json.dumps({'status': result}) + '\n')
            # Flush the last shutdown message before closing the client stream.
            if attached is not None:
                attached.settimeout(2)
                try:
                    attached.sendall(outgoing)
                except OSError:
                    pass
            return result
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    print('Fuse-X did not finish orderly shutdown; see console.log', file=sys.stderr)
            detach()
            for connection in clients:
                connection.close()
            server.close()
            endpoint.unlink(missing_ok=True)
            os.close(master)
            if slave is not None:
                os.close(slave)
            log.close()


def attach(state, *, boot_only=False):
    connection = connect(state, b'A')
    saved = None
    descriptor = sys.stdin.fileno()
    interactive = os.isatty(descriptor)
    boot_output = bytearray()
    last_size = None
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        reply = bytearray()
        while not reply.endswith(b'\n'):
            part = connection.recv(1)
            if not part:
                raise SystemExit('Fuse-X console disconnected before attachment')
            reply.extend(part)
        if reply != b'OK\n':
            raise SystemExit(reply.decode(errors='replace').strip())
        connection.settimeout(None)
        if interactive:
            print('Fuse-X: Ctrl-] detaches the console; --attach reconnects.', file=sys.stderr)
            saved = termios.tcgetattr(descriptor)
            tty.setraw(descriptor)
        inputs = [connection, descriptor]
        while True:
            if interactive:
                try:
                    columns, rows = os.get_terminal_size(descriptor)
                    if rows and columns and (rows, columns) != last_size:
                        with connect(state, b'W' + struct.pack('!HH', rows, columns)) as control:
                            control.recv(256)
                        last_size = rows, columns
                except OSError:
                    # Drain the attached stream even if the kernel just exited,
                    # or an older broker cannot accept a size update.
                    pass
            ready, _, _ = select.select(inputs, [], [], .1 if interactive else None)
            if connection in ready:
                data = connection.recv(8192)
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if boot_only:
                    boot_output.extend(data)
                    if b'Fuse-X: native /init running as PID 1' in boot_output:
                        return 0
                    del boot_output[:-256]
            if descriptor in ready:
                data = os.read(descriptor, 4096)
                if not data:
                    inputs.remove(descriptor)
                elif interactive and b'\x1d' in data:
                    before = data.split(b'\x1d', 1)[0]
                    if before:
                        connection.sendall(before)
                    return 0
                else:
                    connection.sendall(data)
        try:
            return json.loads((state / 'exit.json').read_text())['status']
        except (OSError, ValueError):
            return 1
    finally:
        connection.close()
        if saved is not None:
            termios.tcsetattr(descriptor, termios.TCSANOW, saved)
            print('\r\nFuse-X: console detached.', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['start', 'serve', 'attach', 'stop', 'status'])
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--detach', action='store_true')
    arguments = sys.argv[1:]
    separator = arguments.index('--') if '--' in arguments else len(arguments)
    args = parser.parse_args(arguments[:separator])
    args.command = arguments[separator + 1:]
    state = args.state.resolve()
    if len(os.fsencode(str(state / 'console.sock'))) >= 104:
        parser.error('session socket path is too long; select a shorter FUSE_X_BUILD path')
    if args.action == 'status':
        return 0 if running(state) else 1
    if args.action == 'serve':
        command = args.command[1:] if args.command[:1] == ['--'] else args.command
        return serve(state, command)
    if args.action == 'attach':
        return attach(state)
    if args.action == 'stop':
        with connect(state, b'S') as connection:
            assert connection.recv(256).startswith(b'OK')
        deadline = time.monotonic() + 15
        while running(state):
            if time.monotonic() > deadline:
                raise SystemExit('Fuse-X is still stopping; see ' + str(state / 'console.log'))
            time.sleep(.1)
        return json.loads((state / 'exit.json').read_text())['status']
    if running(state):
        parser.error('Fuse-X is already running; use --attach or --stop')
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('start requires a native kernel command after --')
    with (state / 'supervisor.log').open('ab', buffering=0) as log:
        server = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                   'serve', '--state', str(state), '--', *command],
                                  stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                  start_new_session=True)
    deadline = time.monotonic() + 10
    while not running(state):
        if server.poll() is not None or time.monotonic() > deadline:
            raise SystemExit('Fuse-X session did not start; see ' + str(state / 'supervisor.log'))
        time.sleep(.05)
    if args.detach:
        network = os.environ.get('RCC_NATIVE_NET', os.environ.get('RIPPLE_NET', 'shared'))
        if sys.platform == 'darwin' and network in ('shared', 'host', 'auto'):
            if sys.stdin.isatty():
                # sudo authorizes on the kernel's controlling PTY. Bridge that
                # prompt before leaving a remote-only session in the background.
                result = attach(state, boot_only=True)
                if result:
                    return result
            else:
                print('Fuse-X: use --attach if macOS network authorization is requested.')
        print('Fuse-X: running in background; use --attach or --stop.')
        print('Console log: ' + str(state / 'console.log'))
        return 0
    return attach(state)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, KeyboardInterrupt) as error:
        print('Fuse-X: ' + str(error), file=sys.stderr)
        sys.exit(1)
