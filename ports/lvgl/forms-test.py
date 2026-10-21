#!/usr/bin/env python3
"""Independent Graphite forms checks against the adapted LVGL under UBSan."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from adapt import sources

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl')
args = parser.parse_args()
BUILD = args.build.resolve()
FOLDER = BUILD / 'forms-host'
FOLDER.mkdir(exist_ok=True)
CONFIG = FOLDER / 'config'
CONFIG.mkdir(exist_ok=True)
(CONFIG / 'lv_conf.h').write_bytes((BUILD / 'config/lv_conf.h').read_bytes())
FLAGS = ['-std=c99', '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all',
         '-DLV_CONF_INCLUDE_SIMPLE=1', '-I' + str(BUILD / 'src'), '-I' + str(CONFIG),
         '-I' + str(PORT), '-I' + str(ROOT / 'ports/graphics/include')]
if sys.platform == 'darwin':
    FLAGS += ['-isysroot', '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
COMPILER = '/usr/bin/clang' if sys.platform == 'darwin' else 'cc'
hasher = hashlib.sha256(repr(FLAGS).encode() + (CONFIG / 'lv_conf.h').read_bytes())
for path in sorted((BUILD / 'src').rglob('*.h')):
    hasher.update(path.read_bytes())
for path in sorted(PORT.glob('*.h')):
    hasher.update(path.read_bytes())
PREFIX = hasher.digest()

def compile_one(path):
    extra = (PORT / 'ui-forms.c').read_bytes() if path.name == 'forms-test.c' else b''
    digest = hashlib.sha256(PREFIX + path.read_bytes() + extra).hexdigest()[:16]
    output = FOLDER / (path.stem + '-' + digest + '.o')
    if not output.exists():
        p = subprocess.run([COMPILER, *FLAGS, '-c', str(path), '-o', str(output)], capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(p.stderr)
    return output

units = [*sources(BUILD / 'src'),
         *[PORT / 'fonts' / f'font-{n}.c' for n in json.loads((PORT / 'fonts/generated.json').read_text())['sizes']],
         PORT / 'fonts/symbols.c', PORT / 'icons/data.c', PORT / 'ui-scale.c', PORT / 'ui.c',
         PORT / 'ui-data.c', PORT / 'browser.c', PORT / 'host-gx.c', PORT / 'forms-test.c']
with ThreadPoolExecutor(max_workers=4) as pool:
    objects = list(pool.map(compile_one, units))
program = FOLDER / 'forms-test'
subprocess.run([COMPILER, *FLAGS, *map(str, objects), '-lm', '-o', str(program)], check=True)
fixtures = FOLDER / 'fixtures'
fixtures.mkdir(exist_ok=True)
for name in ('existing.txt', 'other.txt'):
    (fixtures / name).write_text('Forms test fixture.\n')
for scale in (1, 2):
    subprocess.run([str(program)], cwd=fixtures, env=dict(os.environ, HOME=str(fixtures.resolve()), LVGL_SCALE=str(scale)), check=True)
result = dict(status='passed', sanitizer='undefined', scales=[1, 2],
              input_sha256={name: hashlib.sha256((PORT / name).read_bytes()).hexdigest()
                            for name in ('ui-forms.c', 'ui-forms.h', 'browser.c', 'browser.h', 'ui-data.c', 'ui-data.h',
                                         'ui.c', 'forms-test.c', 'forms-test.py', 'host-gx.c')})
(FOLDER / 'forms-component-result.json').write_text(json.dumps(result, indent=2) + '\n')
