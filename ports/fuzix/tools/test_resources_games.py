#!/usr/bin/env python3
"""Exercise MMU pressure, resource accounting and Sokoban in real guests."""
import argparse
from launch import require_mmu
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)


def check_disk(image):
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


def run(image, jit):
    commands = ('free\nuptime\nfree --bad; echo FREE_BAD:$?\nuptime --bad; echo UP_BAD:$?\n'
                'sokoban --help\nsok --help\nsokoban -l 9999; echo SOK_RANGE:$?\n'
                'sokoban -l -1; echo SOK_NUMBER:$?\nsokoban -l; echo SOK_ARG:$?\n'
                'sokoban /bad-levels; echo SOK_BAD:$?\n'
                'man -w free\nman -w uptime\nman -w 6 sokoban\n')
    commands += 'mmucheck --stress\necho MMU_STRESS:$?\n'
    commands += 'free\nuptime\necho RESOURCE_RECOVERY_OK\nshutdown\n'
    args = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
            '--memory', '16384000', '--headless',
            '--input', commands, '--timeout', '600']
    if jit:
        args.append('--jit')
    log = Path('/tmp/fuzix-resources-mmu-' + ('jit' if jit else 'interpreter') + '.log')
    with log.open('w') as out:
        result = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=610)
    text = log.read_text().replace('\r', '')
    assert result.returncode == 0 and 'disk synced; power off.' in text, text[-8000:]
    for expected in ['FREE_BAD:1', 'UP_BAD:1', 'SOK_RANGE:1', 'SOK_NUMBER:1', 'SOK_ARG:1',
                     'SOK_BAD:1', '/usr/man/man1/free.1', '/usr/man/man1/uptime.1',
                     '/usr/man/man6/sokoban.6', 'RESOURCE_RECOVERY_OK']:
        assert expected in text, (expected, text[-8000:])
    rows = re.findall(r'User pool:\s+(\d+)\s+(\d+)\s+(\d+)', text)
    assert len(rows) == 2, text[-8000:]
    for total, used, free in rows:
        assert int(total) == 16000
        assert int(used) + int(free) == int(total)
    # The shell may release its own partial arena page between commands.
    assert int(rows[1][1]) <= int(rows[0][1]) + 1, rows
    assert len(re.findall(r'up \d+ days?, \d+:\d+:\d+, \d+/\d+ processes, load average:', text)) == 2
    for expected in ['MMU_STRESS:0', 'process exhaustion (30 children) and slot recovery passed',
                     'physical exhaustion, failed exec, and page recovery passed',
                     'shrink/regrow, ENOMEM rollback, and repeated cleanup passed']:
        assert expected in text, (expected, text[-8000:])
    assert 'panic:' not in text and 'FAILED' not in text, text[-8000:]
    check_disk(image)


def live(image, jit, resume=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    command = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--net', 'off', '--timeout', '240']
    if not jit:
        command.append('--no-jit')
    proc = subprocess.Popen(command, env=dict(os.environ, FUZIX_BUILD=str(build)),
                            stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    pending = b''
    transcript = bytearray()

    def until(token, timeout=90):
        nonlocal pending
        deadline = time.monotonic() + timeout
        while token not in pending:
            assert time.monotonic() < deadline, (token, pending[-6000:])
            if select.select([master], [], [], 0.1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as e:
                    if e.errno == errno.EIO:
                        raise AssertionError(pending[-6000:]) from e
                    raise
                assert data, pending[-6000:]
                pending += data
                transcript.extend(data)
        end = pending.index(token) + len(token)
        result, pending = pending[:end], pending[end:]
        return result.replace(b'\r', b'')

    def send(data):
        os.write(master, data)

    try:
        until(b'fuzix# ')
        send(b'sokoban\r')
        until(b'Sokoban - level ' + (b'2/2' if resume else b'1/2'))
        until(b'q: quit')
        if not resume:
            # Move, push, undo, restart, then solve with an arrow-key push.
            send(b'llu')
            until(b'1 moves')
            send(b'ry')
            until(b'Restart level?')
            until(b'0 moves')
            send(b'll\x1b[C')
            until(b'Level 1 done in 3 moves.')
            send(b' ')
            until(b'Sokoban - level 2/2')
            send(b'q')
            until(b'Quit?')
            send(b'y')
            until(b'fuzix# ')
            send(b'cat /.sokoban\r')
            assert b'2\n' in until(b'fuzix# ')
        else:
            send(b'\x03')
            text = until(b'fuzix# ')
            assert b'\x1b[?25h' in text and b'\x1b[?1049l' in text, text
        send(b'echo TERMINAL_OK\r')
        assert b'TERMINAL_OK\n' in until(b'fuzix# ')
        send(b'shutdown\r')
        until(b'disk synced; power off.')
        assert proc.wait(timeout=15) == 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)
        os.close(master)
        Path('/tmp/fuzix-sokoban-mmu-' + ('jit' if jit else 'interpreter') + ('-resume' if resume else '') + '.log').write_bytes(transcript)
    check_disk(image)


with tempfile.TemporaryDirectory(prefix='fuzix-resources-games-') as tmp:
    for jit in [True, False]:
        if (jit and a.engine == 'interpreter') or (not jit and a.engine == 'jit'):
            continue
        image = Path(tmp) / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
        subprocess.run(['node', '--input-type=module', '-e', '''
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {sokobanLevels} from './ports/fuzix/tools/sokoban.mjs';
const name=process.argv[1], disk=openImage(name), root=fatFile(disk,'ROOTFS.IMG');
const title='#####\\n#@Hi#\\n#####\\n\\n', puzzle='#######\\n#@ $ .#\\n#######\\n\\n';
const files=[{name:'/usr/lib/sok/sok.levels',mode:0o100644,data:sokobanLevels(title+puzzle+puzzle)},
             {name:'/bad-levels',mode:0o100644,data:Buffer.alloc(1024,32)}];
atomicWrite(name,root.replace(installRoot(root.data,files)));
''', str(image)], cwd=ROOT, check=True)
        run(image, jit)
        live(image, jit)
        live(image, jit, resume=True)
        coverage = 'resources, MMU recovery, Sokoban controls/progress/terminal passed'
        print(('JIT' if jit else 'Interpreter') + ': ' + coverage, flush=True)
