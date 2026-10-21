#!/usr/bin/env python3
"""Build LVGL and its calculator independently of the Fuse-X kernel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
from adapt import prepare, sources

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app', choices=('lvcalc','lvselect','lvgallery','lvdata','lvedit','lvbench','lvtablebench','all'), default='all')
    p.add_argument('--build', type=Path, default=ROOT / 'build/lvgl')
    p.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    p.add_argument('--sdk-build', type=Path, help='separate native SDK cache for isolated builds')
    p.add_argument('--stage', type=Path)
    p.add_argument('--fetch', action='store_true', help='download the pinned upstream archive if absent')
    p.add_argument('-j', '--jobs', type=int, default=4)
    args = p.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    # Normal builds verify and consume baked assets; no SVG rasterizer dependency.
    icon_spec = importlib.util.spec_from_file_location('lvgl_icons', PORT/'icons/build.py')
    icon_assets = importlib.util.module_from_spec(icon_spec); icon_spec.loader.exec_module(icon_assets)
    icon_assets.verify()
    assert (PORT/'icons/LICENSE.Lucide.txt').read_bytes()==(icon_assets.SOURCE/'LICENSE').read_bytes()
    icon_manifest = json.loads((PORT/'icons/source.json').read_text())
    assert hashlib.sha256((PORT/'icons/data.c').read_bytes()).hexdigest()==icon_manifest['generated_sha256']
    assert hashlib.sha256((PORT/'icons/masks.json').read_bytes()).hexdigest()==icon_manifest['masks_sha256']
    source, config = prepare(build, args.fetch)
    compiler = args.compiler.resolve()
    includes = [ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple',
                ROOT / 'ports/graphics/include', ROOT / 'libraries/text', source, config, PORT]
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1',
             '-DFUSE_X_USER=1', '-DLV_CONF_INCLUDE_SIMPLE=1', '-D__func__="C16"']
    flags += [x for path in includes for x in ('-I', str(path))]
    cache = build / 'objects'
    cache.mkdir(exist_ok=True)
    digest = hashlib.sha256(str((compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for path in sorted(directory.rglob('*.h')):
            digest.update(str(path).encode()); digest.update(path.read_bytes())
    prefix = digest.digest()
    font_sizes=json.loads((PORT/'fonts/generated.json').read_text())['sizes']
    pack_spec = importlib.util.spec_from_file_location('graphite_font_pack', PORT/'fonts/pack.py')
    font_pack = importlib.util.module_from_spec(pack_spec); pack_spec.loader.exec_module(font_pack)
    font_units = font_pack.prepare(build)
    units = sources(source)+font_units+[PORT/'font-data.c',PORT/'fonts/symbols.c',PORT/'icons/data.c',PORT/'ui-scale.c',PORT/'ui.c',PORT/'ui-data.c',PORT/'ui-app.c',PORT/'ui-forms.c',PORT/'browser.c',PORT/'ui-tabs.c',PORT/'plot.c',PORT/'plot-raster.c',PORT/'ui-plot.c']

    def compile_one(path):
        key = hashlib.sha256(prefix + path.read_bytes()).hexdigest()[:20]
        output = cache / (path.stem + '-' + key + '.ir')
        if not output.exists():
            r = subprocess.run([str(compiler), 'compile', str(path), *flags, '--emit-ir'], capture_output=True)
            (cache / (path.stem + '.log')).write_bytes(r.stderr)
            if r.returncode: raise RuntimeError(str(path) + '\n' + r.stderr.decode(errors='replace')[-4000:])
            output.write_bytes(r.stdout)
        return output

    if args.app in ('lvbench','lvtablebench') and args.stage:p.error('Benchmarks are test workloads, not staged applications')
    print('Compiling', len(units), 'LVGL units', flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects = list(pool.map(compile_one, units))
    selected = ['lvcalc','lvselect','lvgallery','lvdata','lvedit'] if args.app=='all' else [args.app]
    hashes = {}
    for app in selected:
        app_sources = {'lvcalc': ('calculator.c','calc.c'),
                       'lvdata': ('data-viewer.c','data-plot.c','csv.c'),
                       'lvtablebench': ('table-bench.c','csv.c'),
                       'lvbench': ('scroll-bench.c','fill-check.c','font-check.c'),
                       'lvgallery': ('gallery.c',),
                       'lvedit': ('editor.c',),
                       'lvselect': ('selector.c',)}[app]
        extra_objects = []
        if app in ('lvedit', 'lvgallery'):
            extra_objects = [compile_one(path) for path in
                             (ROOT/'libraries/text/text.c', PORT/'ui-editor.c', PORT/'ui-document.c')]
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(PORT / 'gx.c'),
                   *[str(PORT / name) for name in app_sources],
                   '--compiler', str(compiler),
                   '--static', '--float-format', '-D', 'LV_CONF_INCLUDE_SIMPLE=1', '-D', '__func__="C16"', '-o', str(build / (app+'.fxe'))]
        if args.sdk_build:
            command += ['--build', str(args.sdk_build.resolve())]
        for path in (source, config, PORT): command += ['-I', str(path)]
        for path in objects + extra_objects: command += ['--object-ir', str(path)]
        subprocess.run(command, cwd=ROOT, check=True)
        hashes[app] = hashlib.sha256((build / (app+'.fxe')).read_bytes()).hexdigest()
    info = {
        'upstream': 'LVGL 9.6.0', 'target': 'Fuse-X C16', 'linkage': 'static',
        'source_units': len(units),
        'executables': hashes,
        'fonts': {'family':'Noto Sans','sizes':font_sizes,
                  'storage':'lossless packed globals, immutable heap A8 at startup',
                  'roles':{'secondary':12,'body':14,'heading':16},
                  'codepoints':len(json.loads((PORT/'fonts/generated.json').read_text())['codepoints']),
                  'source':json.loads((PORT/'fonts/source.json').read_text())},
        'density': {'supported':[1,2], 'default':'display preference when supported; 1x on older kernels'},
        'surface_format': {'preferred':'XRGB8888', 'fallback':'RGB565', 'discovery':'GX_FORMATS'},
        'icons': icon_manifest,
    }
    info_path=build/('bench-build-info.json' if args.app in ('lvbench','lvtablebench') else 'build-info.json')
    info_path.write_text(json.dumps(info, indent=2) + '\n')
    if args.stage:
        stage = args.stage.resolve()
        stage.mkdir(parents=True, exist_ok=True)
        for app in selected:
            temporary = stage / ('.'+app+'.fxe.new')
            shutil.copy2(build / (app+'.fxe'), temporary); temporary.replace(stage / (app+'.fxe'))
        assets = stage / 'lvgl'; assets.mkdir(exist_ok=True)
        for license in (build / 'licenses').glob('*.txt'): shutil.copy2(license, assets / license.name)
        shutil.copy2(PORT / 'README.md', assets / 'README.md')
        shutil.copy2(PORT / 'PROVENANCE.json', assets / 'PROVENANCE.json')
        shutil.copy2(build / 'build-info.json', assets / 'build-info.json')
        if 'lvdata' in selected:
            examples = assets / 'examples'; examples.mkdir(exist_ok=True)
            shutil.copy2(PORT / 'examples/measurements.csv', examples / 'measurements.csv')
        if 'lvedit' in selected:
            examples = assets / 'examples'; examples.mkdir(exist_ok=True)
            shutil.copy2(PORT / 'examples/editor-demo.c', examples / 'editor-demo.c')
        if (build/'packed-font-data.json').exists():
            shutil.copy2(build/'packed-font-data.json', assets/'packed-font-data.json')
    for app in selected: print('Built', build / (app+'.fxe'))


if __name__ == '__main__': main()
