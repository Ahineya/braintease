#!/usr/bin/env python3
"""Run prepared native SDK/tool Makefiles with host-compiled guest Make.

Compilation still uses the portable C16 pipeline. This tests build rules and
incremental dependencies, not execution of Make or compiler sources in x86.
Use separate prepared directories; this test creates their build outputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/portable-native-make-test')
    args = parser.parse_args()
    sdk, source, out = args.sdk.resolve(), args.source.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    make = out / 'make'
    command = ['cc', '-std=c99', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE']
    if os.environ.get('SDKROOT'):
        command += ['-isysroot', os.environ['SDKROOT']]
    subprocess.run(command + [str(ROOT / 'ports/fuzix/upstream/Applications/MWC/cmd/make.c'),
                              '-o', str(make)], check=True)
    base = HERE.parent / 'build'
    programs = ['rcc-qbe', 'rcc-x64-ld', 'rcc-native-cc', 'rcc-x64-ar']
    jobs = [('sdk', sdk, ['CC=' + str(base / 'rcc-native-cc') + ' --tools ' + str(sdk / 'tools'),
                         'AR=' + str(base / 'rcc-x64-ar'), 'SDK=' + str(sdk)],
             [sdk / 'libc.a', *[sdk / ('crt0-' + str(n) + '.o') for n in [0, 2, 3]]]),
            ('tools', source, ['CC=' + str(base / 'rcc-native-cc'), 'TOOLS=' + str(sdk / 'tools'),
                               'SDK=' + str(sdk)],
             [source / 'build/bin' / n for n in programs])]
    reports = []
    for name, directory, options, outputs in jobs:
        before = {str(p): digest(p) for p in outputs if p.exists()}
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([str(make), '-r', *options], cwd=directory, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        after = {str(p): digest(p) for p in outputs}
        assert all(after[n] == value for n, value in before.items()), name + ' output changed'
        # An unchanged build must be quiet and leave every output untouched.
        mtimes = {p: p.stat().st_mtime_ns for p in outputs}
        subprocess.run([str(make), '-rq', *options], cwd=directory, check=True)
        # Verify the header hidden behind an SDK forwarding include really
        # participates in BOTH build graphs. Make stores whole-second times.
        header = sdk / 'tree/ports/fuzix/upstream/Library/include/stdlib.h'
        saved = header.stat()
        time.sleep(1.1)
        try:
            header.touch()
            result = subprocess.run([str(make), '-rq', *options], cwd=directory)
            assert result.returncode == 2, name + ' missed an SDK header change'
        finally:
            os.utime(header, ns=(saved.st_atime_ns, saved.st_mtime_ns))
        subprocess.run([str(make), '-rq', *options], cwd=directory, check=True)
        assert mtimes == {p: p.stat().st_mtime_ns for p in outputs}
        reports.append(dict(build=name, status='passed', outputs=after,
                            unchanged_build=True, sdk_header_invalidation=True))
    (out / 'result.json').write_text(json.dumps(reports, indent=2) + '\n')
    print('Guest Make accepts SDK/tool source rules; builds, parity, no-op rebuilds and header invalidation pass')


if __name__ == '__main__':
    main()
