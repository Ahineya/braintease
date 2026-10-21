#!/usr/bin/env python3
"""Compile C sources into an independently loadable Fuse-X native executable."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from memory_mode import selected as native_memory_mode

ROOT = Path(__file__).resolve().parent.parent
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path, nargs='*')
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('-I', dest='includes', action='append', default=[])
    parser.add_argument('-D', dest='defines', action='append', default=[])
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--linker', type=Path)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--heap-profile', choices=('hosted','bounded'), default=os.environ.get('FUSE_X_HEAP_PROFILE','hosted'),
                        help='hosted sparse heap (default) or bounded 48..239 freestanding heap; use a separate build directory per profile')
    parser.add_argument('--float-format', action='store_true', help='enable floating point libc formatting and math')
    parser.add_argument('--curses', action='store_true', help='link the shared C16 curses library')
    parser.add_argument('--ini', action='store_true', help='link the shared INI reader')
    parser.add_argument('--tui-theme', action='store_true', help='link curses plus the INI/theme companion libraries')
    parser.add_argument('--object-ir', type=Path, action='append', default=[],
                        help='link an additional native IR object (platform libraries)')
    linking = parser.add_mutually_exclusive_group()
    linking.add_argument('--shared', action='store_true', default=os.environ.get('FUSE_X_SHARED_LIBRARIES', '1') != '0', help='use versioned guest shared libc and companion libraries (default)')
    linking.add_argument('--static', action='store_false', dest='shared', help='embed guest libraries using the legacy executable ABI')
    parser.add_argument('--sdk-only', action='store_true', help='prepare shared SDK libraries without compiling an executable')
    parser.add_argument('--library', action='append', default=[], metavar='HOST=GUEST', help='import a guest shared library interface')
    args = parser.parse_args()
    try:
        memory_mode = native_memory_mode()
    except ValueError as error:
        parser.error(str(error))
    if args.heap_profile not in ('hosted', 'bounded'):
        parser.error('FUSE_X_HEAP_PROFILE must be hosted or bounded')
    if args.shared:
        args.float_format = True
    args.curses = args.curses or args.tui_theme
    args.ini = args.ini or args.tui_theme
    compiler = args.compiler.resolve()
    linker = (args.linker or compiler).resolve()
    includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include',
                ROOT / 'runtime/include', PORT / 'ripple', ROOT / 'ports/graphics/include',
                ROOT / 'libraries/ini', ROOT / 'libraries/tui-theme']
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DFUSE_X_USER=1', '-DNO_64BIT']
    if args.heap_profile == 'bounded':
        flags += ['-DFUSE_X_BOUNDED_HEAP=1']
    if any(d.split('=',1)[0] == 'FUSE_X_BOUNDED_HEAP' for d in args.defines):
        parser.error('select --heap-profile bounded instead of a source-only FUSE_X_BOUNDED_HEAP define')
    if args.float_format:
        flags.remove('-DNO_64BIT')
    for directory in includes:
        flags += ['-I', str(directory)]
    cache = args.build.resolve() / 'sdk'
    cache.mkdir(parents=True, exist_ok=True)
    excluded = set('regerror malloc calloc realloc readline readline_fn clock_gettime clock_getres __sin __cos '
                   '__rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow '
                   'sin cos atan atan2 strtod ecvt fmod frexp ldexp modf'.split())
    libraries = [PORT / f'ripple/libc/{name}.c'
                 for name in 'syscalls stdio format scan wait clock timezone dirent misc heap pipe sockets resolver search identity'.split()]
    libraries += sorted(p for p in (PORT / 'upstream/Library/libs').glob('*.c')
                        if p.stem not in excluded)
    libraries += [ROOT / 'fuse-x/libc/bridge.c', PORT / 'ripple/userlib.c', ROOT / 'libraries/unicode/unicode.c']
    core_sources = set(libraries)
    if args.curses:
        curses = PORT / 'upstream/Library/libs/curses'
        flags += ['-I', str(curses)]
        overrides = {'initscr', 'endwin', 'newwin', 'windel', 'wgetch', 'wgetstr', 'doupdt', 'refresh', 'winscrol', 'waddch', 'waddstr'}
        libraries += sorted(p for p in curses.glob('*.c') if p.stem not in overrides)
        libraries += sorted((PORT / 'ripple/curses').glob('*.c'))
    if args.ini:
        libraries += [ROOT / 'libraries/ini/ini.c']
    if args.tui_theme:
        libraries += [ROOT / 'libraries/tui-theme/tui_theme.c', ROOT / 'libraries/tui-theme/tui_curses.c']
    if args.float_format:
        floating = [PORT / 'ripple/libc/math-bits.c', PORT / 'ripple/libc/math-host.c']
        floating += [PORT / f'upstream/Library/libs/{name}.c' for name in
                     'scalbn strtod ecvt frexp ldexp modf'.split()]
        libraries += floating
        core_sources.update(floating)
    digest = hashlib.sha256()
    digest.update(memory_mode.encode())
    digest.update(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for header in sorted(directory.rglob('*.h')):
            digest.update(str(header).encode())
            digest.update(header.read_bytes())
    digest.update((PORT / "upstream/Kernel/include/userstructs.h").read_bytes())
    fingerprint = digest.digest()

    def compile_library(source):
        library_flags = flags + (['-DRIPPLE_FLOAT_FORMAT'] if args.float_format and source.name == 'format.c' else [])
        key_hash = hashlib.sha256(fingerprint + str((source, library_flags)).encode())
        for dependency in c_dependencies(source, includes):
            key_hash.update(str(dependency).encode());key_hash.update(dependency.read_bytes())
        key = key_hash.hexdigest()
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

    print(f'Build: Preparing shared SDK for {args.output.name}', flush=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        library_ir = list(pool.map(compile_library, libraries))
    shared_arguments = [item for value in args.library for item in ('--library', value)]
    if args.shared:
        shared_dir = args.build.resolve() / 'lib'
        shared_dir.mkdir(parents=True, exist_ok=True)
        core_ir = [ir for source, ir in zip(libraries, library_ir) if source in core_sources]
        companions = [ir for source, ir in zip(libraries, library_ir) if source not in core_sources]
        def shared_library(name, inputs, dependencies):
            output = shared_dir / name
            stamp = output.with_suffix(output.suffix + '.sha256')
            digest = hashlib.sha256(str((linker, linker.stat().st_mtime_ns, dependencies)).encode())
            digest.update(memory_mode.encode())
            for path in inputs:
                digest.update(path.read_bytes())
            for _, value in zip(dependencies[::2], dependencies[1::2]):
                digest.update(Path(value.split('=',1)[0]).read_bytes())
            key = digest.hexdigest()
            with output.with_suffix(output.suffix + '.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                if not output.exists() or not stamp.exists() or stamp.read_text() != key:
                    print(f'Build: Linking shared {name}', flush=True)
                    subprocess.run([str(linker), 'link-shared', *map(str,inputs), '--soname', name,
                                    *dependencies, '-o', str(output)], check=True,
                                   env=dict(os.environ, RCC_NATIVE_PROGRESS='1'))
                    stamp.write_text(key)
            return ['--library', f'{output}=/lib/fuse-x/{name}']
        shared_arguments += shared_library('libc.fxl.1', core_ir, [])
        if companions:
            # Split more finely as companion APIs stabilize; all still share libc.
            name = 'libtui.fxl.1' if args.tui_theme else ('libcurses.fxl.1' if args.curses else 'libini.fxl.1')
            shared_arguments += shared_library(name, companions, shared_arguments)
        library_ir = []
    if args.sdk_only:
        if not args.shared:
            parser.error('--sdk-only needs --shared')
        return
    if not args.sources:
        parser.error('at least one application source is required')
    print(f'Build: Compiling {args.output.name} application inputs', flush=True)
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
        print(f'Build: Linking {args.output.name} (native code generation)', flush=True)
        subprocess.run([str(linker), 'link-app', str(startup_ir), *map(str, inputs),
                        *map(str, args.object_ir), *map(str, library_ir), *shared_arguments, '-o', str(args.output)], check=True,
                       env=dict(os.environ, RCC_NATIVE_PROGRESS='1'))
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
