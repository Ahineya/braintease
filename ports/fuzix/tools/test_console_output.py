#!/usr/bin/env python3
"""Check exact console output or benchmark 128 KiB on a disposable guest disk.

Build the selected kernel and consolecheck first. No active user disk is opened.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest


def image(system, build, executable, directory):
    path = directory / 'disk.img'
    if system == 'fuse-x':
        sys.path.insert(0, str(ROOT / 'ports/lynx'))
        from guest import native_image
        native_image(path, build / 'fuse-x')
        subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(path),
                        '/bin/consolecheck', str(executable)], check=True, stdout=subprocess.DEVNULL)
    else:
        for name in ['init', 'sh', 'echo', 'consolecheck']:
            (directory / (name + '.rxe')).symlink_to(executable if name == 'consolecheck' else build / (name + '.rxe'))
        (directory / 'FUZIX.SYS').symlink_to(build / 'FUZIX.SYS')
        (directory / 'apps.json').write_text(json.dumps(['init', 'sh', 'echo', 'consolecheck']))
        subprocess.run(['node', str(ROOT / 'ports/fuzix/tools/disk.mjs'), str(directory), str(path)],
                       check=True, stdout=subprocess.DEVNULL)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['fuzix', 'fuse-x'], required=True)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--kernel', type=Path, help='alternate Fuse-X executable or RVM executable')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--bench', action='store_true')
    args = parser.parse_args()
    build = (args.build or ROOT / ('build/fuzix' if args.os == 'fuzix' else 'fuse-x/build')).resolve()
    executable = (args.executable or (build / ('consolecheck.rxe' if args.os == 'fuzix' else 'apps/consolecheck.fxe'))).resolve()
    with tempfile.TemporaryDirectory(prefix='console-output-', dir='/tmp') as temporary:
        disk = image(args.os, build, executable, Path(temporary))
        kernel = (args.kernel or (ROOT / 'target/release/rvm' if args.os == 'fuzix' else build / 'fuse-x')).resolve()
        command = [kernel, '--run', disk] if args.os == 'fuse-x' else [kernel, build / 'bios.bin',
            '--disk', disk, '--memory', '16384000', '--jit', '--net', 'off', '--timeout', '600']
        guest = Guest(command, args.os)
        try:
            guest.raw(guest.prompt)
            samples = []
            for repeat in range(3 if args.bench else 1):
                guest.send(('consolecheck' + (' --bench' if args.bench else '') + '; echo OUTPUT_STATUS:$?\r').encode())
                guest.raw(b'CONSOLE_BEGIN', seconds=180)
                start = time.perf_counter()
                output = guest.raw(b'CONSOLE_END', seconds=180)
                samples.append(time.perf_counter() - start)
                if args.bench:
                    assert output == b'\n' + b'x' * 131072 + b'\nCONSOLE_END', (len(output), output[-200:])
                else:
                    # Guest.raw strips CRs, so inspect the unnormalized log for
                    # all 256 octets rather than its convenient text result.
                    expected = bytes(range(256)) * 4 + b'stdio/write/stderr'
                    assert expected in guest.log, bytes(guest.log[-2000:])
                    assert b'POST\n' in output, output[-200:]
                result = guest.raw(guest.prompt)
                assert b'OUTPUT_STATUS:0\n' in result, result
            if args.bench:
                print(json.dumps(dict(system=args.os, octets=131072,
                    seconds=samples, median_seconds=statistics.median(samples))), flush=True)
            else:
                print(args.os + ': bank-spanning raw octets, stdio/write/stderr ordering and output processing passed')
            guest.send(b'shutdown\r')
            assert guest.process.wait(timeout=30) == 0
        finally:
            guest.close()


if __name__ == '__main__':
    main()
