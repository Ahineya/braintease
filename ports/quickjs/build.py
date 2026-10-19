#!/usr/bin/env python3
"""Build the experimental js terminal host; never modifies installed images."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from explore import ROOT, PORT, CORE, guest_build, sha, run
from vendor import verify
from embed import prepare_http


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'native', 'ripple', 'both'), default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-cli')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='host reference C compiler')
    args = parser.parse_args()
    verify()
    for target in (('native', 'ripple') if args.target == 'both' else (args.target,)):
        build = args.build.resolve() / target if args.target == 'both' else args.build.resolve()
        build.mkdir(parents=True, exist_ok=True)
        manifest = build / 'cli-build.json'
        manifest.unlink(missing_ok=True)
        prepare_http(build / "source")
        if target == 'host':
            binary = build / 'js-host'
            flags = ['-std=gnu11', '-O2', '-fwrapv', '-D_GNU_SOURCE',
                     '-DCONFIG_VERSION="2026-06-04"', '-I' + str(PORT / 'upstream'), '-I' + str(build / 'source')]
            if sys.platform == 'darwin':
                sdk = os.environ.get('SDKROOT') or subprocess.check_output(
                    ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
                flags += ['-isysroot', sdk]
            run([*shlex.split(args.cc), *flags, PORT / 'cli.c', PORT / 'system.c',
                 *[PORT / 'upstream' / (name + '.c') for name in CORE], '-lm', '-o', binary],
                build / 'host-cli-build.log', timeout=180)
            compiler_hash = None
        else:
            compiler, requested = build / 'rcc-pinned', args.compiler.resolve()
            if compiler != requested and (not compiler.exists() or sha(compiler) != sha(requested)):
                shutil.copy2(requested, compiler)
            binary = guest_build(build, compiler, target, 'cli')
            compiler_hash = sha(compiler)
        manifest.write_text(json.dumps(dict(target=target,
            status='built; run tests/test_cli.py before claiming validation',
            compiler_sha256=compiler_hash, executable=str(binary),
            executable_sha256=sha(binary), executable_octets=binary.stat().st_size), indent=2) + '\n')
        print('Built JavaScript terminal host: ' + str(binary), flush=True)


if __name__ == '__main__':
    main()
