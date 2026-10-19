#!/usr/bin/env python3
"""Compile and exercise the actual NetSurf browser core on the host ABI."""
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import hashlib
import os
PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from adapt import prepare
from build import ROOT, LIBRARIES, sources
from core import core_sources
work = ROOT / 'build/netsurf-core-host'
source = prepare(work / 'source')
output = work / 'objects'
output.mkdir(parents=True, exist_ok=True)
flags = ['-std=c99', '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE',
         '-DWITHOUT_ICONV_FILTER', '-DNDEBUG', '-DRIPPLE_MMU=1', '-include', 'strings.h',
         '-I' + str(PORT / 'config'), '-I' + str(source / 'netsurf'),
         '-I' + str(source / 'netsurf/include'), '-I' + str(source / 'netsurf/content/handlers'),
         '-I' + str(source / 'libnsutils/include')]
flags += ['-I' + str(source / library / 'include') for library in LIBRARIES]
flags += ['-I' + str(source / 'libdom/bindings')]
digest = hashlib.sha256(str(flags).encode())
for directory in (PORT / 'config', source):
    for path in sorted(directory.rglob('*')):
        if path.is_file() and path.suffix in ('.h', '.inc'):
            digest.update(str(path).encode()); digest.update(path.read_bytes())
for path in (ROOT / 'runtime/include/ripple_unicode.h', ROOT / 'libraries/unicode/tables.inc'):
    digest.update(path.read_bytes())
base = digest.digest()
tasks = [p for _, p in sources(source)] + core_sources(source) + [ROOT / 'libraries/unicode/unicode.c']
def compile_one(path):
    name = '_'.join(path.relative_to(source).parts) if path.is_relative_to(source) else path.name
    obj = output / (name + '.o')
    stamp = obj.with_suffix('.key')
    key = hashlib.sha256(base + path.read_bytes()).hexdigest()
    if obj.exists() and stamp.exists() and stamp.read_text() == key:
        return obj
    includes = []
    if path.is_relative_to(source):
        includes = ['-I' + str(source / path.relative_to(source).parts[0] / 'src')]
    result = subprocess.run(['cc', *includes, *flags, '-c', str(path), '-o', str(obj)], capture_output=True)
    obj.with_suffix('.log').write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(str(obj.with_suffix('.log')))
    stamp.write_text(key)
    return obj
with ThreadPoolExecutor(max_workers=3) as pool:
    objects = list(pool.map(compile_one, tasks))
executable = work / 'netsurfcorecheck'
result = subprocess.run(['cc', *flags, str(PORT / 'tests/core.c'), *map(str, objects), '-o', str(executable)],
                        capture_output=True, text=True)
if result.stderr:
    print(result.stderr, file=sys.stderr, end='')
if result.returncode:
    raise SystemExit(result.returncode)
try:
    result = subprocess.run([str(executable)], timeout=30)
except subprocess.TimeoutExpired:
    raise SystemExit('NetSurf host core acceptance exceeded 30 seconds')
raise SystemExit(result.returncode)
