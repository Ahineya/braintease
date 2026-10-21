#!/usr/bin/env python3
"""Compare upstream SDL 1.2 software blitters with the C16 pixel adapter."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from adapt import COMMON, prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sdl/pixel-matrix')
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = prepare(build, args.fetch, False)
    units = [source / name for name in COMMON if not name.endswith('SDL_string.c')]
    units += [PORT / 'platform/support.c', PORT / 'platform/host.c', PORT / 'c16/pixels.c']
    units += [source / 'src/video' / name for name in
              ['SDL_blit_0.c', 'SDL_blit_1.c', 'SDL_blit_N.c', 'SDL_blit_A.c', 'SDL_RLEaccel.c']]
    units += [source / 'src/video/dummy' / name for name in
              ['SDL_nullvideo.c', 'SDL_nullevents.c', 'SDL_nullmouse.c']]
    includes = [source / name for name in ['include', 'src', 'src/video', 'src/events', 'src/timer']]
    includes += [PORT / 'platform', PORT / 'c16']
    output = build / 'pixel-matrix'
    command = [os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-fno-strict-aliasing']
    sdk = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
    if sys.platform == 'darwin' and sdk.exists():
        command += ['-isysroot', str(sdk)]
    command += [part for directory in includes for part in ['-I', str(directory)]]
    command += [str(path) for path in units]
    command += [str(PORT / 'tests/pixel-matrix.c'), '-o', str(output)]
    with (build / 'build.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit((build / 'build.log').read_text()[-6000:])
    subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
