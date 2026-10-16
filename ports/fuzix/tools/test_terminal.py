#!/usr/bin/env python3
"""Exercise live terminal input, Ctrl keys, and disk locking through the launcher."""
import argparse
import errno
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=PORT/'build')
args = parser.parse_args()
build = args.build.resolve()
env = dict(os.environ, FUZIX_BUILD=str(build))

with tempfile.TemporaryDirectory(prefix='ripple-fuzix-terminal-') as temporary:
    disk = Path(temporary)/'fuzix.img'
    subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(disk)], check=True)
    master, slave = pty.openpty()
    command = [str(ROOT/'scripts/run-fuzix.sh'), '--run-only', '--disk', str(disk)]
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    buffer = b''

    def until(token, timeout=25):
        global buffer
        deadline = time.monotonic()+timeout
        while token not in buffer:
            if time.monotonic()>deadline:
                raise AssertionError(buffer.decode(errors='replace'))
            if select.select([master], [], [], 0.2)[0]:
                try:
                    buffer += os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        raise AssertionError(buffer.decode(errors='replace')) from error
                    raise
        end = buffer.index(token)+len(token)
        result, buffer = buffer[:end], buffer[end:]
        return result

    try:
        until(b'fuzix# ')
        busy = subprocess.run(command+['--headless', '--input', 'shutdown\n'], cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
        assert busy.returncode != 0 and 'already in use' in busy.stdout, busy.stdout
        for i in range(12):
            text = f'terminal_sequence_{i}_abcdefghijklmnopqrstuvwxyz_0123456789'
            # Alternate Enter (CR) with Ctrl-J (LF). Both must reach the guest
            # as a newline, with no pop/enqueue race against timer IRQ input.
            end = b'\r' if i % 2 else b'\n'
            os.write(master, f'echo {text} > /tty-note'.encode()+end)
            until(b'fuzix# ')
            os.write(master, b'cat /tty-note'+end)
            output = until(b'fuzix# ')
            assert text.encode() in output, output
        os.write(master, b'nonsense\x15echo control_u_works\r')
        output = until(b'fuzix# ')
        assert b'control_u_works' in output and b'error ' not in output, output
        os.write(master, b'hello terminal-argument\r')
        output = until(b'fuzix# ')
        assert b'argument: terminal-argument' in output, output
        # Ctrl-D at an empty prompt is a real guest EOF and clean shutdown.
        os.write(master, b'\x04')
        until(b'disk synced; power off.')
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(disk)], check=True)
print('Live terminal passed: ordered input, Enter/Ctrl-J, Ctrl-U, Ctrl-D, exec, run-only launcher and exclusive disk lock')
