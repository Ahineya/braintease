#!/usr/bin/env python3
"""Import the pinned wolfSSL client sources from the verified audit archive."""
from pathlib import Path
import argparse
import hashlib
import io
import json
import tarfile

PORT = Path(__file__).resolve().parent
CRYPTO = 'cryptocb misc sha sha256 sha512 hmac hash random aes rsa ecc sp_int sp_c32 asn asn_orig asn_tsp coding wc_port logging signature wc_encrypt kdf error memory'.split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    metadata = json.loads((PORT / 'AUDIT.json').read_text())
    archive = args.archive.read_bytes()
    if hashlib.sha256(archive).hexdigest() != metadata['archive_sha256']:
        raise SystemExit('wolfSSL archive hash does not match AUDIT.json')
    destination = PORT / 'upstream'
    if destination.exists():
        raise SystemExit('upstream already exists; verify its manifest instead of replacing local work')
    manifest = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as source:
        for item in source.getmembers():
            relative = Path(*Path(item.name).parts[1:])
            parts = relative.parts
            selected = (parts and parts[0] == 'wolfssl' and relative.suffix == '.h'
                        or len(parts) == 2 and parts[0] == 'src' and relative.suffix in ('.c', '.h')
                        or len(parts) == 3 and parts[:2] == ('wolfcrypt', 'src')
                        and relative.name in {n + '.c' for n in [*CRYPTO, 'evp', 'evp_pk']}
                        or str(relative) in ('COPYING', 'LICENSING', 'README.md', 'ChangeLog.md'))
            if not selected or not item.isfile():
                continue
            if '..' in parts or relative.is_absolute():
                raise SystemExit('unsafe archive path')
            data = source.extractfile(item).read()
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
            manifest[str(relative)] = hashlib.sha256(data).hexdigest()
    for name, expected in metadata['files'].items():
        if manifest.get(name) != expected:
            raise SystemExit(f'{name}: source manifest mismatch')
    (PORT / 'upstream-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    print(f'Imported {len(manifest)} original wolfSSL {metadata["version"]} files')


if __name__ == '__main__':
    main()
