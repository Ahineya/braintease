#!/usr/bin/env python3
"""Real-guest job control, process monitoring and PTY integration checks."""
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
p.add_argument('--part', choices=('both', 'batch', 'live'), default='both')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)


def fsck(image):
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


def batch(image, jit):
    commands = ('ttycheck\necho TTYCHECK:$?\nps -elf\nyes > /dev/null &\ntop -b -n 2 -d 1\n'
                'echo TOP:$?\nkill $!\nwait $!\nps -u root -t tty1; echo PS_SELECTORS:$?\n'
                'ps -p 1,; echo PS_TRAILING:$?\ntop -n 0; echo TOP_BAD:$?\nps -p xxx; echo PS_BAD:$?\n'
                'sleep 1 &\nwait $!\necho WAIT:$?\nshutdown\n')
    args = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
            '--memory', '16384000', '--headless',
            '--input', commands, '--timeout', '240']
    if jit:
        args += ['--jit']
    log = Path('/tmp/fuzix-jobs-mmu-' + ('jit' if jit else 'interpreter') + '.log')
    with log.open('w') as out:
        result = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=250)
    text = log.read_text().replace('\r', '')
    assert result.returncode == 0 and 'disk synced; power off.' in text, text[-10000:]
    for token in ['TTYCHECK:0', 'TOP:0', 'TOP_BAD:1', 'PS_BAD:1', 'PS_SELECTORS:0', 'PS_TRAILING:1', 'WAIT:0',
                  'stop/continue, stopped kill, and process snapshots passed',
                  'PTY isolation, canonical input, backpressure, EOF, hangup, reuse passed',
                  'controlling terminals and session isolation passed', 'PRIV-K SHRD-K']:
        assert token in text, (token, text[-10000:])
    assert text.count('top - up ') == 2, text[-6000:]
    loads = re.findall(r'^\s*\d+\s+\d+\s+\d+\s+R\s+(\d+\.\d+).* yes$', text, re.M)
    assert any(float(load) > 0 for load in loads), text[-6000:]
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 10 and fields[0].isdigit() and re.fullmatch(r'\d+\.\d+', fields[4]):
            assert int(fields[5]) == int(fields[6]) + int(fields[7]), line
    assert 'FAILED' not in text and 'panic:' not in text, text[-10000:]
    fsck(image)


def live(image, jit):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    command = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--net', 'off', '--timeout', '300']
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
            assert time.monotonic() < deadline, (token, pending[-7000:])
            if select.select([master], [], [], 0.1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as e:
                    if e.errno == errno.EIO:
                        raise AssertionError(pending[-7000:]) from e
                    raise
                assert data, pending[-7000:]
                pending += data
                transcript.extend(data)
        end = pending.index(token) + len(token)
        result, pending = pending[:end], pending[end:]
        return result.replace(b'\r', b'')

    def send(data):
        os.write(master, data)

    def command_line(data):
        send(data + b'\r')
        return until(b'fuzix# ')

    try:
        until(b'fuzix# ')
        send(b'ttycheck --wait\r')
        until(b'\nTTY_WAIT_READY\r')
        send(b'\x1a')
        assert b'Stopped ttycheck --wait' in until(b'fuzix# ')
        assert b'Stopped ttycheck --wait' in command_line(b'jobs')
        text = command_line(b'ps -el')
        assert re.search(rb'\bT\s+1\s+\d+:\d+:\d+.*ttycheck', text), text
        assert b'Running ttycheck --wait' in command_line(b'bg')
        send(b'fg\r')
        until(b'ttycheck --wait\r')
        send(b'\x03')
        until(b'fuzix# ')
        assert b'FG_STATUS:130\n' in command_line(b'echo FG_STATUS:$?')
        send(b'vi /job-note\r'); until(b'\x1b[?1049h')
        time.sleep(0.2)
        send(b'iPreserved through suspend.\x1b')
        time.sleep(0.1)
        send(b'\x1a')
        text = until(b'fuzix# ')
        assert b'Stopped vi /job-note' in text and b'\x1b[?1049l' in text, text
        send(b'fg\r'); until(b'\x1b[?1049h')
        send(b':wq\r'); until(b'fuzix# ')
        assert b'Preserved through suspend.' in command_line(b'cat /job-note')
        # Wait for the final executable itself, rather than a shell message
        # before exec: Ctrl-C during that handoff can see shell signal policy.
        send(b'ttycheck --wait | ttycheck --wait\r')
        until(b'\nTTY_WAIT_READY\r')
        send(b'\x1a')
        assert b'Stopped pipeline' in until(b'fuzix# ')
        send(b'fg\r'); until(b'pipeline\r'); send(b'\x03'); until(b'fuzix# ')
        assert b'PIPE_STATUS:130\n' in command_line(b'echo PIPE_STATUS:$?')
        command_line(b'stty tostop')
        command_line(b'echo BG_WRITE &')
        deadline = time.monotonic() + 45
        while b'Stopped echo BG_WRITE' not in command_line(b'jobs'):
            assert time.monotonic() < deadline
            time.sleep(0.1)
        assert b'BG_WRITE\n' in command_line(b'fg')
        command_line(b'stty -tostop')
        command_line(b'cat &')
        deadline = time.monotonic() + 45
        while b'Stopped cat' not in command_line(b'jobs'):
            assert time.monotonic() < deadline
            time.sleep(0.1)
        send(b'fg\r'); until(b'cat\r'); send(b'PTY_CAT\r')
        until(b'PTY_CAT\r')
        send(b'\x04'); until(b'fuzix# ')
        send(b'top\r'); until(b'CPU%')
        time.sleep(0.2); send(b'iPreserved through suspend.\x1b'); time.sleep(0.1); send(b'\x1a')
        assert b'Stopped top' in until(b'fuzix# ')
        send(b'fg\r'); until(b'top\r'); send(b'q')
        text = until(b'fuzix# ')
        assert b'\x1b[?25h' in text and b'\x1b[?1049l' in text, text
        # Each nested host allocates a fresh master; its shell has an independent
        # session and controlling slave, including foreground Ctrl-C handling.
        send(b'ptysh\r'); until(b'fuzix# ')
        text = command_line(b'ps -ef')
        assert re.search(rb'\bS\s+2\s+\d+:\d+:\d+.*sh', text), text
        send(b'ptysh\r'); until(b'fuzix# ')
        text = command_line(b'ps -ef')
        assert re.search(rb'\bS\s+3\s+\d+:\d+:\d+.*sh', text), text
        send(b'ttycheck --wait\r'); until(b'\nTTY_WAIT_READY\r'); send(b'\x03'); until(b'fuzix# ')
        assert b'INNER_STATUS:130\n' in command_line(b'echo INNER_STATUS:$?')
        command_line(b'exit')
        command_line(b'exit')
        assert b'OUTER_OK\n' in command_line(b'echo OUTER_OK')
        send(b'shutdown\r'); until(b'disk synced; power off.')
        assert proc.wait(timeout=15) == 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)
        os.close(master)
        Path('/tmp/fuzix-jobs-live-mmu-' + ('jit' if jit else 'interpreter') + '.log').write_bytes(transcript)
    fsck(image)


with tempfile.TemporaryDirectory(prefix='fuzix-jobs-') as tmp:
    for jit in [True, False]:
        if (jit and a.engine == 'interpreter') or (not jit and a.engine == 'jit'):
            continue
        image = Path(tmp) / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
        if a.part != 'live': batch(image, jit)
        if a.part != 'batch': live(image, jit)
        coverage = {'both': 'job control, monitoring, PTY sessions', 'batch': 'process primitives, PTY driver, batch monitoring', 'live': 'interactive jobs, editor suspend/resume, nested PTYs'}[a.part]
        print(('JIT' if jit else 'Interpreter') + ': ' + coverage + ' passed', flush=True)
