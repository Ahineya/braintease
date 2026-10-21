#!/usr/bin/env python3
"""Isolated Graphite text viewport tests and 1x/2x reference renders under UBSan."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from adapt import prepare, sources

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl/editor-component')
args = parser.parse_args()
BUILD = args.build.resolve()
BUILD.mkdir(parents=True, exist_ok=True)
SOURCE, CONFIG = prepare(BUILD)
FLAGS = ['-std=c99', '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all',
         '-DLV_CONF_INCLUDE_SIMPLE=1', '-I'+str(SOURCE), '-I'+str(CONFIG),
         '-I'+str(PORT), '-I'+str(ROOT/'ports/graphics/include')]
if sys.platform == 'darwin':
    FLAGS += ['-isysroot', '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
COMPILER = '/usr/bin/clang' if sys.platform == 'darwin' else 'cc'
digest = hashlib.sha256(repr(FLAGS).encode())
for folder in (SOURCE, CONFIG, PORT):
    for path in sorted(folder.rglob('*.h')):
        digest.update(path.read_bytes())
PREFIX = digest.digest()
OBJECTS = BUILD/'objects'
OBJECTS.mkdir(exist_ok=True)

def compile_one(path):
    extra = (PORT/'ui-editor.c').read_bytes() if path.name == 'ui-editor-test.c' else b''
    if path.name == 'text.c':
        extra += b''.join(p.read_bytes() for p in sorted(path.parent.glob('*.inc')))
    if path.name == 'unicode.c':
        extra += (path.parent/'tables.inc').read_bytes()
    flags = FLAGS + (['-Dgx_lv_clipboard=editor_reference_unused_clipboard'] if path.name == 'host-gx.c' else [])
    key = hashlib.sha256(PREFIX+path.read_bytes()+extra+repr(flags).encode()).hexdigest()[:20]
    output = OBJECTS/(path.stem+'-'+key+'.o')
    if not output.exists():
        p = subprocess.run([COMPILER, *flags, '-c', str(path), '-o', str(output)], capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(str(path)+'\n'+p.stderr)
    return output

units = [*sources(SOURCE),
         *[PORT/'fonts'/f'font-{n}.c' for n in json.loads((PORT/'fonts/generated.json').read_text())['sizes']],
         PORT/'fonts/symbols.c', PORT/'icons/data.c', PORT/'ui-scale.c', PORT/'ui.c',
         PORT/'ui-data.c', PORT/'host-gx.c', ROOT/'libraries/text/text.c',
         ROOT/'libraries/unicode/unicode.c', PORT/'ui-editor-test.c']
with ThreadPoolExecutor(max_workers=4) as pool:
    objects = list(pool.map(compile_one, units))
program = BUILD/'ui-editor-test'
subprocess.run([COMPILER, *FLAGS, *map(str, objects), '-lm', '-o', str(program)], check=True)
for scale in (1, 2):
    subprocess.run([str(program)], env=dict(os.environ, LVGL_SCALE=str(scale),
                   LVCALC_REFERENCE=str(BUILD/f'editor-{scale}x.ppm')), check=True)
inputs = [PORT/'ui-editor.c', PORT/'ui-editor.h', PORT/'ui-editor-test.c',
          PORT/'ui-editor-test.py', ROOT/'libraries/text/text.c', ROOT/'libraries/text/text.h',
          ROOT/'libraries/unicode/unicode.c']
report = dict(status='passed', sanitizer='undefined', densities=[1, 2],
              checks=['borrowed TextBuffer ownership', 'extended grapheme arrow/backspace/hit testing',
                      'CRLF-preserving atomic newline', '12 KiB clipboard paste one undo group',
                      'clipboard overflow preserves selection', 'proportional pixel wrapping past 512 columns',
                      'horizontal scroll and visual-row keyboard movement', 'syntax runs',
                      'mouse drag selection without pointer panning',
                      'focus-loss input reset and buffer switch restore scrolling/caret',
                      'caret-only incremental pixels equal full redraw',
                      'incremental layout and constant child count', 'idle rendering stops'],
              input_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs})
(BUILD/'result.json').write_text(json.dumps(report, indent=2)+'\n')
print('Graphical editor component checks passed at 1x/2x under UBSan', flush=True)
