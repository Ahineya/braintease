#!/usr/bin/env python3
"""Test host elapsed time, calendar time, alarms and pause recovery in real guests."""
import argparse
from launch import require_mmu
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('jit', 'interpreter', 'both'), default='jit')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']
libs = subprocess.check_output([
    'make', '-s', '--no-print-directory', '-C', str(PORT), f'BUILD={build}',
    '-f', 'Makefile', '-f', '-', 'clock-libs'
], input='clock-libs:\n\t@echo $(LIBC_OBJS)\n', text=True).split()

with tempfile.TemporaryDirectory(prefix='fuzix-timers-') as directory:
    tmp = Path(directory)
    asm, obj, exe = [tmp / ('clockcheck' + suffix) for suffix in ('.asm', '.pobj', '.rxe')]
    subprocess.run([
        str(ROOT / 'target/release/rcc'), 'compile', str(PORT / 'tools/clockcheck.c'),
        '-o', str(asm), '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
        *[f'-I{path}' for path in (PORT / 'ripple/include', PORT / 'upstream/Library/include',
                                 ROOT / 'runtime/include', PORT / 'ripple')]
    ], check=True)
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rasm'), 'assemble', str(asm),
                    '-o', str(obj), '--bank-size', '64000', '--max-immediate', '65535'], check=True)
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rlink'),
                    str(build / 'user/entry.pobj'), str(obj), str(build / 'user/userlib.pobj'),
                    *libs, '-f', 'rxe', '--bank-size', '64000', '-o', str(exe)], check=True)
    # Stage only this test's commands, independent of optional desktop assets.
    names = ['init', 'sh', 'echo', 'preemptcheck', 'uptime']
    for name in [*(name + '.rxe' for name in names), 'FUZIX.SYS', 'bios.bin']:
        (tmp / name).symlink_to(build / name)
    (tmp / 'apps.json').write_text(json.dumps([*names, 'clockcheck']))
    for jit in engines:
        image = tmp / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(tmp), str(image)], check=True)
        args = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                '--memory', '16384000', '--headless',
                '--timeout', '120', *(['--jit'] if jit else [])]
        commands = ('clockcheck\necho CLOCK_STATUS:$?\nclockcheck --pause\n'
                    'echo PAUSE_STATUS:$?\npreemptcheck\nuptime\nshutdown\n')
        before = int(time.time())
        proc = subprocess.Popen([*args, '--input', commands], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = bytearray()
        stopped = False
        sleep_start = None
        sleep_checked = False
        deadline = time.monotonic() + 130
        try:
            while True:
                assert time.monotonic() < deadline, output.decode(errors='replace')[-8000:]
                if not select.select([proc.stdout], [], [], 0.1)[0]:
                    continue
                data = os.read(proc.stdout.fileno(), 65536)
                if not data:
                    break
                output.extend(data)
                normalized = output.replace(b'\r', b'')
                if not sleep_checked and sleep_start is None and b'CLOCK_SLEEP_READY\n' in normalized:
                    sleep_start = time.monotonic()
                if not sleep_checked and sleep_start is not None and b'CLOCK_SLEEP:' in normalized:
                    assert time.monotonic() - sleep_start >= 1.7, 'sleep ran faster than host time'
                    sleep_checked = True
                if not stopped and b'CLOCK_PAUSE_READY\n' in normalized:
                    os.kill(proc.pid, signal.SIGSTOP)
                    time.sleep(3)
                    os.kill(proc.pid, signal.SIGCONT)
                    stopped = True
            assert proc.wait(timeout=10) == 0
        finally:
            if proc.poll() is None:
                os.kill(proc.pid, signal.SIGCONT)
                proc.terminate()
                proc.wait(timeout=10)
        text = output.decode(errors='replace').replace('\r', '')
        Path('/tmp/fuzix-timers-mmu-' + ('jit' if jit else 'interpreter') + '.log').write_text(text)
        assert stopped and 'CLOCK_STATUS:0\n' in text and 'PAUSE_STATUS:0\n' in text, text[-8000:]
        epoch = int(re.search(r'CLOCK_EPOCH:(\d+)', text)[1])
        assert before - 2 <= epoch <= int(time.time()) + 2, text
        paused = int(re.search(r'CLOCK_PAUSED:(\d+) SIGNALS:1', text)[1])
        assert 29 <= paused <= 100, text
        assert 'calendar isolation and timestamps passed' in text, text
        assert text.index('child ran while parent computed') < text.index('parent computation finished'), text
        assert 'disk synced; power off.' in text and 'panic:' not in text, text
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(f'Host clocks, real sleeps/alarms and pause recovery passed (MMU=1, JIT={jit})')
