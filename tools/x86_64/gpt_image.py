"""Small GPT metadata fixtures; even multi-terabyte test disks stay sparse."""
import base64
import json
import hashlib
import stat
from pathlib import Path
import struct
import uuid
import zlib

ESP = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
LINUX = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le


def fixture(sector=512, sectors=None):
    sectors = sectors or 64 * 1024 * 1024 // sector
    entries = bytearray(128 * 128)
    for i, (kind, first, last, name) in enumerate([
            (ESP, 2048, 4095, 'EFI System'), (LINUX, 4096, sectors - 2049, 'Ubuntu')]):
        struct.pack_into('<16s16sQQQ72s', entries, i * 128, kind,
                         uuid.UUID(int=100 + i).bytes_le, first, last, 0, name.encode('utf-16le'))
    blocks = (len(entries) + sector - 1) // sector
    guid = uuid.UUID('12345678-1234-5678-9abc-123456789abc').bytes_le

    def header(lba, alternate, array):
        raw = bytearray(sector)
        struct.pack_into('<8sIIIIQQQQ16sQIII', raw, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         lba, alternate, 2 + blocks, sectors - 2 - blocks,
                         guid, array, 128, 128, zlib.crc32(entries))
        struct.pack_into('<I', raw, 16, zlib.crc32(raw[:92]))
        return raw

    mbr = bytearray(sector)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xee, b'\xff'*3, 1, min(sectors - 1, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    return dict(sector=sector, bytes=sectors * sector, spans={
        0: mbr, sector: header(1, sectors - 1, 2), 2 * sector: entries,
        (sectors - blocks - 1) * sector: bytearray(entries),
        (sectors - 1) * sector: header(sectors - 1, 1, sectors - blocks - 1)})


def write_image(path, data):
    """Create only a new regular sparse fixture; never overwrite or open a device."""
    with Path(path).open('xb') as out:
        out.truncate(data['bytes'])
        for at, span in data['spans'].items():
            out.seek(at); out.write(span)


def load_capture(path):
    data = json.loads(Path(path).read_text())
    return dict(sector=data['sector'], bytes=data['bytes'],
                spans={int(at): bytearray(base64.b64decode(raw)) for at, raw in data['spans'].items()})


def validate_probe_image(path, sector):
    info = Path(path).lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size < 68 * sector or info.st_size % sector:
        raise RuntimeError(f'probe disk must be a regular sector-aligned image, not a device or symlink: {path}')


def fingerprint(path):
    """Full hash for small images; named metadata coverage for sparse TB tests.

    QEMU always opens the backend read-only. The large-image sample is an extra
    check, not a claim to have hashed every unwritten byte of a sparse image.
    """
    path = Path(path)
    size = path.stat().st_size
    digest = hashlib.sha256()
    with path.open('rb') as source:
        if size <= 128 * 1024 * 1024:
            while data := source.read(1024 * 1024):
                digest.update(data)
            coverage = 'whole-image'
        else:
            digest.update(source.read(1024 * 1024))
            source.seek(-1024 * 1024, 2); digest.update(source.read(1024 * 1024))
            coverage = 'first-and-last-1MiB'
    return dict(bytes=size, sha256=digest.hexdigest(), coverage=coverage)
