#!/usr/bin/env python3
"""Compile portable-frontend C sources into FUZIX/Ripple RXE executables."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from fuse_x import INCLUDES

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PORT = ROOT / 'ports/fuzix'


def checked(command, **options):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **options)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path, nargs='+')
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('-I', dest='includes', action='append', default=[])
    parser.add_argument('-D', dest='defines', action='append', default=[])
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--float-format', action='store_true')
    parser.add_argument('--backend',choices=['rust','portable'],default='rust')
    parser.add_argument('--map', type=Path, help='write RXE symbol locations as JSON')
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.output in [source.resolve() for source in args.sources]:
        parser.error('output must not overwrite an input source')
    if args.map:
        args.map = args.map.resolve()
        if args.map in [args.output, *[source.resolve() for source in args.sources]]:
            parser.error('symbol map must be separate from the executable and source files')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    build = args.build.resolve()
    marker = build / '.mmu'
    if not marker.exists() or marker.read_text().strip() != '1':
        raise RuntimeError('build the FUZIX MMU profile first with scripts/run-fuzix.sh --build-only')
    make = ['make', '--no-print-directory', '-C', PORT, f'BUILD={build}', f'RCC={args.compiler.resolve()}']
    checked([*make, 'rfl-sdk', *(['float-sdk'] if args.float_format else [])])
    sdk = json.loads(checked([*make, '-s', 'rfl-sdk-info']))
    libraries = list(map(Path, sdk['libraries']))
    if args.float_format:
        floating = json.loads(checked([*make, '-s', 'float-sdk-info']))
        libraries = [p for p in libraries if p != build / 'libc/format.pobj']
        libraries += list(map(Path, floating['libraries']))
    assembler = ROOT / 'src/ripple-asm/target/release'
    with tempfile.TemporaryDirectory(prefix='.portable-rxe-', dir=args.output.resolve().parent) as temporary:
        work = Path(temporary)
        objects = []
        for index, source in enumerate(args.sources):
            ir, assembly, obj = [work / f'unit{index}.{suffix}' for suffix in ['ir', 'asm', 'pobj']]
            command = [sys.executable, HERE / 'cc.py', source, '--target', 'ripple', '--emit-ir',
                       '--rcc', args.compiler, '-o', ir, '-DRIPPLE_MMU=1']
            if not args.float_format:
                command.append('-DNO_64BIT')
            for include in [*args.includes, *INCLUDES]:
                command += ['-I', str(include)]
            for define in args.defines:
                command += ['-D', define]
            checked(command)
            try:
                if args.backend=='portable':
                    from build_ripple import ensure_host
                    checked([ensure_host(),'--unit',str(source.resolve()),ir,'-o',assembly])
                else:
                    checked([args.compiler, 'generate-asm', '-i', ir, '-o', assembly])
                checked([assembler / 'rasm', 'assemble', assembly, '-o', obj,
                         '--bank-size', '64000', '--max-immediate', '65535','--memory-offset','0'])
            except RuntimeError as error:
                raise RuntimeError(f'{source}:\n{error}') from error
            objects.append(obj)
        package = work / args.output.name
        map_args = ['--map', work / 'symbols.json'] if args.map else []
        checked([assembler / 'rlink', sdk['entry'], *objects, *libraries,
                 '-f', 'rxe', '--bank-size', '64000', '-o', package, *map_args])
        if args.map:
            args.map.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=args.map.parent, delete=False) as file:
                temporary_map = Path(file.name)
                file.write((work / 'symbols.json').read_bytes())
            try:
                os.replace(temporary_map, args.map)
            finally:
                temporary_map.unlink(missing_ok=True)
        package.chmod(package.stat().st_mode | 0o111)
        os.replace(package, args.output)
    print(f'Built portable-frontend FUZIX executable: {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'portable FUZIX compiler: {error}', file=sys.stderr)
        sys.exit(1)
