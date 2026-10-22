#!/usr/bin/env python3
"""Build and cache the optional Graphite desktop application set, without images."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import cached_step, sources

APPLICATIONS = ('lvcalc', 'lvselect', 'lvgallery', 'lvdata', 'lvedit', 'fusedeshell')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--lvgl-build', type=Path)
    args = parser.parse_args()
    build, compiler = args.build.resolve(), args.compiler.resolve()
    product = args.lvgl_build.resolve() if args.lvgl_build else build / 'graphite'
    apps = build / 'apps'
    apps.mkdir(parents=True, exist_ok=True)
    dependencies = sources(ROOT / 'ports/lvgl', ROOT / 'ports/fusede/catalog',
                           ROOT / 'ports/graphics/include', ROOT / 'libraries/ini',
                           ROOT / 'fuse-x/cc.py', ROOT / 'fuse-x/libc', ROOT / 'fuse-x/include',
                           ROOT / 'fuse-x/memory_mode.py', ROOT / 'fuse-x/heap_profile.py',
                           ROOT / 'ports/fuzix/ripple/libc', ROOT / 'ports/fuzix/ripple/userlib.c',
                           ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library',
                           ROOT / 'runtime/include', ROOT / 'libraries/text', ROOT / 'libraries/unicode',
                           ROOT / 'ports/fuzix/upstream/Kernel/include/userstructs.h', Path(__file__)) + [compiler]
    outputs = [apps / (name + '.fxe') for name in APPLICATIONS]

    def compile_applications():
        command = [sys.executable, str(ROOT / 'ports/lvgl/build.py'), '--build', str(product),
                   '--sdk-build', str(build), '--compiler', str(compiler)]
        subprocess.run([*command, '--app', 'all'], check=True, cwd=ROOT)
        subprocess.run([*command, '--app', 'fusedeshell'], check=True, cwd=ROOT)
        for name, output in zip(APPLICATIONS, outputs):
            temporary = output.with_name('.' + output.name + '.new')
            shutil.copy2(product / output.name, temporary)
            temporary.replace(output)
        (build / 'graphite-applications.json').write_text(json.dumps({
            'applications': {path.stem: hashlib.sha256(path.read_bytes()).hexdigest() for path in outputs},
            'source': 'ports/lvgl', 'catalog': 'ports/fusede/catalog',
        }, indent=2) + '\n')

    changed = cached_step(build / '.graphite-cache.json', dependencies,
                          [str(build), str(product), str(compiler),
                           {key:os.environ.get(key) for key in ('RCC_NATIVE_MEMORY','FUSE_X_HEAP_PROFILE','RCC_CODEGEN_OPT','RCC_NATIVE_RUNTIME')}],
                          compile_applications, outputs, force=os.environ.get('RIPPLE_REBUILD') == '1')
    print('Graphite desktop: ' + ('updated' if changed else 'current'), flush=True)


if __name__ == '__main__':
    main()
