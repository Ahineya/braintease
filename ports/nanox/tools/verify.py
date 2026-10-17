#!/usr/bin/env python3
"""Verify the pinned archive, pristine extracted subset and derived terminal font."""
import hashlib,json,re,tarfile
from pathlib import Path
PORT=Path(__file__).resolve().parents[1]
m=json.loads((PORT/'UPSTREAM.json').read_text())
archive=PORT/m['archive']
assert hashlib.sha256(archive.read_bytes()).hexdigest()==m['sha256'], 'archive checksum'
actual={p.relative_to(PORT/'upstream').as_posix() for p in (PORT/'upstream').rglob('*') if p.is_file()}
assert actual==set(m['files']), 'imported subset differs from manifest'
with tarfile.open(archive) as source:
    for name in m['files']:
        assert (PORT/'upstream'/name).read_bytes()==source.extractfile(m['prefix']+name).read(),name
font=(PORT/'upstream/src/fonts/compiled/rom8x16.c').read_bytes().decode('latin1')
body=font.split('rom8x16_bits[] = {',1)[1].split('};',1)[0]
words=re.findall(r'0x[0-9a-fA-F]+',re.sub(r'/\*.*?\*/','',body,flags=re.S))
atlas=(PORT.parent/'graphics/fonts/rom8x16.inc').read_text()
assert [int(w,16) for w in words]==[int(w,16) for w in re.findall(r'0x[0-9a-fA-F]+',atlas)],'font atlas'
assert len(words)==4096
print(f"Nano-X: archive, {len(actual)} pristine files and 4096 font rows verified")
