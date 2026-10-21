#!/usr/bin/env python3
"""Test the production SDL cooperative PCM core against deterministic services."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from adapt import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sdl/audio-backend')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve();build.mkdir(parents=True, exist_ok=True)
    source = prepare(build, False, False)
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2', '-fno-strict-aliasing']
    if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    sdk = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
    if sys.platform == 'darwin' and sdk.exists(): command += ['-isysroot', str(sdk)]
    command += ['-I' + str(p) for p in [source/'include', PORT/'platform', PORT/'c16']]
    command += [str(p) for p in [PORT/'platform/audio.c', PORT/'c16/audio_formats.c', PORT/'tests/audio-backend.c']]
    output = build / 'audio-backend';command += ['-o', str(output)]
    compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (build/'build.log').write_text(compiled.stdout+compiled.stderr)
    if compiled.returncode: raise SystemExit(compiled.stderr[-5000:])
    try:
        result = subprocess.run([str(output)], text=True, capture_output=True, timeout=20)
    except subprocess.TimeoutExpired:
        (build/'result.txt').write_text('TIMEOUT: audio backend fixture exceeded 20 seconds\n')
        raise SystemExit('Audio backend fixture timed out after 20 seconds')
    (build/'result.txt').write_text(result.stdout+result.stderr)
    if result.returncode: raise SystemExit(result.stderr[-5000:])
    print(result.stdout, end='')


if __name__ == '__main__': main()
