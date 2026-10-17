#!/usr/bin/env python3
"""Lossless large raw-mode paste through the real native console line discipline."""
import argparse
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time
import tty

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=HERE / 'build/fuse-x')
    args = parser.parse_args()
    kernel = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='fx-paste-', dir='/tmp') as temporary:
        directory = Path(temporary)
        share = directory / 'share'
        share.mkdir()
        subprocess.run([sys.executable, str(HERE / 'cc.py'), str(HERE / 'examples/console-paste.c'),
                        '-o', str(share / 'paste')], check=True)
        image = directory / 'root.img'
        builder = (ROOT / 'ports/fuzix/tools/disk.mjs').as_uri()
        source = ('import fs from "node:fs";import {rootImage,atomicWrite} from ' + repr(builder) + ';'
                  'atomicWrite(process.argv[1],rootImage(['
                  '{name:"/init",mode:0o100755,data:Buffer.from("FXN1:init:1\\n")},'
                  '{name:"/bin/sh",mode:0o100755,data:fs.readFileSync(process.argv[2])},'
                  '{name:"/bin/echo",mode:0o100755,data:fs.readFileSync(process.argv[3])},'
                  '{name:"/dev/tty",mode:0o20666,device:0x200}'
                  '],8192,128));')
        subprocess.run(['node', '--input-type=module', '-e', source, str(image),
                        str(kernel.parent / 'apps/sh.fxe'), str(kernel.parent / 'apps/echo.fxe')], check=True)
        master, slave = pty.openpty()
        tty.setraw(slave)
        guest = subprocess.Popen([str(kernel), '--run', str(image), '--share', str(share)],
            stdin=slave, stdout=slave, stderr=slave,
            env=dict(os.environ, RCC_NATIVE_NET='off', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT='30'))
        os.close(slave)
        output = bytearray()
        at = 0
        def expect(token):
            nonlocal at
            deadline = time.monotonic() + 30
            while token not in output[at:]:
                assert time.monotonic() < deadline and guest.poll() is None, output.decode(errors='replace')
                if select.select([master], [], [], .05)[0]: output.extend(os.read(master, 65536))
            at = output.index(token, at) + len(token)
        try:
            expect(b'fuse-x# ')
            os.write(master, b'/mnt/host/paste; echo PASTE_STATUS:$?\r')
            expect(b'PASTE_READY')
            os.write(master, bytes(ord('a') + i % 26 for i in range(1500)))
            expect(b'PASTE_OK 1500')
            expect(b'PASTE_STATUS:0')
            expect(b'fuse-x# ')
            os.write(master, b'shutdown\r')
            assert guest.wait(timeout=10) == 0
        finally:
            Path('/tmp/fuse-x-console-paste.log').write_bytes(output)
            if guest.poll() is None: guest.kill(); guest.wait()
            os.close(master)
    print('Native console: 1500-byte raw paste retained in order; shell modes restored')


if __name__ == '__main__': main()
