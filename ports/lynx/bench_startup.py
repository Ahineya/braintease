#!/usr/bin/env python3
"""Measure installed Lynx startup on disposable Fuse-X disks, with networking off.

Run without concurrent builds for comparable wall times. --kernel can select a
saved Fuse-X executable; --build supplies the same apps to both measurements.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

from guest import native_image
from probe import PORT, ROOT
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--repeat', type=int, default=3)
    args = parser.parse_args()
    if args.repeat < 1: parser.error('--repeat must be positive')
    build = args.build.resolve()
    kernel = (args.kernel or build / 'fuse-x').resolve()
    executable = (args.executable or build / 'apps/lynx.fxe').resolve()
    with tempfile.TemporaryDirectory(prefix='lynx-startup-', dir='/tmp') as temporary:
        image = Path(temporary) / 'root.img'
        native_image(image, build / 'fuse-x', ('sh', 'echo'), 65535)
        subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image),
                        '/bin/lynx', str(executable)], check=True, stdout=subprocess.DEVNULL)
        guest = Guest([kernel, '--run', image, '--share', PORT / 'fixtures'], 'fuse-x')
        measurements = {'version': [], 'local_dump': [], 'local_first_frame': []}
        try:
            guest.raw(guest.prompt)
            for repeat in range(args.repeat):
                for name, command, required in [
                    ('version', 'lynx -version', b'Lynx Version'),
                    ('local_dump', 'lynx -dump -nolist -cfg=/mnt/host/lynx.cfg /mnt/host/local.html', b'Guest HTML rendering works.')]:
                    start = time.perf_counter()
                    guest.send((command + '; echo LYNX_STATUS:$?\r').encode())
                    output = guest.raw(b'LYNX_STATUS:0\r\n', seconds=180)
                    elapsed = time.perf_counter() - start
                    assert required in output, output
                    guest.raw(guest.prompt)
                    measurements[name].append(elapsed)
                start = time.perf_counter()
                guest.send(b'lynx -cfg=/mnt/host/lynx.cfg /mnt/host/local.html\r')
                guest.wait(lambda: 'Ripple browser' in guest.screen.text()
                           and 'Commands:' in guest.screen.text()
                           and guest.screen.y == 8 and guest.screen.x == 4,
                           'complete local page and final link cursor', 180)
                measurements['local_first_frame'].append(time.perf_counter() - start)
                guest.send(b'q')
                guest.visible('Are you sure')
                guest.send(b'y')
                guest.raw(guest.prompt)
            guest.send(b'shutdown\r')
            assert guest.process.wait(timeout=30) == 0
        finally:
            guest.close()
        for name, values in measurements.items():
            print(json.dumps(dict(test=name, seconds=values, median_seconds=statistics.median(values),
                kernel=str(kernel), executable=str(executable))), flush=True)


if __name__ == '__main__':
    main()
