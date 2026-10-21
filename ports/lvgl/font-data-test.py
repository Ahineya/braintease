#!/usr/bin/env python3
"""Prove C16 packed font storage is lossless and allocation-failure safe."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
from adapt import prepare as prepare_lvgl

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl/font-data-tests')
args = parser.parse_args()
BUILD = args.build.resolve()
BUILD.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('packed_fonts', PORT/'fonts/pack.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
fonts = pack.prepare(BUILD)
manifest = json.loads((BUILD/'packed-font-data.json').read_text())
source, config = prepare_lvgl(BUILD)
fixture = []
for face, adapted in zip(manifest['fonts'], fonts):
    size = face['size']
    raw = Path(face['source']).read_text()
    text = adapted.read_text().split('\n/* Generated C16 storage adapter;')[0]
    original_match, _ = pack.bitmap(raw, size)
    text = re.sub(rf'static const uint16_t packed_bitmap_{size}\[\]=\{{.*?\}};',
                  lambda _: original_match[0], text, count=1, flags=re.S)
    text = text.replace('#include "lvgl.h"\n#include "font-data.h"', '#include "lvgl.h"')
    text = text.replace(f'static lv_font_fmt_txt_dsc_t data_{size}=', f'static const lv_font_fmt_txt_dsc_t data_{size}=')
    text = text.replace('.glyph_bitmap=NULL', f'.glyph_bitmap=bitmap_{size}')
    assert text == raw, f'Unexpected non-storage change in {adapted}'
    fixture += [f'extern const lv_font_t gx_font_{size}, reference_font_{size};']
fixture += ['static const struct face faces[]={']
fixture += [f'{{&gx_font_{f["size"]},&reference_font_{f["size"]},{f["coverage_octets"]}UL,{f["glyph_count"]}UL}},'
            for f in manifest['fonts']]
fixture += ['};']
(BUILD/'font-data-fixture.h').write_text('\n'.join(fixture)+'\n')
flags = ['-std=c99', '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all',
         '-DLV_CONF_INCLUDE_SIMPLE=1', '-DGX_FONT_DATA_TEST=1',
         '-I'+str(source), '-I'+str(config), '-I'+str(PORT), '-I'+str(BUILD)]
if sys.platform == 'darwin':
    flags += ['-isysroot', '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
compiler = '/usr/bin/clang' if sys.platform == 'darwin' else 'cc'
headers = hashlib.sha256((PORT/'font-data.h').read_bytes()+(BUILD/'font-data-fixture.h').read_bytes())
for folder in (source, config):
    for path in sorted(folder.rglob('*.h')):
        headers.update(path.read_bytes())
prefix = headers.digest()
jobs = [(PORT/'font-data.c', ['-Dmalloc=font_data_test_malloc', '-Dfree=font_data_test_free'])]
jobs += [(path, []) for path in fonts]
jobs += [(Path(f['source']), [f'-Dgx_font_{f["size"]}=reference_font_{f["size"]}']) for f in manifest['fonts']]
jobs += [(PORT/'font-data-test.c', [])]

def compile_one(item):
    path, extra = item
    key = hashlib.sha256(prefix+path.read_bytes()+repr(flags+extra).encode()).hexdigest()[:16]
    output = BUILD/(path.stem+'-'+key+'.o')
    if not output.exists():
        p = subprocess.run([compiler, *flags, *extra, '-c', str(path), '-o', str(output)], capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(p.stderr)
    return output

with ThreadPoolExecutor(max_workers=4) as pool:
    objects = list(pool.map(compile_one, jobs))
program = BUILD/'font-data-test'
subprocess.run([compiler, *flags, *map(str, objects), '-o', str(program)], check=True)
subprocess.run([str(program)], check=True)
noop = BUILD/'host-noop.c'
noop.write_text('#include "font-data.h"\n#include <assert.h>\n'
                'int main(void) { assert(!gx_font_data_open()); gx_font_data_close(); '
                'assert(!gx_font_data_open()); gx_font_data_close(); gx_font_data_close(); return 0; }\n')
host_flags = [f for f in flags if f != '-DGX_FONT_DATA_TEST=1']
subprocess.run([compiler, *host_flags, str(PORT/'font-data.c'), str(noop), '-o', str(BUILD/'host-noop')], check=True)
subprocess.run([str(BUILD/'host-noop')], check=True)
for f in manifest['fonts']:
    assert hashlib.sha256(Path(f['source']).read_bytes()).hexdigest() == f['source_sha256']
inputs = [PORT/'font-data.c', PORT/'font-data.h', PORT/'font-data-test.c', PORT/'font-data-test.py', PORT/'fonts/pack.py']
report = dict(status='passed', sanitizer='undefined', **manifest,
              checks=['every A8 coverage octet identical to original six faces',
                      'all glyph metrics and cmap mappings preserved',
                      'reversible source transform changes storage only',
                      'open before LVGL initialization needs no LVGL state',
                      'each of six allocation failures rolls back all faces',
                      'idempotent open preserves immutable bitmap pointers',
                      'three repeated open/close cycles and double close',
                      'odd final octet and malformed decoder arguments',
                      'byte-addressed host lifecycle is a no-op without packed faces',
                      'original baked sources unmodified'],
              input_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
              executable_sha256=hashlib.sha256(program.read_bytes()).hexdigest())
(BUILD/'result.json').write_text(json.dumps(report, indent=2)+'\n')
print(f'Packed font storage passed: {report["coverage_octets"]} original octets, '
      f'{report["saved_static_cells"]} fewer C16 static cells, exact A8 heap storage', flush=True)
