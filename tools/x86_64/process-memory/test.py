#!/usr/bin/env python3
"""Run the isolated model under host UBSan; retain results, remove binaries."""
import argparse
import json
import os
import platform
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--sysroot', type=Path, default=os.environ.get('SDKROOT'),
                        help='SDK path when the host compiler needs an explicit sysroot')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/x86_64-process-memory')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = output / 'result.json'
    result.unlink(missing_ok=True)
    with (output / 'host.log').open('w') as log, tempfile.TemporaryDirectory(prefix='host-', dir=output) as temporary:
        temporary = Path(temporary)
        common = [*shlex.split(args.cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        if args.sysroot:
            common += ['-isysroot', str(args.sysroot.resolve())]
        commands = [
            [*common, '-ffreestanding', '-fno-builtin', '-c', str(HERE / 'process_memory.c'), '-o', str(temporary / 'model.o')],
            [*common, '-fsanitize=undefined', '-fno-sanitize-recover=all', str(HERE / 'process_memory.c'),
             str(HERE / 'test.c'), '-o', str(temporary / 'test')],
            [str(temporary / 'test')],
        ]
        for command in commands:
            log.write(shlex.join(command) + '\n')
            log.flush()
            run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
            log.write(run.stdout)
            log.flush()
            if run.returncode:
                raise SystemExit(f'Process-memory check failed; see {output / "host.log"}')
        marker = next(line[7:] for line in run.stdout.splitlines() if line.startswith('RESULT '))
        report = json.loads(marker)
        report.update(status='passed', scope='isolated host model; no production integration',
                      sanitizer='undefined, fatal', host_architecture=platform.machine(),
                      host_platform=sys.platform, freestanding_compile=True,
                      test_command=shlex.join([sys.executable, *sys.argv]))
        result.write_text(json.dumps(report, indent=2) + '\n')
        print(run.stdout, end='')
        print(f'Results: {result}')


if __name__ == '__main__':
    main()
