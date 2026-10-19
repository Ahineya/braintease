#!/usr/bin/env python3
"""Import unmodified NetSurf dependency sources from the pinned full release."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
from adapt import write_changed

PORT = Path(__file__).resolve().parent


def vendor(archive, extend=False):
    audit = json.loads((PORT / 'AUDIT.json').read_text())
    archive = Path(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != audit['archive_sha256']:
        raise ValueError('NetSurf archive checksum does not match AUDIT.json')
    destination = PORT / 'upstream'
    if destination.exists() and not extend:
        raise ValueError('Refusing to replace the existing upstream tree')
    manifest = PORT / 'upstream-manifest.json'
    previous = json.loads(manifest.read_text()) if extend else {}
    # Validate every existing pinned file before allowing an additive import.
    for name, checksum in previous.items():
        path = destination / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError(f'Existing pinned source changed: {name}')
    files = {}
    additions = {}
    with tarfile.open(archive) as bundle:
        for member in bundle:
            parts = PurePosixPath(member.name).parts
            if '..' in parts or member.name.startswith('/'):
                raise ValueError('Unsafe archive path')
            if len(parts) < 3 or parts[0] != 'netsurf-all-' + audit['version']:
                continue
            if parts[1] not in audit['components'] or not member.isfile():
                continue
            # Tests/data are deliberately not part of the guest source import.
            if parts[2] in ('test', 'docs', 'examples'):
                continue
            name = '/'.join(parts[1:])
            data = bundle.extractfile(member).read()
            checksum = hashlib.sha256(data).hexdigest()
            files[name] = checksum
            if name in previous:
                if previous[name] != checksum:
                    raise ValueError(f'Archive differs from pinned source: {name}')
            else:
                if (destination / name).exists():
                    raise ValueError(f'Refusing to overwrite untracked source: {name}')
                additions[name] = data
    if not files:
        raise ValueError('No NetSurf sources found')
    if previous.keys() - files.keys():
        raise ValueError('Archive/component selection omits existing pinned sources')
    # All validation completes before the first write. Existing sources are
    # never rewritten; additions and the manifest use atomic file replacement.
    for name, data in additions.items():
        write_changed(destination / name, data)
    write_changed(manifest, (json.dumps(files, sort_keys=True, indent=2) + '\n').encode())
    print(f'Imported {len(additions)} new pristine files; {len(files)} total')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--extend', action='store_true', help='Add components without replacing existing files')
    args = parser.parse_args()
    vendor(args.archive, args.extend)
