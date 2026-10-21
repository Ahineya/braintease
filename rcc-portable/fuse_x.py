#!/usr/bin/env python3
"""Compile portable-frontend C sources into a loadable Fuse-X executable.

Application code uses the portable frontend. Startup, libc and FXE packaging
still use the existing hosted SDK: this is a host-assisted bootstrap step.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PORT = ROOT / 'ports/fuzix'
INCLUDES = [PORT / 'ripple/include', PORT / 'upstream/Library/include',
            ROOT / 'runtime/include', PORT / 'ripple', ROOT / 'ports/graphics/include']
ENTRY = '__rcc_portable_program_main'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path, nargs='+')
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('-I', dest='includes', action='append', default=[])
    parser.add_argument('-D', dest='defines', action='append', default=[])
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--float-format', action='store_true')
    parser.add_argument('--linker', type=Path, help='explicit application packaging adapter')
    parser.add_argument('--heap-profile', choices=['hosted','bounded'])
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.output in [source.resolve() for source in args.sources]:
        parser.error('output must not overwrite an input source')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='portable-fuse-x-') as temporary:
        directory = Path(temporary)
        objects = []
        arity = None
        for index, source in enumerate(args.sources):
            output = directory / f'unit{index}.ir'
            command = [sys.executable, str(HERE / 'cc.py'), str(source), '--target', 'native',
                       '--emit-ir', '--rcc', str(args.compiler), '-o', str(output), '-DRIPPLE_MMU=1']
            if not args.float_format:
                command.append('-DNO_64BIT')
            for include in [*args.includes, *INCLUDES]:
                command += ['-I', str(include)]
            for define in args.defines:
                command += ['-D', define]
            subprocess.run(command, check=True)
            ir = output.read_text()
            if f'@"{ENTRY}"' in ir:
                raise RuntimeError(f'{ENTRY} is reserved for the hosted startup adapter')
            match = re.search(r'\bfunction i16 @"main"\(([^)]*)\)\s*\{', ir)
            if match:
                if arity is not None:
                    raise RuntimeError('multiple main definitions')
                arity = len(match[1].split(',')) if match[1].strip() else 0
            output.write_text(ir.replace('@"main"', f'@"{ENTRY}"'))
            objects.append(output)
        if arity not in (0, 2, 3):
            raise RuntimeError('main must return int and accept (), (argc, argv), or (argc, argv, envp)')
        parameters = ['int', 'char **', 'char **'][:arity]
        values = ['argc', 'argv', 'envp'][:arity]
        startup = directory / 'entry.c'
        startup.write_text(f'extern int {ENTRY}({",".join(parameters) or "void"});\n'
                           'int main(int argc,char **argv,char **envp) {\n'
                           f'    return {ENTRY}({",".join(values)});\n}}\n')
        # Publish only after both frontends and the linker have succeeded.
        # Keep the staging file on the destination filesystem for atomic rename.
        with tempfile.TemporaryDirectory(prefix='.portable-fxe-', dir=args.output.resolve().parent) as staging:
            package = Path(staging) / args.output.name
            command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(startup),
                       '--compiler', str(args.compiler), '--build', str(args.build), '-o', str(package)]
            if args.float_format:
                command.append('--float-format')
            if args.linker:
                command += ['--static', '--linker', str(args.linker.resolve())]
            if args.heap_profile:
                command += ['--heap-profile', args.heap_profile]
            for obj in objects:
                command += ['--object-ir', str(obj)]
            subprocess.run(command, check=True, stdout=sys.stderr)
            package.chmod(package.stat().st_mode | 0o111)
            os.replace(package, args.output)
    print(f'Built portable-frontend Fuse-X executable: {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'portable Fuse-X compiler: {error}', file=sys.stderr)
        sys.exit(1)
