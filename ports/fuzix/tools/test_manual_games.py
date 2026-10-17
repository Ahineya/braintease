#!/usr/bin/env python3
"""Run manuals and classic games in real guests, including live terminal control."""
import argparse
import errno
import json
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a = p.parse_args()
build = a.build.resolve()

COMMANDS = '''man -w sh
man -w 6 wump
man -w tclsh
man sh | col -b > /sh-help
grep SYNOPSIS /sh-help
man vi | col -b > /vi-help
grep Levee /vi-help
man tclsh | col -b > /tcl-help
grep 'Tcl subset' /tcl-help
man less | col -b > /less-help
grep implemented /less-help
man hamurabi | col -b > /ham-help
grep 'govern ancient Sumeria' /ham-help
man nonexistent; echo MAN_MISSING:$?
man 3 wump; echo MAN_SECTION:$?
man -z sh; echo MAN_OPTION:$?
man; echo MAN_EMPTY:$?
man -l /manual-local | col -b
man -l /manual-loop; echo MAN_LOOP:$?
man -l /manual-stress > /manual-render; echo MAN_STRESS:$?
man -l /manual-native > /manual-reject; echo MAN_NATIVE:$?
man -l /manual-high > /manual-reject; echo MAN_HIGH:$?
man -l /manual-ffff > /manual-reject; echo MAN_FFFF:$?
env MANPATH=/missing:/usr/man MANSECT=8:1 man -w sh
env MANPATH=/manual-tree MANSECT=1 man -w custom
man -l /manual-plain
wump --help
wump < /dev/null; echo WUMP_EOF:$?
wump < /wump-invalid; echo WUMP_INPUT:$?
hamurabi --help
hamurabi < /dev/null; echo HAM_EOF:$?
hamurabi < /ham-input; echo HAM_INPUT:$?
hamurabi < /ham-negative; echo HAM_NEGATIVE:$?
hamurabi < /ham-overfeed; echo HAM_OVERFEED:$?
cat /sh-help | wc -l
sync
shutdown
'''


def seed(image):
    fixtures = {
        '/manual-local': '.TH LOCAL 1\n.SH NAME\nLOCAL_MANUAL_OK\n',
        '/manual-loop': '.so /manual-loop\n',
        '/manual-stress': '.TH ' + 'x' * 79 + ' 1 "' + 'y' * 79 + '" "' + 'z' * 79 + '"\n'
                          + '.RS\n' * 100 + '.IP tag\n' + 'x' * 500 + '\n' + '.RE\n' * 100,
        '/manual-native': '.TH BAD 1\n\u1234\n',
        '/manual-high': '.TH BAD 1\n\u8000\n',
        '/manual-ffff': '.TH BAD 1\n\uffff\n',
        '/manual-plain': 'PREFORMATTED_MANUAL_OK\n',
        '/manual-tree/man1/custom.1': '.TH CUSTOM 1\n.SH NAME\ncustom\n',
        '/wump-invalid': 'n\nm 99999999999999999999999999999\nm nope\nq\n',
        '/ham-input': 'nonsense\n99999999999999999999999999999999999999999999999999\n1000000\nq\n',
        '/ham-negative': '-1\n',
        '/ham-overfeed': '0\n0\n2800\n0\n0\n0\n0\n0\n',
    }
    manifest = image.with_suffix('.json')
    manifest.write_text(json.dumps([{'name': name, 'mode': 0o100644, 'data': [ord(c) for c in text]}
                                    for name, text in fixtures.items()]))
    subprocess.run(['node', '--input-type=module', '-e', '''
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const name=process.argv[1], disk=openImage(name), root=fatFile(disk,'ROOTFS.IMG');
atomicWrite(name,root.replace(installRoot(root.data,JSON.parse(fs.readFileSync(process.argv[2],'utf8')))));
''', str(image), str(manifest)], cwd=ROOT, check=True)


def batch(image, jit, label):
    command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
               '--memory', '16384000', '--headless', '--input', COMMANDS, '--timeout', '300']
    if jit:
        command.append('--jit')
    log = Path('/tmp/fuzix-manual-games-' + label + '.log')
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=310)
    output = log.read_text().replace('\r', '').split('Starting /init\n')[-1]
    output = '\n'.join(line for line in output.splitlines() if not line.startswith('fuzix# '))
    assert result.returncode == 0 and 'disk synced; power off.' in output, output
    for expected in ('/usr/man/man1/sh.1', '/usr/man/man6/wump.6', '/usr/man/man1/tclsh.1',
                     'SYNOPSIS', 'Levee', 'Tcl subset',
                     'implemented', 'govern ancient Sumeria',
                     'MAN_MISSING:1', 'MAN_SECTION:1', 'MAN_OPTION:1', 'MAN_EMPTY:1',
                     'MAN_LOOP:1', 'MAN_STRESS:0', 'MAN_NATIVE:1', 'MAN_HIGH:1', 'MAN_FFFF:1', 'LOCAL_MANUAL_OK',
                     'PREFORMATTED_MANUAL_OK', '/manual-tree/man1/custom.1',
                     'WUMP_EOF:0', 'WUMP_INPUT:0', 'You hit the wall',
                     'HAM_EOF:0', 'HAM_INPUT:0', 'HAM_NEGATIVE:0', 'HAM_OVERFEED:0',
                     'Please enter a whole number', 'Think again', 'Get yourself another steward',
                     'in year 2,', 'been impeached'):
        assert expected in output, (expected, output)
    assert 'in year 3,' not in output, output
    assert 'corrupt superblock' not in output and 'panic:' not in output, output
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


def live(image, jit, label):
    master, slave = pty.openpty()
    command = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--timeout', '360']
    if not jit:
        command.append('--no-jit')
    proc = subprocess.Popen(command, env=dict(os.environ, FUZIX_BUILD=str(build)), stdin=slave,
                            stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    pending = b''
    transcript = bytearray()

    def until_any(tokens, timeout=60):
        nonlocal pending
        deadline = time.monotonic() + timeout
        while not any(token in pending for token in tokens):
            assert time.monotonic() < deadline, (tokens, pending[-5000:])
            if select.select([master], [], [], 0.1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as e:
                    if e.errno == errno.EIO:
                        raise AssertionError(pending[-5000:]) from e
                    raise
                assert data, pending[-5000:]
                pending += data
                transcript.extend(data)
        token = next(t for t in tokens if t in pending)
        end = pending.index(token) + len(token)
        result, pending = pending[:end], pending[end:]
        return result.replace(b'\r', b'')

    def until(token):
        return until_any([token])

    def send(data):
        os.write(master, data)

    def shell(command):
        send(command.encode() + b'\r')
        return until(b'fuzix# ')

    try:
        until(b'fuzix# ')
        shell('stty -g > /tty-before-games')
        send(b'env LINES=6 man sh\r')
        until(b'--More--')
        send(b'q')
        until(b'fuzix# ')
        assert b'MAN_QUIT:0' in shell('echo MAN_QUIT:$?')
        send(b'env PAGER=less man intro\r')
        until(b'\x1b[?1049h'); until(b'-- less:')
        send(b'q'); until(b'\x1b[?1049l'); until(b'fuzix# ')
        send(b'env LINES=6 man sh\r'); until(b'--More--')
        send(b'\x03'); until(b'fuzix# ')
        shell('echo AFTER_MAN')
        send(b'wump\r'); until(b'Instructions?')
        send(b'y\r')
        intro = until(b'Move or shoot (m-s) ')
        assert b'Bottomless Pits' in intro and b'There are tunnels to' in intro, intro
        # Shoot once, then return via either q or the loss/replay prompt.
        send(b's 0\r')
        state = until_any([b'Move or shoot (m-s) ', b'Another game?'])
        send(b'n\r' if b'Another game?' in state else b'q\r')
        until(b'fuzix# ')
        send(b'wump\r'); until(b'Instructions?'); send(b'n\r')
        until(b'Move or shoot (m-s) '); send(b'\x03'); until(b'fuzix# ')
        assert b'WUMP_INTERRUPT:130' in shell('echo WUMP_INTERRUPT:$?')
        send(b'hamurabi\r'); until(b'How many acres do you wish to buy?')
        send(b'0\r'); until(b'How many acres do you wish to sell?')
        send(b'0\r'); until(b'How many bushels do you wish to feed your people?')
        until(b'bushels.)'); send(b'2800\r'); until(b'How many acres do you wish to plant with seed?')
        until(b'acres)'); send(b'0\r')
        year2 = until(b'How many acres do you wish to buy?')
        assert b'in year 2,' in year2, year2
        send(b'q\r'); until(b'fuzix# ')
        send(b'hamurabi\r'); until(b'How many acres do you wish to buy?')
        send(b'\x03'); until(b'fuzix# ')
        assert b'HAM_INTERRUPT:130' in shell('echo HAM_INTERRUPT:$?')
        shell('stty -g > /tty-after-games')
        assert b'GAME_TTY:0' in shell('cmp /tty-before-games /tty-after-games; echo GAME_TTY:$?')
        send(b'shutdown\r'); until(b'disk synced; power off.')
        assert proc.wait(timeout=10) == 0
    finally:
        Path('/tmp/fuzix-manual-games-live-' + label + '.log').write_bytes(transcript)
        if proc.poll() is None:
            proc.kill(); proc.wait()
        os.close(master)
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


with tempfile.TemporaryDirectory(prefix='fuzix-manual-games-') as temporary:
    for jit in ([True, False] if a.engine == 'both' else [a.engine == 'jit']):
        label = 'jit' if jit else 'interpreter'
        image = Path(temporary) / (label + '.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
        seed(image)
        batch(image, jit, label)
        live(image, jit, label)
        print(label + ': manuals, pager controls, game input/gameplay, EOF and Ctrl-C passed', flush=True)
