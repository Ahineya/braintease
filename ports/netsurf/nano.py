"""Build the existing C16 Nano-X client library for the browser frontend."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from build import ROOT
from adapt import write_changed


def client(output, compiler, target, jobs):
    directory = Path(output) / ('nanox-' + target)
    generated = directory / 'source'
    subprocess.run([sys.executable, str(ROOT / 'ports/nanox/adapt.py'), str(generated)], check=True)
    if target == 'native':
        subprocess.run([sys.executable, str(ROOT / 'fuse-x/nanox/adapt.py'), str(generated)], check=True)
    includes = [generated / 'include', generated / 'drivers', generated / 'nanox',
                ROOT / 'ports/nanox/ripple', ROOT / 'ports/fusede/include', ROOT / 'fuse-x/nanox']
    defines = ['-DMWCONFIG_FILE="nxr_config.h"', '-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565', '-DNXR_DESKTOP=1']
    if target == 'native':
        defines += ['-DRIPPLE_NETSURF_HOSTED=1']
    sdk = [ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
           ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple', ROOT / 'ports/graphics/include']
    flags = ['--target', target, '-DRIPPLE_MMU=1', '-DNO_64BIT', *defines]
    if target == 'native':
        flags += ['-DNXR_NATIVE_GUI=1']
    flags += [v for path in [*sdk, *includes] for v in ('-I', str(path))]
    paths = [generated / f'nanox/{name}.c' for name in ('client', 'nxproto', 'nxdraw', 'nxpaintnc', 'nxutil')]
    paths += [ROOT / 'ports/nanox/ripple/client-os.c', ROOT / 'ports/fusede/skin.c',
              ROOT / 'libraries/unicode/font.c']
    sys.path.insert(0, str(ROOT / 'scripts'))
    from build_cache import c_dependencies

    def compile_one(path):
        ir = directory / (path.stem + '.ir')
        digest = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
        # MWCONFIG_FILE is a macro include, outside the literal-include scanner.
        dependencies = set(c_dependencies(path, [*sdk, *includes]))
        dependencies.add(ROOT / 'ports/nanox/ripple/nxr_config.h')
        for dependency in sorted(dependencies):
            digest.update(str(dependency).encode()); digest.update(dependency.read_bytes())
        key = digest.hexdigest()
        stamp = ir.with_suffix('.json')
        if not (ir.exists() and stamp.exists() and json.loads(stamp.read_text())['key'] == key):
            result = subprocess.run([str(compiler), 'compile', str(path), *flags, '--emit-ir'],
                                    capture_output=True, env=dict(os.environ, RUST_LOG='error'))
            ir.with_suffix('.log').write_bytes(result.stderr + (result.stdout if result.returncode else b''))
            if result.returncode:
                raise RuntimeError(f'{path}: see {ir.with_suffix(".log")}')
            write_changed(ir, result.stdout)
            stamp.write_text(json.dumps(dict(key=key)) + '\n')
        return ir
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(compile_one, paths))
    return objects, includes, defines
