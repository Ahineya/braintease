#!/usr/bin/env python3
"""Freeze the host-built ROM/minimal/virtio platform for an OS bootstrap."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
OBJECTS = {'arch.o', 'kernel.o', 'console.o', 'input.o', 'timer.o', 'acpi.o',
           'runtime.o', 'vectors.o', 'pci.o', 'block.o', 'root.o', 'unsupported.o', 'irq.o'}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate(base):
    objects = {p.name for p in base.glob('*.o') if p.name != 'rcc.o'}
    if objects != OBJECTS:
        raise ValueError('expected the exact BOOT_TEST ROM/minimal/virtio platform object set')
    manifest = base / 'capsule.json'
    if manifest.exists():
        record = json.loads(manifest.read_text())
        if record['format'] != 'riplix-platform-capsule-1' or record['abi'] != 'x64-c16-continuations':
            raise ValueError('unsupported platform capsule identity')
        expected = OBJECTS | {'loader.obj', 'root.img', 'kernel.ld', 'build.json'}
        if set(record['files']) != expected:
            raise ValueError('incomplete platform capsule inventory')
        if any(digest(base / n) != h for n, h in record['files'].items()):
            raise ValueError('platform capsule content changed')
    return sorted(base / n for n in OBJECTS)


def freeze(base, output):
    base, output = base.resolve(), output.resolve()
    validate(base)
    if output.exists():
        raise ValueError('use a new platform capsule directory')
    sources = {n: base / n for n in OBJECTS | {'loader.obj', 'root.img', 'build.json'}}
    sources['kernel.ld'] = ROOT / 'tools/x86_64/boot/kernel.ld'
    for p in sources.values():
        if not p.is_file():
            raise ValueError('missing platform input: ' + str(p))
    # The kernel object is deliberately absent: each guest frontend build must
    # supply its own IR and pass backend/link/boot acceptance again.
    output.mkdir(parents=True)
    for n, p in sources.items():
        shutil.copyfile(p, output / n)
    record = dict(format='riplix-platform-capsule-1', abi='x64-c16-continuations',
                  profile='BOOT_TEST-ROM-minimal-virtio',
                  files={n: digest(output / n) for n in sorted(sources)},
                  origin=str(base), guest_compiled=False)
    (output / 'capsule.json').write_text(json.dumps(record, indent=2) + '\n')
    validate(output)
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    freeze(a.base, a.output)
    print('Frozen host platform:', a.output)


if __name__ == '__main__':
    main()
