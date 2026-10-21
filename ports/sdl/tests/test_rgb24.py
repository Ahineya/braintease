#!/usr/bin/env python3
"""Compare RGB24 bytes against upstream and the actual Fuse-X renderer."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/sdl'
sys.path.insert(0, str(ROOT))
from ports.sdl.build import library


def execute(command, log):
    with log.open('w') as output:
        result = subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(log.read_text()[-8000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sdl/rgb24')
    parser.add_argument('--sdl-build', type=Path, default=ROOT / 'build/sdl')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--guest', action='store_true')
    args = parser.parse_args()
    work = args.build.resolve()
    work.mkdir(parents=True, exist_ok=True)
    lib = library('host', work / 'reference', args.compiler.resolve())
    host = work / 'rgb24-host'
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2', '-fno-strict-aliasing']
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT')
        available = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
        if not sdk and available.exists():
            sdk = str(available)
        if sdk:
            command += ['-isysroot', sdk]
    command += [part for path in lib['includes'] for part in ['-I', str(path)]]
    command += [*map(str, lib['units']), str(PORT / 'c16/pixels.c'), str(PORT / 'tests/rgb24.c'), '-o', str(host)]
    execute(command, work / 'host-build.log')
    reference = subprocess.check_output([str(host)], text=True)
    actual = subprocess.check_output([str(host), '--adapter'], text=True)
    assert reference == actual, (reference, actual)
    (work / 'reference.log').write_text(reference)
    (work / 'host-adapter.log').write_text(actual)
    results = dict(host='passed', summary=reference.strip())
    if args.guest:
        build = args.sdl_build.resolve()
        build.mkdir(parents=True, exist_ok=True)
        with (build / '.build.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            lib = library('fuse-x', build, args.compiler.resolve())
            guest_app = work / 'rgb24.fxe'
            command = [sys.executable, ROOT / 'fuse-x/cc.py', PORT / 'tests/rgb24.c',
                       '--compiler', lib['compiler'], '--static', '--build', build / 'sdk', '-o', guest_app]
            for path in lib['includes']:
                command += ['-I', str(path)]
            for path in lib['objects']:
                command += ['--object-ir', path]
            execute(command, work / 'guest-build.log')
        sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
        from guest import Guest
        with tempfile.TemporaryDirectory(prefix='sdl-rgb24-') as directory:
            guest = Guest(directory, 'native', {'rgb24': guest_app}, blocks=32768)
            try:
                output = guest.shell('rgb24; echo SDL_RGB24_STATUS:$?', seconds=180)
                assert reference.strip() in output and 'SDL_RGB24_STATUS:0' in output, output
                (work / 'guest.log').write_text(output)
            finally:
                guest.close()
        results['guest'] = 'passed'
    (work / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(reference.strip() + (' on upstream, host adapter and Fuse-X' if args.guest else ' on upstream and host adapter'))


if __name__ == '__main__':
    main()
