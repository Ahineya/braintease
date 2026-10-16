#!/usr/bin/env python3
"""Exercise live pager controls, pipes, signals and terminal restoration."""
import argparse
import errno
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='both')
a = p.parse_args()
build = a.build.resolve()


def exercise(image, jit):
    master, slave = pty.openpty()
    command = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--timeout', '600']
    if not jit:
        command.append('--no-jit')
    process = subprocess.Popen(command, env=dict(os.environ, FUZIX_BUILD=str(build)),
                               stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    pending = b''
    transcript = bytearray()

    def until(token, timeout=60):
        nonlocal pending
        deadline = time.monotonic() + timeout
        while token not in pending:
            if time.monotonic() > deadline:
                raise AssertionError((token, pending[-5000:]))
            if select.select([master], [], [], 0.1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as e:
                    if e.errno == errno.EIO:
                        raise AssertionError(pending[-5000:]) from e
                    raise
                pending += data
                transcript.extend(data)
        at = pending.index(token) + len(token)
        result, pending = pending[:at], pending[at:]
        return result.replace(b'\r', b'')

    def send(data):
        os.write(master, data)

    def shell(text):
        send(text.encode() + b'\r')
        return until(b'fuzix# ')

    def less(command):
        send(command.encode() + b'\r')
        until(b'\x1b[?1049h')
        return until(b'-- less:')

    def leave_less(key):
        send(key)
        until(b'\x1b[?1049l')
        return until(b'fuzix# ')

    try:
        until(b'fuzix# ')
        shell('stty -g > /tty-before')
        first = less('env LINES=6 COLUMNS=40 less /pager-lines')
        assert b'LINE01' in first and b'LINE05' in first and b'LINE06' not in first, first
        send(b' ')
        page = until(b'-- less:')
        assert b'LINE06' in page and b'LINE10' in page, page
        send(b'b')
        assert b'LINE01' in until(b'-- less:')
        send(b'j')
        assert b'LINE02' in until(b'-- less:')
        send(b'g')
        assert b'LINE01' in until(b'-- less:')
        leave_less(b'q')
        short = less('less /pager-short')
        assert b'LAST_WITHOUT_NEWLINE' in short, short
        leave_less(b'q')
        less('less /pager-lines')
        leave_less(b'\x03')
        shell('stty -g > /tty-after')
        assert b'TTY_RESTORED:0' in shell('cmp /tty-before /tty-after; echo TTY_RESTORED:$?')
        piped = less('cat /pager-lines | env LINES=6 less')
        assert b'LINE01' in piped, piped
        leave_less(b'q')
        live = less('yes | env LINES=6 less')
        assert b'y\n' in live, live
        leave_less(b'q')
        assert b'PAGER_MISSING:1' in shell('less /missing; echo PAGER_MISSING:$?')
        # more must accept a single key without Enter, including piped input.
        send(b'env LINES=6 more /pager-lines\r')
        page = until(b'--More--')
        assert b'LINE01' in page and b'LINE05' in page, page
        send(b'\r')
        assert b'LINE06' in until(b'--More--')
        send(b'q')
        until(b'fuzix# ')
        send(b'cat /pager-lines | env LINES=6 more\r')
        until(b'--More--')
        send(b'\x03')
        until(b'fuzix# ')
        shell('stty -g > /tty-after')
        assert b'TTY_RESTORED:0' in shell('cmp /tty-before /tty-after; echo TTY_RESTORED:$?')
        send(b'garbage\x15echo restored\r')
        assert b'restored\n' in until(b'fuzix# ')
        shell('echo pager_session > /pager-session')
        send(b'shutdown\r')
        until(b'Ripple: disk synced; power off.')
        assert process.wait(timeout=10) == 0
    finally:
        Path('/tmp/fuzix-pagers-' + ('jit' if jit else 'interpreter') + '.log').write_bytes(transcript)
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


with tempfile.TemporaryDirectory(prefix='fuzix-pagers-') as temporary:
    image = Path(temporary) / 'disk.img'
    subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
    files = [{'name': '/pager-lines', 'data': list((''.join('LINE%02d\n' % i for i in range(1, 61))).encode())},
             {'name': '/pager-short', 'data': list(b'LAST_WITHOUT_NEWLINE')}]
    seed = image.with_suffix('.json')
    seed.write_text(json.dumps(files))
    subprocess.run(['node', '--input-type=module', '-e', """
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const disk = fs.readFileSync(process.argv[1]), root = fatFile(disk, 'ROOTFS.IMG');
fs.writeFileSync(process.argv[1], root.replace(installRoot(root.data, JSON.parse(fs.readFileSync(process.argv[2])))));
""", str(image), str(seed)], cwd=ROOT, check=True)
    for jit in ((True, False) if a.engine == 'both' else (a.engine == 'jit',)):
        exercise(image, jit)
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(('JIT' if jit else 'Interpreter') + ': live less/more, pipes, Ctrl-C and terminal recovery passed', flush=True)
