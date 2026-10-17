#!/usr/bin/env python3
"""Run the C16 curses probe inside an actual kernel with a disposable disk."""
import argparse
import json
import os
from pathlib import Path
import pty
import re
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
    with tempfile.TemporaryDirectory(prefix='fcurses-', dir='/tmp') as temporary:
        tmp = Path(temporary)
        image = tmp / 'disk.img'
        if args.native:
            executable = tmp / 'cursescheck.fxe'
            subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'), str(PORT / 'tools/cursescheck.c'),
                            '--tui-theme', '--build', str(native), '-o', str(executable)], check=True)
            subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                            '--apps', str(native / 'apps')], check=True)
            subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image),
                            '/bin/cursescheck', str(executable)], check=True)
            command = [str(native / 'fuse-x'), '--run', str(image)]
        else:
            executable = build / 'cursescheck.rxe'
            subprocess.run(['make', '-s', '-C', str(PORT), f'BUILD={build}', '-j4', str(executable)], check=True)
            stage = tmp / 'stage'
            stage.mkdir()
            names = ['init', 'sh', 'echo', 'true', 'cursescheck']
            for name in [*(n + '.rxe' for n in names), 'FUZIX.SYS', 'bios.bin']:
                (stage / name).symlink_to(build / name)
            (stage / 'apps.json').write_text(json.dumps(names))
            subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(stage), str(image)], check=True)
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                       '--memory', '16384000', '--net', 'off', '--jit', '--timeout', '90']
        env = dict(os.environ, RCC_NATIVE_NET='off', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT='90')
        log = ROOT / 'build/browser' / ('curses-native.log' if args.native else 'curses-rvm.log')
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
                    if b'CURSES_FAILED' in content or re.search(rb'CURSES_STATUS:[1-9][0-9]*\n', content):
                        raise AssertionError(content.decode(errors='replace'))
                    if guest.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError(content.decode(errors='replace'))
                    time.sleep(.02)
            try:
                wait(b'fuse-x# ' if args.native else b'fuzix# ')
                os.write(master, b'cursescheck; echo CURSES_STATUS:$?\r')
                content = wait(b'CURSES_STATUS:0\n')
                assert b'CURSES_OK ' in content and b'CURSES_FAILED' not in content, content
                os.write(master, b'shutdown\r')
                guest.wait(timeout=20)
                assert guest.returncode == 0
                print(('Native' if args.native else 'RVM JIT') + ': ' +
                      next(line.decode() for line in content.splitlines() if line.startswith(b'CURSES_OK ')))
            finally:
                if guest.poll() is None:
                    guest.kill()
                    guest.wait()
                os.close(master)


if __name__ == '__main__':
    main()
