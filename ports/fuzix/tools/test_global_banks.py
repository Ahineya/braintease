#!/usr/bin/env python3
"""Exercise bank-spanning static data in a disposable RVM/Riplix guest."""
import argparse
import json
import os
from pathlib import Path
import pty
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    args = parser.parse_args()
    build = args.build.resolve()
    log = build / 'global-banks-test.log'
    with tempfile.TemporaryDirectory(prefix='global-banks-') as temporary:
        stage = Path(temporary)
        names = ['init', 'sh', 'echo', 'globalcheck']
        for name in [*(name + '.rxe' for name in names), 'FUZIX.SYS', 'bios.bin']:
            (stage / name).symlink_to(build / name)
        (stage / 'apps.json').write_text(json.dumps(names))
        image = stage / 'disk.img'
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(stage), str(image)], check=True)
        master, slave = pty.openpty()
        with log.open('wb') as output:
            guest = subprocess.Popen([str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                                      '--disk', str(image), '--memory', '16384000', '--net', 'off',
                                      '--jit', '--timeout', '120'],
                                     stdin=slave, stdout=output, stderr=subprocess.STDOUT)
            os.close(slave)

            def expect(marker):
                deadline = time.monotonic() + 100
                while True:
                    content = log.read_bytes().replace(b'\r', b'')
                    if b'GLOBAL_CHECK_FAILED' in content or b'GLOBAL_STATUS:1\n' in content:
                        raise AssertionError(content.decode(errors='replace'))
                    if marker in content:
                        return content
                    if guest.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError(content.decode(errors='replace'))
                    time.sleep(.05)

            try:
                expect(b'riplix# ')
                os.write(master, b'/bin/globalcheck; echo GLOBAL_STATUS:$?\r')
                content = expect(b'GLOBAL_STATUS:0\n')
                assert b'GLOBAL_BANKS_OK ' in content and b'GLOBAL_BANKS_EXEC_OK' in content, content
                os.write(master, b'shutdown\r')
                assert guest.wait(timeout=15) == 0
            finally:
                if guest.poll() is None:
                    guest.kill()
                    guest.wait()
                os.close(master)
    print('RVM/Riplix: banked globals, wide values, heap, syscall spans, fork/COW and exec passed')


if __name__ == '__main__':
    main()
