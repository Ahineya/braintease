#!/usr/bin/env python3
"""Create a FAT16 disk image and add files to the root directory."""
import argparse
import os
import struct
import sys

BPS = 512
SPC = 4
RESERVED = 1
NUM_FATS = 2
ROOT_ENT = 512
MEDIA = 0xF8
SECS_PER_FAT = 32  # 32 * 512 / 2 = 8192 FAT entries
TOTAL_SECS = 20480  # 10 MiB


def u16(x):
    return struct.pack("<H", x)


def u32(x):
    return struct.pack("<I", x)


def canon83(name):
    base, ext = os.path.splitext(name.upper())
    ext = ext[1:] if ext.startswith(".") else ext
    return (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")


def fat16_image(files):
    root_secs = (ROOT_ENT * 32 + BPS - 1) // BPS
    fat_off = RESERVED * BPS
    root_off = (RESERVED + NUM_FATS * SECS_PER_FAT) * BPS
    data_off = root_off + root_secs * BPS
    cluster_bytes = SPC * BPS

    img = bytearray(TOTAL_SECS * BPS)
    bpb = bytearray(512)
    bpb[0:3] = b"\xEB\x3C\x90"
    bpb[3:11] = b"MSDOS5.0"
    bpb[11:13] = u16(BPS)
    bpb[13] = SPC
    bpb[14:16] = u16(RESERVED)
    bpb[16] = NUM_FATS
    bpb[17:19] = u16(ROOT_ENT)
    bpb[19:21] = u16(TOTAL_SECS if TOTAL_SECS < 65536 else 0)
    bpb[21] = MEDIA
    bpb[22:24] = u16(SECS_PER_FAT)
    bpb[24:26] = u16(32)
    bpb[26:28] = u16(2)
    bpb[32:36] = u32(TOTAL_SECS)
    bpb[36] = 0x80
    bpb[38] = 0x29
    bpb[39:43] = u32(0x12345678)
    bpb[43:54] = b"RIPPLEROS  "
    bpb[54:62] = b"FAT16   "
    bpb[510:512] = b"\x55\xAA"
    img[0:512] = bpb

    def fat_set(clus, val):
        off = fat_off + clus * 2
        img[off:off + 2] = u16(val)
        off2 = fat_off + SECS_PER_FAT * BPS + clus * 2
        img[off2:off2 + 2] = u16(val)

    fat_set(0, 0xFF00 | MEDIA)
    fat_set(1, 0xFFFF)

    next_clus = 2
    dir_index = 0

    def add_file(name, data):
        nonlocal next_clus, dir_index
        size = len(data)
        if size == 0:
            first = 0
        else:
            first = next_clus
            remaining = data
            prev = None
            while remaining:
                clus = next_clus
                next_clus += 1
                start = data_off + (clus - 2) * cluster_bytes
                chunk = remaining[:cluster_bytes]
                img[start:start + len(chunk)] = chunk
                remaining = remaining[cluster_bytes:]
                if prev is not None:
                    fat_set(prev, clus)
                prev = clus
            fat_set(prev, 0xFFFF)

        ent_off = root_off + dir_index * 32
        dir_index += 1
        img[ent_off:ent_off + 11] = canon83(name)
        img[ent_off + 11] = 0x20
        img[ent_off + 26:ent_off + 28] = u16(first)
        img[ent_off + 28:ent_off + 32] = u32(size)

    for path, out_name in files:
        with open(path, "rb") as f:
            add_file(out_name, f.read())
    return img


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", required=True)
    p.add_argument("files", nargs="*", help="src:DESTNAME or src (basename used)")
    args = p.parse_args()
    parsed = []
    for item in args.files:
        if ":" in item:
            src, dest = item.split(":", 1)
        else:
            src, dest = item, os.path.basename(item)
        if not os.path.isfile(src):
            print("missing", src, file=sys.stderr)
            sys.exit(1)
        parsed.append((src, dest))
    img = fat16_image(parsed)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(img)
    print("wrote", args.output, "files", len(parsed))


if __name__ == "__main__":
    main()
