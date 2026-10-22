#!/usr/bin/env python3
"""Focused inactive adapter host UBSan + freestanding x86 object check."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCES = [ROOT / 'tools/x86_64/process-memory/process_memory.c',
           ROOT / 'tools/x86_64/boot/process_memory_fixture.c',
           ROOT / 'tools/x86_64/tests/process-memory-fixture.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--sysroot', type=Path, default=os.environ.get('SDKROOT'))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/x86_64-process-memory-fixture')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'result.json').unlink(missing_ok=True)
    cc = shlex.split(args.cc)
    common = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
              '-DX64_PROCESS_MEMORY_FIXTURE']
    with tempfile.TemporaryDirectory(prefix='host-', dir=args.output) as temp, \
            (args.output / 'host.log').open('w') as log:
        temp = Path(temp)

        def run(command, check=True):
            log.write(shlex.join(command) + '\n')
            log.flush()
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            log.write(result.stdout + result.stderr)
            log.flush()
            if check and result.returncode:
                raise SystemExit(f'Fixture check failed: see {args.output / "host.log"}')
            return result

        # No SDK or executable kernel link is needed for freestanding sources.
        for source in SOURCES[:2]:
            run([*cc, *common, '--target=x86_64-unknown-none-elf',
                 '-ffreestanding', '-fno-builtin', '-mno-red-zone',
                 '-mgeneral-regs-only', '-c', str(source), '-o', str(temp / (source.stem + '.o'))])
        negative = run([*cc, '--target=x86_64-unknown-none-elf', '-ffreestanding',
                        '-c', str(SOURCES[1]), '-o', str(temp / 'disabled.o')], check=False)
        assert negative.returncode and 'explicit acceptance build' in negative.stderr
        host = [*cc, *common]
        if args.sysroot:
            host += ['-isysroot', str(args.sysroot.resolve())]
        run([*host, '-fsanitize=undefined', '-fno-sanitize-recover=all',
             *map(str, SOURCES), '-o', str(temp / 'fixture')])
        result = run([str(temp / 'fixture')])
    report = json.loads(next(line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT ')))
    all_sources = [*SOURCES, ROOT / 'tools/x86_64/boot/process_memory_fixture.h',
                   ROOT / 'tools/x86_64/process-memory/process_memory.h', Path(__file__).resolve()]
    report.update(status='passed', scope='host callbacks; inactive platform fixture; no production activation',
                  sanitizer='undefined, fatal', host_architecture=platform.machine(),
                  freestanding_x86_compile=True, disabled_build_rejected=True,
                  sources={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in all_sources})
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
