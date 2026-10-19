"""Immutable compiler inputs and software-only comparison translation unit."""
import hashlib
from pathlib import Path
import re
import shutil
ROOT = Path(__file__).resolve().parents[3]

def compiler_snapshot(requested, destination):
    requested, destination = requested.resolve(), destination.resolve()
    digest = lambda path: hashlib.sha256(path.read_bytes()).digest()
    if requested != destination and (not destination.exists() or digest(requested) != digest(destination)):
        # A new compiler gets a fresh timestamp, invalidating dependent SDKs.
        shutil.copy(requested, destination)
    return destination

def software_reference(build):
    source = ROOT / 'runtime/softfloat.c'
    names = sorted(set(re.findall(r'\b(__rcc_f(?:32|64)_\w+)\(', source.read_text())))
    reference = build / 'reference.c'
    text = '#define RCC_SOFTWARE_FLOAT 1\n'
    text += ''.join(f'#define {name} reference_{name[6:]}\n' for name in names)
    text += f'#include "{source}"\n'
    if not reference.exists() or reference.read_text() != text:
        reference.write_text(text)
    return reference
