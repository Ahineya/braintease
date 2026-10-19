#!/usr/bin/env python3
"""Exercise the installed pkill against real guest processes on private disks."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    build = (args.native_build if args.native else args.build).resolve()
    with tempfile.TemporaryDirectory(prefix='pkill-check-') as temporary:
        work = Path(temporary)
        image = work / 'disk.img'
        stage = work / 'apps'
        stage.mkdir()
        names = ['sh', 'echo', 'pkill', 'pkillcheck']
        if args.native:
            subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'),
                            str(PORT / 'tools/pkillcheck.c'), '--build', str(build),
                            '-o', str(stage / 'pkillcheck.fxe')], check=True)
            for name in ['sh', 'echo', 'pkill', 'native-hello']:
                (stage / (name + '.fxe')).symlink_to(build / 'apps' / (name + '.fxe'))
            (work / 'application-report.json').write_text(json.dumps([
                dict(name=name, status='built') for name in names]))
            subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                            '--apps', str(stage)], check=True)
            command = [str(build / 'fuse-x'), '--run', str(image), '--one-shot']
        else:
            subprocess.run(['make', '-s', '-C', str(PORT), f'BUILD={build}',
                            str(build / 'pkillcheck.rxe')], check=True)
            names += ['init']
            for name in names:
                (stage / (name + '.rxe')).symlink_to(build / (name + '.rxe'))
            (stage / 'apps.json').write_text(json.dumps(names))
            (stage / 'FUZIX.SYS').symlink_to(build / 'FUZIX.SYS')
            subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(stage), str(image)], check=True)
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                       '--memory', '16384000', '--headless', '--net', 'off', '--jit', '--timeout', '90']
        commands = 'pkillcheck\necho PKILL_STATUS:$?\nshutdown\n'
        if not args.native:
            command += ['--input', commands]
        result = subprocess.run(command, input=commands if args.native else None,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=100, env=dict(os.environ, RCC_NATIVE_HEADLESS='1',
                                RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='90'))
        (build / 'pkill-test.log').write_text(result.stdout)
        assert result.returncode == 0 and 'PKILL_OK 0 failures' in result.stdout, result.stdout[-8000:]
        assert 'PKILL_STATUS:0' in result.stdout and 'PKILL_FAILED' not in result.stdout, result.stdout[-8000:]
        print(('Fuse-X' if args.native else 'RVM JIT') + ': real-process pkill checks passed')


if __name__ == '__main__':
    main()
