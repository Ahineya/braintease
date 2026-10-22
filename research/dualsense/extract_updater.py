#!/usr/bin/env python3
"""Statically unpack the pinned Sony Windows updater. Never executes its code.

ISSetupStream v4 layout/decoding follows ISx by YX Hao/lifenjoiner:
https://github.com/Coldblackice/InstallShield-installer-extractor-ISx/tree/098e866fa5341db4424d3831d40943c01b88aefe

MIT License
Copyright (c) 2017 lifenjoiner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil
import struct
import tempfile
import zlib

INSTALLER_SHA256 = "37d2d7337e83558486fb4a811368ec76f2d5cedb329207040e9b5a55a9029e43"
MAX_MEMBER = 256 * 1024 * 1024
CHUNK = 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def members(data):
    require(len(data) >= 64 and data[:2] == b"MZ", "Not a PE executable")
    pe = struct.unpack_from("<I", data, 60)[0]
    require(pe + 24 <= len(data) and data[pe:pe + 4] == b"PE\0\0", "Invalid PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional
    require(0 < count <= 96 and table + count * 40 <= len(data), "Invalid PE sections")
    offset = max(sum(struct.unpack_from("<II", data, table + i * 40 + 16))
                 for i in range(count))
    require(offset + 46 <= len(data), "Missing archive header")
    signature, count, kind = struct.unpack_from("<14sHI", data, offset)
    require(signature == b"ISSetupStream\0" and kind == 4 and 0 < count <= 128,
            "Unsupported InstallShield archive")
    offset += 46
    result, names = [], set()
    for _ in range(count):
        require(offset + 48 <= len(data), "Truncated member header")
        name_size, flags, _, size, _, compressed = struct.unpack_from("<II2sI8sH", data, offset)
        offset += 48
        require(0 < name_size <= 520 and name_size % 2 == 0 and flags == 6 and compressed == 1,
                "Unsupported member encoding")
        require(0 < size <= MAX_MEMBER and offset + name_size + size <= len(data), "Invalid member size")
        name = data[offset:offset + name_size].decode("utf-16-le").rstrip("\0")
        offset += name_size
        path = PureWindowsPath(name)
        require(name and path.name == name and not path.is_absolute()
                and not any(c in name for c in ':<>"|?*\0/\\') and name not in {".", ".."}
                and name.casefold() not in names, "Unsafe or duplicate member name")
        names.add(name.casefold())
        result.append((name, offset, size))
        offset += size
    return result


def decode(data, name, target):
    seed = name.encode("utf-8")
    key = bytes(value ^ (0x13, 0x35, 0x86, 0x07)[i % 4] for i, value in enumerate(seed))
    block = bytes(key[i % len(key)] for i in range(1024))
    key_chunk = block * (CHUNK // len(block))
    translation = bytes(~((i << 4) | (i >> 4)) & 255 for i in range(256))
    decoder, digest, size = zlib.decompressobj(), hashlib.sha256(), 0
    with target.open("xb") as stream:
        for offset in range(0, len(data), CHUNK):
            encoded = data[offset:offset + CHUNK].translate(translation)
            raw = (int.from_bytes(encoded, "little") ^ int.from_bytes(key_chunk[:len(encoded)], "little"))
            plain = decoder.decompress(raw.to_bytes(len(encoded), "little"), MAX_MEMBER - size + 1)
            require(size + len(plain) <= MAX_MEMBER and not decoder.unconsumed_tail,
                    "Decompressed member exceeds size limit")
            stream.write(plain)
            digest.update(plain)
            size += len(plain)
        require(decoder.eof and not decoder.unused_data, "Truncated or trailing compressed data")
    target.chmod(0o444)
    return {"name": name, "size_bytes": size, "sha256": digest.hexdigest()}


def extract(installer, output):
    data = installer.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    require(digest == INSTALLER_SHA256, "Installer differs from the pinned Sony download")
    require(not output.exists(), "Output must be a new directory")
    entries = members(data)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".unpack-", dir=output.parent))
    try:
        records = []
        for name, offset, size in entries:
            record = decode(data[offset:offset + size], name, temporary / name)
            record.update(payload_offset=offset, compressed_bytes=size)
            records.append(record)
        (temporary / "extraction.json").write_text(json.dumps({
            "installer_sha256": digest, "format": "ISSetupStream v4",
            "operation": "static archive decoding; no vendor code execution", "members": records,
        }, indent=2) + "\n")
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary)
        raise
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("installer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    records = extract(args.installer, args.output)
    print(f"Extracted {len(records)} files to {args.output}")
