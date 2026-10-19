#!/usr/bin/env python3
"""Render local HTML with C16 Lynx inside a disposable native or RVM guest."""
import argparse
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import time

from probe import PORT, ROOT


from guest import native_image, ripple_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/fuzix')
    args = parser.parse_args()
    native = args.target == 'native'
    executable = (args.executable or PORT / ('build/lynx.fxe' if native else 'build/lynx.rxe')).resolve()
    log = PORT / f'build/local-{args.target}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lynx-local-') as temporary:
        directory = Path(temporary)
        share = directory / 'share'
        shutil.copytree(PORT / 'fixtures', share)
        image = directory / 'root.img'
        master = None
        if native:
            shutil.copy2(executable, share / 'lynx')
            native_image(image, args.kernel)
            command_line = [str(args.kernel), '--run', str(image), '--share', str(share)]
            guest_input = subprocess.PIPE
            program, pages, prompt = '/mnt/host/lynx', '/mnt/host', b'fuse-x# '
        else:
            config = share / 'lynx.cfg'
            config.write_text(config.read_text().replace('/mnt/host/', '/www/'))
            build = args.build_dir.resolve()
            ripple_image(directory, image, build, executable)
            command_line = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                            '--disk', str(image), '--memory', '16384000', '--net', 'off',
                            '--jit', '--timeout', '240']
            master, guest_input = pty.openpty()
            program, pages, prompt = '/bin/lynx', '/www', b'riplix# '
        guest = subprocess.Popen(command_line, stdin=guest_input, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 env=dict(os.environ, RCC_NATIVE_NET='off', RCC_NATIVE_HEADLESS='1',
                                          RCC_NATIVE_TIMEOUT='120'))
        if master is not None:
            os.close(guest_input)
        transcript = bytearray()
        offset = 0

        def expect(marker):
            nonlocal offset
            deadline = time.monotonic() + (60 if native else 180)
            while True:
                normalized = bytes(transcript).replace(b'\r', b'')
                for line in normalized.split(b'\n')[:-1]:
                    if line.startswith((b'VERSION_STATUS:', b'DUMP_STATUS:')) and line.rsplit(b':', 1)[1] != b'0':
                        raise AssertionError(normalized.decode(errors='replace'))
                found = normalized.find(marker, offset)
                if found >= 0:
                    result = normalized[offset:found]
                    offset = found + len(marker)
                    return result
                if time.monotonic() >= deadline or guest.poll() is not None:
                    raise AssertionError(f'Waiting for {marker!r}:\n' + normalized.decode(errors='replace'))
                if select.select([guest.stdout], [], [], .05)[0]:
                    chunk = os.read(guest.stdout.fileno(), 65536)
                    if not chunk:
                        raise AssertionError(normalized.decode(errors='replace'))
                    transcript.extend(chunk)
                    log.write_bytes(transcript)

        def command(text):
            if master is not None:
                os.write(master, text.encode() + b'\r')
            else:
                guest.stdin.write(text.encode() + b'\n')
                guest.stdin.flush()

        try:
            expect(prompt)
            command(f'{program} -version; echo VERSION_STATUS:$?')
            version = expect(b'\nVERSION_STATUS:0\n')
            assert b'Lynx Version 2.9.2' in version, version
            expect(prompt)
            for page, required in [('local.html', [b'Ripple browser', b'Guest HTML rendering works.',
                                                   b'Entities: & < > "', b'First item', b'Second item',
                                                   b'relative link', b'preserved   spacing', b'C16']),
                                   ('next.html', [b'Following local links works', b'Back'])]:
                command(f'{program} -dump -nolist -width=60 -cfg={pages}/lynx.cfg '
                        f'{pages}/{page}; echo DUMP_STATUS:$?')
                rendered = expect(b'\nDUMP_STATUS:0\n')
                for text in required:
                    assert text in rendered, (text, rendered)
                assert b'<html' not in rendered and b'<h1>' not in rendered, rendered
                expect(prompt)
            command('shutdown')
            assert guest.wait(timeout=15) == 0
        finally:
            if guest.poll() is None:
                guest.kill()
                guest.wait()
            if master is not None:
                os.close(master)
            log.write_bytes(transcript)
    print(('Native Fuse-X' if native else 'RVM/Riplix') + ': Lynx version and two local HTML dumps passed')


if __name__ == '__main__':
    main()
