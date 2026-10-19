#!/usr/bin/env python3
"""Run the same dependency acceptance source with the host C compiler."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from adapt import prepare
from build import ROOT, LIBRARIES, sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/netsurf-host')
    parser.add_argument('--sanitize', action='store_true', help='Enable host AddressSanitizer')
    parser.add_argument('--timeout', type=float, default=60, help='Acceptance execution timeout in seconds')
    args = parser.parse_args()
    work = args.build.resolve()
    source = prepare(work / 'source')
    output = work / 'objects'
    output.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c99', '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-DWITHOUT_ICONV_FILTER', '-DNDEBUG',
             '-include', 'strings.h',
             '-I' + str(PORT / 'config')]
    flags += ['-I' + str(source / lib / 'include') for lib in LIBRARIES]
    flags += ['-I' + str(source / 'libdom/bindings')]
    if args.sanitize:
        flags += ['-fsanitize=address', '-fno-omit-frame-pointer', '-g']
    def compile_one(task):
        library, path = task
        obj = output / (library + '_' + '_'.join(path.relative_to(source / library).parts) + '.o')
        result = subprocess.run(['cc', *flags, '-I' + str(source / library / 'src'),
                                 '-c', str(path), '-o', str(obj)], capture_output=True)
        obj.with_suffix('.log').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f'Host compile failed: {obj.with_suffix(".log")}')
        return obj
    with ThreadPoolExecutor(max_workers=3) as pool:
        objects = list(pool.map(compile_one, sources(source)))
    executable = work / 'netsurfcheck'
    subprocess.run(['cc', *flags, str(PORT / 'tests/dependencies.c'), *map(str, objects),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=args.timeout)


if __name__ == '__main__':
    main()
