#!/usr/bin/env python3
"""Independent host oracle for the octet-based C asset diagnostic.

Only reads the selected WAD. Generated RGB565 files are canonical little-endian
octets, independent of host C type widths and target memory-bank layout.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def span(data, offset, length):
    require(0 <= offset <= len(data) and 0 <= length <= len(data) - offset,
            "serialized range outside buffer")
    return data[offset:offset + length]


def u16(data, offset):
    return struct.unpack("<H", span(data, offset, 2))[0]


def u32(data, offset):
    return struct.unpack("<I", span(data, offset, 4))[0]


def directory(data):
    require(span(data, 0, 4) in (b"IWAD", b"PWAD"), "invalid WAD signature")
    count, start = u32(data, 4), u32(data, 8)
    require(count > 0, "empty WAD directory")
    records = span(data, start, count * 16)
    result = []
    for offset, length, name in struct.iter_unpack("<II8s", records):
        span(data, offset, length)
        result.append((name.rstrip(b"\0").decode("ascii"), offset, length))
    return result


def patch_image(patch, width, height):
    require(u16(patch, 0) == width and u16(patch, 2) == height, "patch dimensions differ")
    table_end = 8 + 4 * width
    span(patch, 0, table_end)
    indexed = bytearray(width * height)
    for x in range(width):
        pos = u32(patch, 8 + x * 4)
        require(pos >= table_end, "patch column overlaps header")
        while True:
            top = span(patch, pos, 1)[0]
            if top == 255:
                break
            run = span(patch, pos + 1, 1)[0]
            require(top + run <= height, "patch post exceeds height")
            post = span(patch, pos, run + 4)
            for y, value in enumerate(post[3:3 + run]):
                indexed[(top + y) * width + x] = value
            pos += run + 4
    return indexed


def inspect(data):
    entries = directory(data)
    by_name = {name: (offset, length) for name, offset, length in entries}

    def lump(name):
        require(name in by_name, f"missing lump {name}")
        return span(data, *by_name[name])

    metrics = {"wad_length": len(data), "wad_lumps": len(entries), "malformed_checks": 10}
    pnames = lump("PNAMES")
    names = u32(pnames, 0)
    span(pnames, 4, names * 8)
    metrics["pnames"] = names
    textures = lump("TEXTURE1")
    count = u32(textures, 0)
    table_end = 4 + count * 4
    span(textures, 0, table_end)
    references = 0
    for index in range(count):
        pos = u32(textures, 4 + index * 4)
        require(pos >= table_end, "texture overlaps offset table")
        span(textures, pos, 22)
        require(u16(textures, pos + 12) and u16(textures, pos + 14), "zero texture dimension")
        patches = u16(textures, pos + 20)
        span(textures, pos + 22, patches * 10)
        for index in range(patches):
            require(u16(textures, pos + 26 + index * 10) < names, "texture patch index outside PNAMES")
            references += 1
    metrics.update(texture1=count, texture_patch_references=references)

    map_names = ("THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS",
                 "NODES", "SECTORS", "REJECT", "BLOCKMAP")
    sizes = (10, 14, 30, 4, 12, 4, 28, 26, 1, 2)
    labels = ("things", "linedefs", "sidedefs", "vertexes", "segs", "subsectors",
              "nodes", "sectors", "reject_octets", "blockmap_words")
    markers = [i for i, entry in enumerate(entries) if entry[0] == "E1M1"]
    require(bool(markers), "missing E1M1")
    marker = markers[-1]
    require(marker + 10 < len(entries), "truncated E1M1 namespace")
    maps = []
    counts = []
    for i, (name, size, label) in enumerate(zip(map_names, sizes, labels)):
        entry_name, offset, length = entries[marker + i + 1]
        require(entry_name == name, f"unexpected E1M1 lump {entry_name}")
        require(length % size == 0, f"partial {name} record")
        maps.append(span(data, offset, length))
        counts.append(length // size)
        metrics["e1m1_" + label] = counts[-1]
    require(all(counts[i] for i in (0, 1, 2, 3, 4, 5, 7)), "empty required map lump")
    require(len(maps[9]) >= 8, "truncated BLOCKMAP")
    require((counts[7] ** 2 + 7) // 8 <= len(maps[8]), "truncated REJECT")
    players = [(x, y, angle) for x, y, angle, kind, flags in struct.iter_unpack("<hhHHH", maps[0]) if kind == 1]
    require(bool(players), "missing player start")
    metrics.update(zip(("e1m1_player_x", "e1m1_player_y", "e1m1_player_angle"), players[0]))
    for a, b, flags, special, tag, side0, side1 in struct.iter_unpack("<7H", maps[1]):
        require(a < counts[3] and b < counts[3], "LINEDEFS vertex reference outside VERTEXES")
        require(all(side == 65535 or side < counts[2] for side in (side0, side1)), "LINEDEFS side reference outside SIDEDEFS")
    for pos in range(0, len(maps[2]), 30):
        require(u16(maps[2], pos + 28) < counts[7], "SIDEDEFS sector reference outside SECTORS")
    for a, b, angle, line, side, offset in struct.iter_unpack("<6H", maps[4]):
        require(a < counts[3] and b < counts[3] and line < counts[1] and side <= 1, "invalid SEGS reference")
    for count, first in struct.iter_unpack("<HH", maps[5]):
        require(first + count <= counts[4], "SSECTORS range outside SEGS")
    for pos in range(0, len(maps[6]), 28):
        for child in (u16(maps[6], pos + 24), u16(maps[6], pos + 26)):
            require((child & 32767) < counts[5] if child & 32768 else child < counts[6], "invalid NODES child")

    indexed = patch_image(lump("TITLEPIC"), 320, 200)
    palette = span(lump("PLAYPAL"), 0, 768)
    raw = bytearray()
    for index in indexed:
        red, green, blue = palette[index * 3:index * 3 + 3]
        raw += struct.pack("<H", ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3))
    metrics.update(validation_ok=1, title_width=320, title_height=200)
    return metrics, bytes(raw)


def self_test(data):
    def fails(action):
        try:
            action()
        except (ValueError, struct.error):
            return
        raise AssertionError("malformed input was accepted")

    fails(lambda: directory(data[:11]))
    bad = bytearray(data)
    struct.pack_into("<I", bad, 8, 0xFFFFFFF0)
    fails(lambda: directory(bad))
    bad = bytearray(data)
    struct.pack_into("<II", bad, u32(data, 8), 0xFFFFFFFE, 4)
    fails(lambda: directory(bad))
    patch = bytes((1,0,1,0,0,0,0,0,12,0,0,0,0,1,0,7,0,255))
    require(patch_image(patch, 1, 1) == b"\x07", "valid patch failed")
    fails(lambda: patch_image(patch[:-1], 1, 1))
    for index, value in ((8, 8), (12, 1), (13, 255)):
        bad_patch = bytearray(patch)
        bad_patch[index] = value
        fails(lambda: patch_image(bad_patch, 1, 1))
    fails(lambda: patch_image(patch, 2, 1))
    return 9


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wad", type=Path)
    parser.add_argument("--write-raw", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--check-raw", type=Path)
    parser.add_argument("--check-log", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    data = args.wad.read_bytes()
    metrics, raw = inspect(data)
    result = {"wad_sha256": hashlib.sha256(data).hexdigest(), "rgb565_sha256": hashlib.sha256(raw).hexdigest(),
              "metrics": metrics}
    if args.self_test:
        result["oracle_self_tests"] = self_test(data)
    if args.write_raw:
        args.write_raw.parent.mkdir(parents=True, exist_ok=True)
        args.write_raw.write_bytes(raw)
    if args.check_raw:
        actual = args.check_raw.read_bytes()
        require(actual == raw, f"RGB565 mismatch: expected {len(raw)} octets / {result['rgb565_sha256']}, "
                f"got {len(actual)} / {hashlib.sha256(actual).hexdigest()}")
        result["raw_matches"] = True
    if args.check_log:
        actual = {key: int(value) for key, value in re.findall(r"^ASSET (\w+) (-?\d+)\s*$",
                                                               args.check_log.read_text(), re.MULTILINE)}
        for key, value in metrics.items():
            require(actual.get(key) == value, f"metric {key}: expected {value}, got {actual.get(key)}")
        result["metrics_match"] = True
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
