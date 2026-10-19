#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure drawing in a disposable RVM guest using existing built Nano-X objects.

Does not rebuild/install the desktop or open the user's disk. Timings include
IPC, server drawing and a server acknowledgement, not host scanout latency.
"""
import argparse
import json
import os
import platform
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def run(*args, **options):
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT, **options)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--width', type=int, default=320)
    parser.add_argument('--height', type=int, default=220)
    parser.add_argument('--samples', type=int, default=12)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fusede/profile')
    args = parser.parse_args()
    if not 16 <= args.width <= 608 or not 32 <= args.height <= 448:
        parser.error('width must be 16..608 and height 32..448')
    if not 4 <= args.samples <= 100:
        parser.error('samples must be 4..100')
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix('.c')
    app = output / 'fdeprofile.rxe'
    makefile = output / 'profile.mk'
    # Only build this probe. The desktop objects under investigation stay fixed.
    makefile.write_text(f'''include {ROOT}/ports/fuzix/Makefile
.PHONY: fde-profile
fde-profile:
\t$(RCC) compile {source} -o {output}/profile.asm $(NX_DESKTOP_FLAGS) -DPROFILE_WIDTH={args.width} -DPROFILE_HEIGHT={args.height} -DPROFILE_SAMPLES={args.samples}
\t$(RASM) assemble {output}/profile.asm -o {output}/profile.pobj --bank-size 64000 --max-immediate 65535
\t$(RLINK) $(BUILD)/user/entry.pobj {output}/profile.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) -f rxe --bank-size 64000 -o {app}
''')
    with (output / 'build.log').open('w') as log:
        run('make', '-s', '-C', ROOT / 'ports/fuzix', '-f', makefile, f'BUILD={build}', 'fde-profile',
            stdout=log, stderr=subprocess.STDOUT)
    with tempfile.TemporaryDirectory(prefix='fusede-profile-') as directory:
        work = Path(directory)
        image = work / 'test.img'
        run('node', ROOT / 'ports/fusede/tools/test_image.mjs', build, image, 'fdeprofile', app)
        run('python3', ROOT / 'ports/fuzix/tools/fsck.py', image)
        commands = ('nano-X -p &\ns=$!\nsleep 1\nfdeprofile\necho PROFILE_STATUS:$?\n'
                    'kill $s\nwait $s\nshutdown\n')
        with (output / 'rvm.log').open('w') as log:
            run(ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                '--memory', '16384000', '--headless', '--jit', '--net', 'off',
                '--input', commands, '--report', output / 'execution.json', '--timeout', '300', stdout=log, stderr=subprocess.STDOUT, timeout=320,
                env=dict(os.environ, RIPPLE_NET='off'))
        text = (output / 'rvm.log').read_text().replace('\r\n', '\n')
        if '\nPROFILE_STATUS:0\n' not in text or 'FDE_PROFILE_DONE' not in text:
            raise RuntimeError(text[-8000:])
        samples = {}
        for name, iteration, ms in re.findall(r'^FDE_PROFILE (\w+) (\d+) (\d+)$', text, re.M):
            samples.setdefault(name, []).append(dict(iteration=int(iteration), ms=int(ms)))
        expected = ['fill_rectangle', 'fill_rows', 'skin_frame', 'cached_frame_copy', 'overlap_copy', 'move_window',
                    'offscreen_fill', 'offscreen_skin', 'single_pixel', 'fence_only']
        if len(samples.get('theme_load', [])) != 1 or any(len(samples.get(n, [])) != args.samples for n in expected):
            raise RuntimeError('incomplete profiling output')
        report = dict(build=str(build), width=args.width, height=args.height, samples=samples,
                      host=platform.platform(), machine=platform.machine(),
                      note='Headless JIT RVM without NanoWM; server completion, not physical input-to-display latency. '
                           'Early samples can include compilation when a path becomes hot; compare the last five samples.')
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        for name, rows in samples.items():
            warm = [r['ms'] for r in rows[max(1, len(rows) - 5):]]
            print(f'{name}: first {rows[0]["ms"]} ms' +
                  (f', tail median {statistics.median(warm):g} ms' if warm else ''), flush=True)
        run('python3', ROOT / 'ports/fuzix/tools/fsck.py', image)


if __name__ == '__main__':
    main()
