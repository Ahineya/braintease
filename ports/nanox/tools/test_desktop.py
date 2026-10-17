#!/usr/bin/env python3
"""Exercise the real Nano-X desktop on JIT RVM with an isolated small disk."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--only', choices=('all', 'desktop', 'roach'), default='all')
    args = parser.parse_args()
    build = args.build.resolve()
    cases = {'desktop': 'desktop_clients_input_resize_and_recovery',
             'roach': 'roaches_masks_animation_occlusion_and_cleanup'}
    for case, test in cases.items():
        if args.only not in ('all', case):
            continue
        with tempfile.TemporaryDirectory(prefix=f'nanox-{case}-') as directory:
            image = Path(directory) / 'root.img'
            subprocess.run(['node', str(ROOT / 'ports/nanox/tools/desktop-image.mjs'),
                            str(build), str(image)], check=True)
            subprocess.run(['cargo', 'test', '--release', '-p', 'rvm', '--test',
                            'nanox_desktop', test, '--', '--ignored', '--exact', '--nocapture'], cwd=ROOT,
                           env=dict(os.environ, NANOX_TEST_BUILD=str(build),
                                    NANOX_DESKTOP_IMAGE=str(image)), check=True)
            subprocess.run(['python3', str(ROOT / 'ports/fuzix/tools/fsck.py'),
                            str(image)], check=True)


if __name__ == '__main__':
    main()
