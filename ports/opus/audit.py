#!/usr/bin/env python3
"""Audit pinned unmodified Opus decoder units, without installing guest software."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tarfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = json.loads((HERE / 'UPSTREAM.json').read_text())
UNITS = ('celt/entdec.c', 'celt/entcode.c', 'celt/mdct.c', 'celt/kiss_fft.c',
         'celt/celt_decoder.c', 'silk/dec_API.c', 'src/opus_decoder.c')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/opus-audit')
    ap.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    ap.add_argument('--matrix', action='store_true', help='also record unsupported VLA and alloca configurations')
    ap.add_argument('--full', action='store_true', help='compile all generic fixed-point library units, not just seven decoder probes')
    args = ap.parse_args()
    archive, build, compiler = args.archive.resolve(), args.build.resolve(), args.compiler.resolve()
    if hashlib.sha256(archive.read_bytes()).hexdigest() != PIN['sha256']:
        raise SystemExit('Official Opus archive differs from pin')
    build.mkdir(parents=True, exist_ok=True)
    report = build / 'report.json'
    report.unlink(missing_ok=True)
    source = build / ('opus-' + PIN['version'])
    # Re-extract expected source bytes; refuse a modified cached source tree.
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            relative = Path(member.name)
            if relative.is_absolute() or '..' in relative.parts or relative.parts[0] != source.name:
                raise SystemExit('Unexpected archive path')
            destination = build / relative
            data = tar.extractfile(member).read()
            if destination.exists() and destination.read_bytes() != data:
                raise SystemExit('Modified cached source: ' + str(relative))
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
    includes = [source, source / 'include', source / 'celt', source / 'silk', source / 'silk/fixed',
                ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']
    modes = ('VAR_ARRAYS', 'USE_ALLOCA', 'NONTHREADSAFE_PSEUDOSTACK') if args.matrix else ('NONTHREADSAFE_PSEUDOSTACK',)
    units = list(UNITS)
    if args.full:
        units = []
        for file, groups in [('opus_sources.mk', ['OPUS_SOURCES']), ('celt_sources.mk', ['CELT_SOURCES']),
                             ('silk_sources.mk', ['SILK_SOURCES', 'SILK_SOURCES_FIXED'])]:
            text = (source / file).read_text()
            for group in groups:
                match = re.search(r'^' + group + r'\s*=([^\n]*(?:\n[^\n]+)*)', text, re.M)
                if not match:
                    raise SystemExit('Missing upstream source list: ' + group)
                units.extend(match[1].replace('\\', ' ').split())
        units = list(dict.fromkeys(units))
    records = []
    for mode in modes:
        for name in units:
            started = time.monotonic()
            command = [str(compiler), 'compile', str(source / name), '--target', 'native', '--emit-ir',
                       '-DRIPPLE_MMU=1', '-DFUSE_X_USER=1', '-DOPUS_BUILD=1', '-DHAVE_STDINT_H=1',
                       '-DFIXED_POINT=1', '-D' + mode + '=1', *['-I' + str(d) for d in includes]]
            run = subprocess.run(command, capture_output=True, timeout=60, env=dict(os.environ, RUST_LOG='error'))
            log = Path(name).stem + '-' + mode + '.log'
            (build / log).write_bytes(run.stderr)
            records.append(dict(unit=name, allocation=mode, compiled=run.returncode == 0,
                                seconds=time.monotonic() - started, diagnostic=run.stderr.decode(errors='replace')[-2000:]))
            print(mode + ': ' + name + ': ' + ('compiled' if not run.returncode else 'blocked'), flush=True)
    good = all(r['compiled'] for r in records if r['allocation'] == 'NONTHREADSAFE_PSEUDOSTACK')
    report.write_text(json.dumps(dict(status='compile gate passed' if good else 'blocked',
        scope=('Generic fixed-point library compilation' if args.full else 'Seven selected original decoder units') + '; not linking, PCM correctness, realtime playback or C16 ABI safety',
        upstream=PIN, compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), cases=records), indent=2) + '\n')
    if not good:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
