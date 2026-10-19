#!/usr/bin/env python3
"""Import the pinned, pristine QuickJS archive; never overwrite changed sources."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile

PORT = Path(__file__).resolve().parent


def verify():
    manifest = json.loads((PORT / 'upstream-manifest.json').read_text())
    for name, digest in manifest.items():
        path = PORT / 'upstream' / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('Modified upstream source: ' + name)
    actual = {p.relative_to(PORT / 'upstream').as_posix()
              for p in (PORT / 'upstream').rglob('*') if p.is_file()}
    if actual != manifest.keys():
        raise ValueError('Unexpected or missing upstream files')
    return manifest


def vendor(archive):
    pin = json.loads((PORT / 'UPSTREAM.json').read_text())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        raise ValueError('Archive checksum differs from UPSTREAM.json')
    files = {}
    with tarfile.open(archive) as bundle:
        for member in bundle:
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe archive path')
            if not member.isfile():
                if not member.isdir():
                    raise ValueError('Unexpected archive member: ' + member.name)
                continue
            if path.parts[0] != 'quickjs-' + pin['version'] or len(path.parts) < 2:
                raise ValueError('Unexpected archive root')
            name = PurePosixPath(*path.parts[1:]).as_posix()
            if name in files:
                raise ValueError('Duplicate archive member: ' + name)
            files[name] = bundle.extractfile(member).read()
    if not files:
        raise ValueError('Empty archive')
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    destination = PORT / 'upstream'
    if destination.exists():
        if verify() != manifest:
            raise ValueError('Existing sources differ from the pinned archive')
        return
    for name, data in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (PORT / 'upstream-manifest.json').write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    print(f'Imported {len(files)} pristine QuickJS files')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, nargs='?')
    args = parser.parse_args()
    if args.archive:
        vendor(args.archive)
    else:
        print(f'Verified {len(verify())} pristine QuickJS files')
