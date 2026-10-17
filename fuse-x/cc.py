#!/usr/bin/env python3
"""Compile C sources into an independently loadable Fuse-X native executable."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PORT = ROOT / 'ports/fuzix'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path, nargs='+')
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('-I', dest='includes', action='append', default=[])
    parser.add_argument('-D', dest='defines', action='append', default=[])
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--linker', type=Path)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--float-format', action='store_true', help='enable floating point libc formatting for awk')
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    linker = (args.linker or compiler).resolve()
    includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include',
                ROOT / 'runtime/include', PORT / 'ripple', ROOT / 'ports/graphics/include']
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT']
    if args.float_format:
        flags.remove('-DNO_64BIT')
    for directory in includes:
        flags += ['-I', str(directory)]
    cache = args.build.resolve() / 'sdk'
    cache.mkdir(parents=True, exist_ok=True)
    excluded = set('regerror malloc calloc realloc readline readline_fn __sin __cos '
                   '__rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow '
                   'sin cos atan atan2 strtod ecvt fmod frexp ldexp modf'.split())
    libraries = [PORT / f'ripple/libc/{name}.c'
                 for name in 'syscalls stdio format dirent misc heap pipe sockets resolver'.split()]
    libraries += sorted(p for p in (PORT / 'upstream/Library/libs').glob('*.c')
                        if p.stem not in excluded)
    libraries += [ROOT / 'fuse-x/libc/bridge.c', PORT / 'ripple/userlib.c']
    if args.float_format:
        libraries += [PORT / f'upstream/Library/libs/{name}.c' for name in
                      '__sin __cos __rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow sin cos atan atan2 strtod ecvt fmod frexp ldexp modf'.split()]
    digest = hashlib.sha256()
    digest.update(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for header in sorted(directory.rglob('*.h')):
            digest.update(str(header).encode())
            digest.update(header.read_bytes())
    digest.update((PORT / "upstream/Kernel/include/userstructs.h").read_bytes())
    fingerprint = digest.digest()

    def compile_library(source):
        library_flags = flags + (['-DRIPPLE_FLOAT_FORMAT'] if args.float_format and source.name == 'format.c' else [])
        key = hashlib.sha256(fingerprint + str((source, library_flags)).encode() + source.read_bytes()).hexdigest()
        output = cache / f'{source.stem}-{key[:24]}.ir'
        if not output.exists():
            result = subprocess.run([str(compiler), 'compile', str(source), *library_flags, '--emit-ir'],
                                    capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{source}:\n{result.stderr.decode(errors="replace")}')
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as file:
                file.write(result.stdout)
                temporary = file.name
            os.replace(temporary, output)
        return output

    with ThreadPoolExecutor(max_workers=4) as pool:
        library_ir = list(pool.map(compile_library, libraries))
    with tempfile.TemporaryDirectory(prefix='fuse-x-cc-') as temporary:
        directory = Path(temporary)
        inputs = []
        main_arity = None
        # Keep the C entry named main through frontend lowering: C99 permits
        # it to fall through with status zero. Rename the resulting IR symbol
        # rather than losing that rule with a preprocessor macro.
        user_flags = list(flags)
        for include in args.includes:
            user_flags += ['-I', include]
        for define in args.defines:
            user_flags += ['-D', define]
        for index, source in enumerate(args.sources):
            result = subprocess.run([str(compiler), 'compile', str(source), *user_flags,
                                     '--emit-ir'], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
            match = re.search(r'function\s+\S+\s+@main\(([^)]*)\)\s*\{', result.stdout)
            if match:
                if main_arity is not None:
                    raise RuntimeError('multiple main definitions')
                main_arity = len(match[1].split(',')) if match[1].strip() else 0
            output = directory / f'input{index}.ir'
            output.write_text(re.sub(r'@main\b', '@fuse_program_main', result.stdout))
            inputs.append(output)
        if main_arity not in (0, 2, 3):
            raise RuntimeError('main must accept (), (argc, argv), or (argc, argv, envp)')
        parameters = ['int', 'char **', 'char **'][:main_arity]
        values = ['a->argc', 'a->argv', 'a->envp'][:main_arity]
        startup = directory / 'startup.c'
        startup.write_text('#include <stdlib.h>\n'
                           'struct arguments { unsigned argc; char **argv, **envp; };\n'
                           'extern void ripple_libc_init(int,char **,char **);\n'
                           f'extern int fuse_program_main({",".join(parameters) or "void"});\n'
                           'void main(unsigned long pointer) {\n'
                           'struct arguments *a=(void *)pointer;\n'
                           'ripple_libc_init(a->argc,a->argv,a->envp);\n'
                           f'exit(fuse_program_main({",".join(values)}));\n}}\n')
        result = subprocess.run([str(compiler), 'compile', str(startup), *flags, '--emit-ir'],
                                capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors='replace'))
        startup_ir = directory / 'startup.ir'
        startup_ir.write_bytes(result.stdout)
        subprocess.run([str(linker), 'link-app', str(startup_ir), *map(str, inputs),
                        *map(str, library_ir), '-o', str(args.output)], check=True)
    print(f'Built Fuse-X executable: {args.output}')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f'Fuse-X compiler: linker failed (status {error.returncode}); see diagnostics above', file=sys.stderr)
        sys.exit(1)
    except (OSError, RuntimeError) as error:
        print(f'Fuse-X compiler: {error}', file=sys.stderr)
        sys.exit(1)
