#!/usr/bin/env python3
"""Build pinned C16 Lynx for native Fuse-X or RVM/Riplix."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from probe import PORT, ROOT, all_units, prepare


def build_ripple(inputs, build, jobs, compiler, executable):
    make = ['make', '--no-print-directory', '-s', '-C', str(ROOT / 'ports/fuzix'),
            f'BUILD={build}', f'RCC={compiler}', 'RFL_CURSES=1']
    subprocess.run([*make, f'-j{jobs}', 'rfl-sdk', 'float-sdk'], check=True)
    sdk = json.loads(subprocess.check_output([*make, 'rfl-sdk-info'], text=True))
    floats = json.loads(subprocess.check_output([*make, 'float-sdk-info'], text=True))
    assembler = ROOT / 'src/ripple-asm/target/release/rasm'
    linker = assembler.with_name('rlink')

    def lower(path):
        assembly = path.with_suffix('.asm')
        obj = path.with_suffix('.pobj')
        log_path = path.with_suffix('.lower.log')
        if (assembly.exists() and obj.exists() and
                min(assembly.stat().st_mtime_ns, obj.stat().st_mtime_ns) >=
                max(p.stat().st_mtime_ns for p in (path, compiler, assembler))):
            print(f'{path.stem}: assembled (cached)', flush=True)
            return str(obj)
        try:
            with log_path.open('w') as log:
                subprocess.run([str(compiler), 'generate-asm', '-i', str(path),
                                '--banked-globals', '-o', str(assembly)], check=True, stdout=log, stderr=log)
                subprocess.run([str(assembler), 'assemble', str(assembly), '-o', str(obj),
                                '--bank-size', '64000', '--max-immediate', '65535'],
                               check=True, stdout=log, stderr=log)
        except subprocess.CalledProcessError as error:
            obj.unlink(missing_ok=True)
            print(f'{path.stem}: failed, status {error.returncode} ({log_path})\n' + log_path.read_text(), flush=True)
            return None
        print(f'{path.stem}: assembled', flush=True)
        return str(obj)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(lower, inputs))
    if any(obj is None for obj in objects):
        raise SystemExit('Lynx assembly failed; see the per-unit logs above')
    # The floating formatter replaces the ordinary integer-only formatter.
    libraries = [p for p in sdk['libraries'] if Path(p) != build / 'libc/format.pobj']
    with (PORT / 'build/ripple-link.log').open('w') as log:
        subprocess.run([sys.executable, str(ROOT/'ports/fuzix/tools/shared-link.py'), '--build', str(build), '--linker', str(linker), '--compiler', str(compiler), '--', sdk['entry'], *objects, *floats['libraries'], *libraries,
                        '-f', 'rxe', '--bank-size', '64000', '-o', str(executable),
                        '--map', str(PORT / 'build/ripple-map.json')],
                       check=True, stdout=log, stderr=log)
    data = executable.read_bytes()
    if data[:4] not in (b'RXE1', b'RXE2'):
        raise RuntimeError('invalid linked executable envelope')
    # The RXE1 header puts instruction and cell counts after bank size and entry.
    code, cells = struct.unpack_from('<II', data, 12)
    return {'target': 'ripple', 'package_octets': len(data), 'instructions': code,
            'global_cells': cells}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', type=int, default=4)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--build-dir', type=Path, help='guest SDK directory (defaults to the selected OS build)')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('-o', '--output', type=Path, help='executable path')
    parser.add_argument('--skip-compile', action='store_true', help='reuse the selected target IR probe')
    args = parser.parse_args()
    if args.j < 1:
        parser.error('-j must be positive')
    (PORT / 'build').mkdir(exist_ok=True)
    # A normal image build and a manual browser build can target the same
    # generated IR/assembly. Serialize them before either changes those files.
    with (PORT / f'build/{args.target}.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        build_browser(args)


def build_browser(args):
    compiler = args.compiler.resolve()
    build = (args.build_dir or ROOT / ('build/fuzix' if args.target == 'ripple' else 'fuse-x/build')).resolve()
    executable = (args.output or PORT / ('build/lynx.rxe' if args.target == 'ripple' else 'build/lynx.fxe')).resolve()
    executable.parent.mkdir(parents=True, exist_ok=True)
    if not args.skip_compile:
        subprocess.run([sys.executable, str(PORT / 'probe.py'), '--all', '--target', args.target,
                        '--compiler', str(compiler), '-j', str(args.j)], check=True)
    directory = PORT / ('build/native' if args.target == 'native' else 'build/probe')
    inputs = [directory / (unit + '.ir') for unit in all_units(prepare())]
    tls = build / 'tls'
    tls.mkdir(parents=True, exist_ok=True)
    # Match app.py's lock while compiling/lowering shared TLS objects.
    with (tls / (args.target + '.lock')).open('a') as tls_lock:
        fcntl.flock(tls_lock, fcntl.LOCK_EX)
        subprocess.run([sys.executable, str(ROOT / 'ports/tls/build.py'), '--target', args.target,
                        '--compiler', str(compiler), '--build', str(tls), '-j', str(args.j)], check=True)
        inputs += [Path(p) for p in json.loads((tls / args.target / 'objects.json').read_text())]
        if args.target == 'ripple':
            report = build_ripple(inputs, build, args.j, compiler, executable)
        else:
            library_args = []
            if os.environ.get('FUSE_X_SHARED_LIBRARIES', '1') != '0':
                tls_inputs=[Path(p) for p in json.loads((tls / args.target / 'objects.json').read_text())]
                subprocess.run([sys.executable,str(ROOT/'ports/tls/build.py'),'--target','native',
                                '--compiler',str(compiler),'--build',str(tls),'--shared-sdk',str(build)],check=True)
                library_args=['--library',f'{build}/lib/libtls.fxl.1=/lib/fuse-x/libtls.fxl.1']
                inputs=[p for p in inputs if p not in tls_inputs]
            subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'), *map(str, inputs),
                            '--curses', '--float-format', '--compiler', str(compiler),
                            '--build', str(build), *library_args, '-o', str(executable)], check=True)
            data = executable.read_bytes()
            if data[:8] != b'FUSEX001':
                raise RuntimeError('invalid linked executable envelope')
            manifest_length, payload_length = struct.unpack_from('<II', data, 8)
            manifest = json.loads(data[16:16 + manifest_length])
            report = {'target': 'native', 'package_octets': len(data), 'payload_octets': payload_length,
                      'global_cells': manifest['global_cells'], 'global_bank': manifest['global_bank'],
                      'functions': len(manifest['functions'])}
        (PORT / f'build/resources-{args.target}.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
