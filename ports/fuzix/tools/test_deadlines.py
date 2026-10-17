#!/usr/bin/env python3
"""Exercise fine deadlines inside an actual kernel with a disposable disk."""
import argparse
import json
import os
from pathlib import Path
import pty
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    build = args.build.resolve()
    native = args.native_build.resolve()
    with tempfile.TemporaryDirectory(prefix='fdeadline-', dir='/tmp') as temporary:
        tmp = Path(temporary)
        image = tmp / 'disk.img'
        if args.native:
            executable = tmp / 'finecheck.fxe'
            subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'), str(PORT / 'tools/finecheck.c'),
                            '--build', str(native), '-o', str(executable)], check=True)
            subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                            '--apps', str(native / 'apps')], check=True)
            subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image),
                            '/bin/finecheck', str(executable)], check=True)
            command = [str(native / 'fuse-x'), '--run', str(image)]
        else:
            executable = build / 'finecheck.rxe'
            subprocess.run(['make', '-s', '-C', str(PORT), f'BUILD={build}', '-j4', str(executable)], check=True)
            stage = tmp / 'stage'
            stage.mkdir()
            names = ['init', 'sh', 'echo', 'true', 'finecheck']
            for name in [*(n + '.rxe' for n in names), 'FUZIX.SYS', 'bios.bin']:
                (stage / name).symlink_to(build / name)
            (stage / 'apps.json').write_text(json.dumps(names))
            subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(stage), str(image)], check=True)
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                       '--memory', '16384000', '--net', 'off', '--jit', '--timeout', '90']
        env = dict(os.environ, RCC_NATIVE_NET='off', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT='90')
        log = ROOT / 'build/browser' / ('deadlines-native.log' if args.native else 'deadlines-rvm.log')
        log.parent.mkdir(parents=True, exist_ok=True)
        master, slave = pty.openpty()
        with log.open('wb') as output:
            guest = subprocess.Popen(command, stdin=slave, stdout=output, stderr=subprocess.STDOUT, env=env)
            os.close(slave)
            def wait(marker, timeout=60):
                deadline = time.monotonic() + timeout
                while True:
                    content = log.read_bytes().replace(b'\r', b'')
                    if marker in content:
                        return content
                    if b'FINE_FAILED' in content or re.search(rb'(?:FINE|IDLE|PAUSE)_STATUS:[1-9][0-9]*\n', content):
                        raise AssertionError(content.decode(errors='replace'))
                    if guest.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError(content.decode(errors='replace'))
                    time.sleep(.02)
            try:
                wait(b'fuse-x# ' if args.native else b'fuzix# ')
                os.write(master, b'finecheck; echo FINE_STATUS:$?\r')
                content = wait(b'FINE_STATUS:0\n')
                assert b'FINE_OK ' in content and b'FINE_FAILED' not in content, content
                os.write(master, b'finecheck idle; echo IDLE_STATUS:$?\r')
                wait(b'FINE_IDLE_READY\n')
                def cpu_time():
                    value = subprocess.check_output(['ps', '-o', 'time=', '-p', str(guest.pid)], text=True).strip()
                    return sum(float(part) * 60**i for i, part in enumerate(reversed(value.split(':'))))
                cpu_before = cpu_time()
                wait(b'IDLE_STATUS:0\n')
                spent = cpu_time() - cpu_before
                assert spent < .5, f'Idle wait consumed {spent:.3f}s host CPU'
                print(f'Two-second idle wait: {spent:.3f}s host CPU')
                os.write(master, b'finecheck pause; echo PAUSE_STATUS:$?\r')
                wait(b'FINE_PAUSE_READY\n')
                stopped = time.monotonic()
                os.kill(guest.pid, signal.SIGSTOP)
                time.sleep(.6)
                resumed = time.monotonic()
                os.kill(guest.pid, signal.SIGCONT)
                # Observe the syscall return before waiting for printf, child
                # reaping and shell exit (which may need cold JIT compilation).
                wait(b'FINE_RESUMED\n')
                recovery = time.monotonic() - resumed
                assert recovery < .25, f'Expired wait resumed {recovery:.3f}s after SIGCONT'
                pause_output = wait(b'PAUSE_STATUS:0\n')
                assert b'FINE_PAUSE_OK' in pause_output
                paused = int(re.search(rb'FINE_PAUSED (\d+) ms', pause_output)[1])
                stopped_ms = (resumed - stopped) * 1000
                assert stopped_ms - 5 <= paused < stopped_ms + 300, (paused, stopped_ms)
                print(f'Expired deadline after host pause: resumed in {recovery*1000:.0f} ms')
                os.write(master, b'shutdown\r')
                guest.wait(timeout=20)
                assert guest.returncode == 0
                print(('Native' if args.native else 'RVM JIT') + ': ' +
                      next(line.decode() for line in content.splitlines() if line.startswith(b'FINE_OK ')))
            finally:
                if guest.poll() is None:
                    guest.kill()
                    guest.wait()
                os.close(master)


if __name__ == '__main__':
    main()
