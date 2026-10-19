#!/usr/bin/env python3
"""Compile selected upstream Lynx units with the reviewed C16 configuration.

This compile gate feeds build.py; it uses guest headers and the reviewed
cross-target configuration. Diagnostics are retained per unit; failures are fatal.
"""
import argparse
from datetime import datetime, timezone
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile
from adapt import generate, array_bounds

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
SHA256 = '99f8f28f860094c533100d1cedf058c27fb242ce25e991e2d5f30ece4457a3bf'


def prepare():
    archive = PORT / 'vendor/lynx2.9.2.tar.gz'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Lynx archive checksum mismatch')
    source = PORT / 'upstream'
    if not source.exists():
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                relative = Path(*Path(member.name).parts[1:])
                if '..' in relative.parts or Path(member.name).is_absolute():
                    raise RuntimeError('unsafe archive member')
                if member.isfile():
                    output = source / relative
                    output.parent.mkdir(parents=True, exist_ok=True)
                    output.write_bytes(tar.extractfile(member).read())
    return source


def charset_tables(source, output):
    tool = PORT / 'build/tools/makeuctb'
    tool.parent.mkdir(parents=True, exist_ok=True)
    generator = source / 'src/chrtrans/makeuctb.c'
    if not tool.exists() or tool.stat().st_mtime < generator.stat().st_mtime:
        command = shlex.split(os.environ.get('HOST_CC', os.environ.get('RCC_NATIVE_CC', 'cc')))
        command += shlex.split(os.environ.get('HOST_CFLAGS', ''))
        if sys.platform == 'darwin':
            sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
            command += ['-isysroot', sdk]
        command += ['-DHAVE_CONFIG_H=1', '-DNO_LYNX_TRACE']
        for path in [PORT / 'config', source, source / 'src', source / 'WWW/Library/Implementation', source / 'src/chrtrans']:
            command += ['-I', str(path)]
        subprocess.run([*command, str(generator), '-o', str(tool)], check=True)
    output.mkdir(parents=True, exist_ok=True)
    for table in sorted((source / 'src/chrtrans').glob('*.tbl')):
        header = output / (table.stem + '.h')
        if not header.exists() or header.stat().st_mtime < table.stat().st_mtime:
            subprocess.run([str(tool), str(table), str(header)], check=True, stdout=subprocess.DEVNULL)


def all_units(source):
    app = (source / 'src/makefile.in').read_text().split('OBJS\t\t=')[1].split('C_SRC')[0]
    library = (source / 'WWW/Library/Implementation/Makefile.in').read_text().split('COMMON =')[1].split('CFILES')[0]
    return list(dict.fromkeys(re.findall(r'\b(\w+)\$o', app + library) + ['UCdomap', 'UCAux', 'UCAuto', 'ripple_browser']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('units', nargs='*', default=['HTList', 'HTChunk', 'HTAtom', 'HTParse', 'HTMLDTD', 'GridText'])
    parser.add_argument('-j', type=int, default=4)
    parser.add_argument('--all', action='store_true', help='probe all configured browser/libwww units')
    parser.add_argument('--target', choices=['ripple', 'native'], default='ripple')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    args = parser.parse_args()
    source = prepare()
    output = PORT / ('build/probe' if args.target == 'ripple' else 'build/native')
    output.mkdir(parents=True, exist_ok=True)
    adapted = output / 'adapted'
    generate(source, adapted)
    tables = output / 'chrtrans'
    if args.all or 'UCdomap' in args.units:
        charset_tables(source, tables)
    if args.all:
        args.units = all_units(source)
    includes = [ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/ripple', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/tls', PORT / 'config', adapted, tables, source, source / 'src',
                source / 'WWW/Library/Implementation', source / 'src/chrtrans']
    flags = ['-DRIPPLE_MMU=1', '-DNO_64BIT', '-DHAVE_CONFIG_H=1', '-DNO_LYNX_TRACE=1']
    stamp = datetime.fromtimestamp(int(os.environ['SOURCE_DATE_EPOCH']), timezone.utc) if 'SOURCE_DATE_EPOCH' in os.environ else datetime.now(timezone.utc)
    date = stamp.strftime('%b %d %Y')
    flags += [f'-D__DATE__="{date}"', f'-D__TIME__="{stamp:%H:%M:%S}"']
    for path in includes:
        flags += ['-I', str(path)]
    compiler = args.compiler.resolve()
    preprocessor = [str(compiler.with_name('rcpp'))]
    if args.all or {'LYReadCFG', 'LYEditmap'}.intersection(args.units):
        array_bounds(source, adapted, preprocessor, flags)
    def compile_unit(unit):
        paths = [PORT / (unit + '.c'), adapted / (unit + '.c'), source / 'src' / (unit + '.c'), source / 'WWW/Library/Implementation' / (unit + '.c')]
        path = next((p for p in paths if p.exists()), None)
        if path is None:
            raise RuntimeError(f'Unknown unit: {unit}')
        preprocessed = output / (unit + '.i')
        pp = subprocess.run([*preprocessor, *flags, str(path)], capture_output=True, text=True, timeout=120)
        preprocessed.write_text(pp.stdout)
        run = subprocess.run([str(compiler), 'compile', str(preprocessed),
                              '--no-preprocess', '--target', args.target, '--bank-size', '64000', '--emit-ir'],
                             capture_output=True, text=True, timeout=120) if pp.returncode == 0 else pp
        (output / (unit + '.log')).write_text(pp.stderr + (run.stderr if run is not pp else ''))
        if run.returncode == 0:
            (output / (unit + '.ir')).write_text(run.stdout)
        else:
            (output / (unit + '.ir')).unlink(missing_ok=True)
        print(f'{unit}: ' + ('compiled' if run.returncode == 0 else run.stderr.splitlines()[-1]), flush=True)
        return {'unit': unit, 'compiled': run.returncode == 0}
    with ThreadPoolExecutor(max_workers=args.j) as pool:
        results = list(pool.map(compile_unit, args.units))
    (output / 'results.json').write_text(json.dumps(results, indent=2))
    raise SystemExit(0 if all(r['compiled'] for r in results) else 1)


if __name__ == '__main__':
    main()
