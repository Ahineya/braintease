#!/usr/bin/env python3
"""Import the pinned wolfSSH release without modifying upstream sources."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile

PORT = Path(__file__).resolve().parent
VERSION = '1.6.0'
ARCHIVE_SHA256 = 'e345b580643af6715a4293ed7bab4e99ea562bb379adb8898eeae55c38b12f56'
URL = 'https://codeload.github.com/wolfSSL/wolfssh/tar.gz/refs/tags/v1.6.0-stable'


def import_extra_crypto(path):
    """Import the original crypto sources omitted from the TLS subset."""
    manifest = json.loads((PORT / 'crypto/source.json').read_text())
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != manifest['archive_sha256']:
        raise SystemExit('wolfSSL archive hash mismatch')
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for name, expected in manifest['files'].items():
            matches = [item for item in archive.getmembers()
                       if item.isfile() and Path(*Path(item.name).parts[1:]) == Path('wolfcrypt/src') / name]
            if len(matches) != 1:
                raise SystemExit(f'wolfSSL archive must contain exactly one {name}')
            contents = archive.extractfile(matches[0]).read()
            if hashlib.sha256(contents).hexdigest() != expected:
                raise SystemExit(f'{name} source hash mismatch')
            output = PORT / 'crypto' / name
            if output.exists() and output.read_bytes() != contents:
                raise SystemExit(f'refusing to replace modified {name}')
            output.write_bytes(contents)
            print(f'Verified/imported original wolfCrypt {name}')



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, nargs='?')
    parser.add_argument('--wolfcrypt-archive', type=Path)
    args = parser.parse_args()
    if args.wolfcrypt_archive:
        import_extra_crypto(args.wolfcrypt_archive)
    if args.archive is None:
        if not args.wolfcrypt_archive:
            parser.error('supply a wolfSSH archive or --wolfcrypt-archive')
        return
    data = args.archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit('wolfSSH archive hash mismatch')
    destination = PORT / 'upstream'
    if destination.exists():
        raise SystemExit('upstream already exists; refusing to replace local work')
    manifest = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for item in archive.getmembers():
            relative = Path(*Path(item.name).parts[1:])
            if relative.is_absolute() or '..' in relative.parts:
                raise SystemExit('unsafe archive path')
            if not item.isfile():
                continue
            selected = (relative.parts and relative.parts[0] in ('src', 'wolfssh')
                        and relative.suffix in ('.c', '.h')) or str(relative) in (
                            'COPYING', 'LICENSING', 'README.md', 'ChangeLog.md')
            if not selected:
                continue
            contents = archive.extractfile(item).read()
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(contents)
            manifest[str(relative)] = hashlib.sha256(contents).hexdigest()
    (PORT / 'source.json').write_text(json.dumps(dict(version=VERSION, url=URL,
        archive_sha256=ARCHIVE_SHA256, files=manifest), indent=2, sort_keys=True) + '\n')
    print(f'Imported {len(manifest)} original wolfSSH {VERSION} files')


if __name__ == '__main__':
    main()
