"""Create a regular GPT/FAT32 UEFI removable-media image; never open devices."""
from pathlib import Path
import hashlib
import os
import stat
import struct
import tempfile
import uuid
import zlib


def boot_disk(esp):
    esp = Path(esp)
    payload = (esp / 'EFI/BOOT/BOOTX64.EFI').read_bytes()
    sector, cluster_sectors, reserved = 512, 4, 32
    sectors, first, volume_sectors = 260 * 2048, 2048, 256 * 2048
    # A conservative upper bound leaves enough FAT entries for every cluster.
    fat_sectors = ((volume_sectors // cluster_sectors + 2) * 4 + sector - 1) // sector
    data_sector = reserved + 2 * fat_sectors
    clusters = (volume_sectors - data_sector) // cluster_sectors
    count = (len(payload) + sector * cluster_sectors - 1) // (sector * cluster_sectors)
    if not count or count + 3 > clusters:
        raise RuntimeError('UEFI payload exceeds the USB image EFI partition')
    image = bytearray(sectors * sector)
    image[446:462] = struct.pack('<B3sB3sII', 0, b'\x00\x02\x00', 0xee, b'\xff\xff\xff', 1, sectors - 1)
    image[510:512] = b'\x55\xaa'
    identity = uuid.uuid5(uuid.NAMESPACE_OID, hashlib.sha256(payload).hexdigest())
    entries = bytearray(128 * 128)
    entries[:16] = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    entries[16:32] = uuid.uuid5(identity, 'esp').bytes_le
    struct.pack_into('<QQQ', entries, 32, first, first + volume_sectors - 1, 0)
    name = 'Riplix EFI System'.encode('utf-16-le')
    entries[56:56 + len(name)] = name

    def header(current, backup, table):
        result = bytearray(sector)
        struct.pack_into('<8sIIIIQQQQ16sQIII', result, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         current, backup, 34, sectors - 34, identity.bytes_le,
                         table, 128, 128, zlib.crc32(entries))
        struct.pack_into('<I', result, 16, zlib.crc32(result[:92]))
        return result

    image[sector:2 * sector] = header(1, sectors - 1, 2)
    image[2 * sector:2 * sector + len(entries)] = entries
    image[(sectors - 33) * sector:(sectors - 1) * sector] = entries
    image[(sectors - 1) * sector:] = header(sectors - 1, 1, sectors - 33)
    start = first * sector
    boot = bytearray(sector)
    boot[:11] = b'\xeb\x58\x90RIPLIX  '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, sector, cluster_sectors, reserved,
                     2, 0, 0, 0xf8, 0, 63, 255, first, volume_sectors)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    struct.pack_into('<BBBI', boot, 64, 0x80, 0, 0x29, 0x5249504c)
    boot[71:90] = b'RIPLIXBOOT FAT32   '
    boot[510:512] = b'\x55\xaa'
    info = bytearray(sector)
    struct.pack_into('<I', info, 0, 0x41615252)
    struct.pack_into('<III', info, 484, 0x61417272, clusters - count - 3,
                     count + 5 if count + 3 < clusters else 0xffffffff)
    struct.pack_into('<I', info, 508, 0xaa550000)
    for at, data in [(0, boot), (1, info), (6, boot), (7, info)]:
        image[start + at * sector:start + (at + 1) * sector] = data
    fat = bytearray(fat_sectors * sector)
    for index, value in [(0, 0x0ffffff8), (1, 0x0fffffff), (2, 0x0fffffff),
                         (3, 0x0fffffff), (4, 0x0fffffff)]:
        struct.pack_into('<I', fat, index * 4, value)
    for index in range(5, 5 + count):
        struct.pack_into('<I', fat, index * 4, index + 1 if index < 4 + count else 0x0fffffff)
    for at in (reserved, reserved + fat_sectors):
        image[start + at * sector:start + at * sector + len(fat)] = fat
    data_start = start + data_sector * sector
    cluster = cluster_sectors * sector

    def entry(directory, index, name, attributes, first_cluster, size=0):
        at = data_start + (directory - 2) * cluster + index * 32
        image[at:at + 11] = name
        image[at + 11] = attributes
        struct.pack_into('<H', image, at + 20, first_cluster >> 16)
        struct.pack_into('<HHHI', image, at + 22, 0, 33, first_cluster & 65535, size)

    entry(2, 0, b'RIPLIXBOOT ', 8, 0)
    entry(2, 1, b'EFI        ', 0x10, 3)
    entry(3, 0, b'.          ', 0x10, 3)
    entry(3, 1, b'..         ', 0x10, 0)
    entry(3, 2, b'BOOT       ', 0x10, 4)
    entry(4, 0, b'.          ', 0x10, 4)
    entry(4, 1, b'..         ', 0x10, 3)
    entry(4, 2, b'BOOTX64 EFI', 0x20, 5, len(payload))
    at = data_start + 3 * cluster
    image[at:at + len(payload)] = payload
    path = esp.parent / 'usb-boot.img'
    if path.is_symlink() or (path.exists() and not stat.S_ISREG(path.stat().st_mode)):
        raise RuntimeError(f'USB image destination must be a regular file: {path}')
    with tempfile.TemporaryDirectory(prefix='.usb-boot-', dir=path.parent) as directory:
        temporary = Path(directory) / 'image'
        with temporary.open('xb') as target:
            target.write(image); target.flush(); os.fsync(target.fileno())
        os.replace(temporary, path)
    return path
