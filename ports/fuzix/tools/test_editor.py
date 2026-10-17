#!/usr/bin/env python3
"""Edit real files through VILE's live terminal with the selected RVM engine(s)."""
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
parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
parser.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
args = parser.parse_args()
engines = [True, False] if args.engine == 'both' else [args.engine == 'jit']
engine_label = 'JIT and interpreter' if args.engine == 'both' else args.engine
build = args.build.resolve()


def exercise(image, jit):
    master, slave = pty.openpty()
    command = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk',
               str(image), '--timeout', '150']
    if not jit:
        command.append('--no-jit')
    env = dict(os.environ, FUZIX_BUILD=str(build))
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=slave,
                               stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    buffer = b''
    transcript = bytearray()

    def until(token, timeout=35):
        nonlocal buffer
        deadline = time.monotonic() + timeout
        while token not in buffer:
            if time.monotonic() > deadline:
                raise AssertionError(buffer[-3000:].decode(errors='replace'))
            if select.select([master], [], [], 0.2)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        raise AssertionError(buffer[-3000:].decode(errors='replace')) from error
                    raise
                if not data:
                    raise AssertionError('terminal closed before ' + repr(token))
                buffer += data
                transcript.extend(data)
        end = buffer.index(token) + len(token)
        result, buffer = buffer[:end], buffer[end:]
        return result.replace(b'\r', b'')

    def send(data):
        os.write(master, data)

    def shell(text):
        send(text.encode() + b'\r')
        return until(b'fuzix# ')

    def contents(path):
        command = 'cat ' + path
        output = shell(command)
        prefix = command.encode() + b'\n'
        assert output.startswith(prefix) and output.endswith(b'fuzix# '), output
        return output[len(prefix):-len(b'fuzix# ')]

    def enter(text):
        print(('JIT' if jit else 'Interpreter') + ': ' + text, flush=True)
        send(text.encode() + b'\r')
        until(b'\x1b[?1049h')
        until(b'\x1b[1;1H')

    def leave(keys):
        send(keys)
        until(b'\x1b[?1049l')
        return until(b'fuzix# ')

    try:
        until(b'fuzix# ')
        if not jit:
            assert contents('/edit-note') == b'Yello Ripple\n'
            assert contents('/named') == b'unnamed\nagain\n'
            shell('rm /edit-note /named')
        enter('env LINES=0 COLUMNS=999 vile /edit-note')
        leave(b'iHello Ripplx\x7fe\rsecond line\r\x1b:wq\r')
        assert contents('/edit-note') == b'Hello Ripple\nsecond line\n'
        shell('chmod 640 /edit-note')

        enter('env LINES=12 COLUMNS=40 vile /edit-note')
        leave(b'rYjdd:wq\r')
        assert contents('/edit-note') == b'Yello Ripple\n'
        assert contents('/edit-note~') == b'Hello Ripple\nsecond line\n'
        assert b'-rw-r-----' in shell('ls -l /edit-note')

        enter('vile /edit-note')
        send(b'r\x03li\x08\x03:q\r')
        until(b'No write since last change.')
        leave(b':q!\r')
        assert contents('/edit-note') == b'Yello Ripple\n'

        # A filename supplied by :w must remain usable by the next :wq.
        enter('vile')
        leave(b'iunnamed\r\x1b:w /named\riagain\r\x1b:wq\r')
        assert contents('/named') == b'unnamed\nagain\n'

        # Raw mode must end with the editor: canonical Ctrl-U works again.
        send(b'garbage\x15echo restored > /tty-after\r')
        until(b'fuzix# ')
        assert contents('/tty-after') == b'restored\n'

        enter('vile /limit')
        leave(b':q\r')
        enter('vile /native-char')
        leave(b':q\r')
        assert b'\\u1234' in transcript
        output = shell('vile /oversize; echo EDITOR:$?')
        assert b'vile: file too large' in output and b'EDITOR:2' in output, output

        send(b'shutdown\r')
        until(b'Ripple: disk synced; power off.')
        assert process.wait(timeout=10) == 0
        assert b'\x1b[12;1H' in transcript and b'\x1b[24;1H' in transcript
    finally:
        Path('/tmp/fuzix-editor-' + ('jit' if jit else 'interpreter') + '.log').write_bytes(transcript)
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


with tempfile.TemporaryDirectory(prefix='fuzix-editor-') as temporary:
    image = Path(temporary) / 'editor.img'
    subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
    fixtures = r"""
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const file = process.argv[1], disk = openImage(file), root = fatFile(disk, 'ROOTFS.IMG');
const data = Array.from('line\n'.repeat(6000), c => c.charCodeAt(0));
const updated = installRoot(root.data, [
  {name: '/limit', mode: 0o100644, data},
  {name: '/oversize', mode: 0o100644, data: [...data, 88]},
  {name: '/native-char', mode: 0o100644, data: [0x1234, 10]}
]);
atomicWrite(file, root.replace(updated));
"""
    subprocess.run(['node', '--input-type=module', '-e', fixtures, str(image)], cwd=ROOT, check=True)
    exercise(image, engines[0])
    # Reopen saved files on the same persistent disk.
    exercise(image, engines[-1])

print('VILE passed: live editing, save/reopen, backups, permissions, discard, named writes, raw-mode restoration, file limits; ' + engine_label)
