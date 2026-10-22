#!/usr/bin/env python3
"""Statically unpack protected host resources from one pinned Sony updater build.

Does not execute .NET code or firmware, access a controller, or modify inputs.
This is a build-specific data decoder, not a firmware decryptor. The compression
variant was reconstructed from the original CIL listed in crypto-and-recovery.md.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from trace_updater import ASSEMBLY_SHA256, BASE, ROOT, Trace, require

LIMIT = 2 * 1024 * 1024
EXPECTED = {
    "leyD": (44851, "4e2214ea4a92073a77fe0a64b76361e26fe039155c607083129790eb039c8029"),
    "assembly": (43520, "6c949c43476df6844e8aa251ecac14e254de0e874d398adf889716892c63ddec"),
    "tSHbR": (40931, "3d2a849a7c41db087b134a26bd68636df526531e0e5b9d9533e7350ed172b3b5"),
    "cDKfK": (1027, "5854f5ff15b62c8464e1543ea766289082757d364fac33b953e59367a5c1d786"),
}
LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LENGTH_EXTRA = [0] * 8 + [i for i in range(1, 6) for _ in range(4)] + [0]
DISTANCE_BASE = [1, 2, 3, 4] + [2 ** i + 1 + j * 2 ** (i - 1)
                               for i in range(2, 15) for j in range(2)]
DISTANCE_EXTRA = [0] * 4 + [i for i in range(1, 14) for _ in range(2)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def identity(data):
    return {"size": len(data), "sha256": digest(data)}


def check(name, data):
    require((len(data), digest(data)) == EXPECTED[name], f"Unexpected {name} bytes")


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, count):
        require(0 <= count <= LIMIT and self.pos + count <= len(self.data), "Truncated data")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def number(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def string(self):
        size = 0
        for shift in range(0, 35, 7):
            byte = self.number("B")
            require(shift < 28 or byte <= 7, "Invalid string length")
            size |= (byte & 127) << shift
            if not byte & 128:
                return self.take(size).decode("utf-8", errors="strict")
        raise ValueError("Invalid string length")


class Bits:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def read(self, count):
        require(0 <= count <= 16 and self.pos + count <= len(self.data) * 8,
                "Truncated compressed stream")
        value = sum(((self.data[(self.pos + i) // 8] >> ((self.pos + i) % 8)) & 1) << i
                    for i in range(count))
        self.pos += count
        return value

    def align(self):
        self.pos = (self.pos + 7) // 8 * 8


class Huffman:
    def __init__(self, lengths):
        require(all(0 <= n <= 15 for n in lengths), "Invalid Huffman length")
        counts = [lengths.count(i) for i in range(16)]
        counts[0] = 0
        code, next_code, self.table = 0, {}, {}
        for n in range(1, 16):
            code = (code + counts[n - 1]) << 1
            require(code + counts[n] <= 1 << n, "Oversubscribed Huffman tree")
            next_code[n] = code
        for symbol, n in enumerate(lengths):
            if n:
                self.table[n, next_code[n]] = symbol
                next_code[n] += 1

    def read(self, bits):
        value = 0
        for n in range(1, 16):
            value = (value << 1) | bits.read(1)
            if (n, value) in self.table:
                return self.table[n, value]
        raise ValueError("Invalid Huffman code")


def inflate(data, limit=LIMIT):
    """Decode the host protector's variant; never treat it as standard DEFLATE."""
    require(0 < len(data) <= LIMIT and 0 <= limit <= LIMIT, "Invalid stream size/limit")
    bits, output, blocks = Bits(data), bytearray(), []
    while True:
        last, kind = bits.read(1), bits.read(3)
        blocks.append({"bit_offset": bits.pos - 4, "last": bool(last),
                       "type": kind, "output_offset": len(output)})
        if kind == 3:
            bits.align()
            count = bits.read(16) ^ 0xBB9E
            require(len(output) + count <= limit, "Output limit exceeded")
            output.extend(bits.read(8) for _ in range(count))
        elif kind in (2, 6):
            if kind == 6:
                literals = Huffman([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8)
                distances = Huffman([5] * 32)
            else:
                nl, nd, nc = bits.read(5) + 257, bits.read(5) + 1, bits.read(4) + 4
                require(nl <= 286 and nd <= 32, "Invalid dynamic table sizes")
                order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
                lengths = [0] * 19
                for i in range(nc):
                    lengths[order[i]] = bits.read(3)
                alphabet, lengths = Huffman(lengths), []
                while len(lengths) < nl + nd:
                    symbol = alphabet.read(bits)
                    if symbol < 16:
                        lengths.append(symbol)
                    elif symbol == 16:
                        require(bool(lengths), "Repeat without a preceding length")
                        lengths.extend([lengths[-1]] * (bits.read(2) + 3))
                    elif symbol == 17:
                        lengths.extend([0] * (bits.read(3) + 3))
                    else:
                        lengths.extend([0] * (bits.read(7) + 11))
                    require(len(lengths) <= nl + nd, "Code lengths exceed declared size")
                require(lengths[256] > 0, "Missing end-of-block code")
                literals, distances = Huffman(lengths[:nl]), Huffman(lengths[nl:])
            while True:
                symbol = literals.read(bits)
                if symbol == 256:
                    break
                if symbol < 256:
                    require(len(output) < limit, "Output limit exceeded")
                    output.append(symbol)
                else:
                    require(257 <= symbol <= 285, "Reserved length code")
                    index = symbol - 257
                    count = LENGTH_BASE[index] + bits.read(LENGTH_EXTRA[index])
                    index = distances.read(bits)
                    require(index < 30, "Reserved distance code")
                    distance = DISTANCE_BASE[index] + bits.read(DISTANCE_EXTRA[index])
                    require(distance <= len(output), "Distance exceeds output history")
                    require(len(output) + count <= limit, "Output limit exceeded")
                    for _ in range(count):
                        output.append(output[-distance])
        else:
            raise ValueError(f"Unsupported compressed block type {kind}")
        if last:
            break
    require((bits.pos + 7) // 8 == len(data), "Trailing compressed data")
    return bytes(output), blocks


def unwrap(data, openssl):
    reader = Reader(data)
    header = reader.take(reader.number("<H"))
    iv = reader.take(reader.number("B"))
    require(len(iv) == 16, "Unsupported resource IV")
    header = Reader(bytes(byte ^ iv[i % len(iv)] for i, byte in enumerate(header)))
    require(header.string() == "licF8865AC8", "Unknown protector signature")
    flags, algorithm = header.number("B"), header.number("B")
    source, count = header.number("B"), header.number("B")
    require(flags in (1, 3) and algorithm == 2 and source < 64 and count == 32,
            "Unsupported resource crypto parameters")
    key = header.take(count)
    require(header.pos == len(header.data), "Unexpected resource header suffix")
    # Original CIL attempts a second key read at EOF; zero bytes leave it intact.
    payload = reader.take(len(data) - reader.pos)
    if flags & 2:
        require(len(payload) % 16 == 0, "Unaligned AES ciphertext")
        result = subprocess.run([openssl, "enc", "-d", "-aes-256-cbc", "-K", key.hex(),
                                 "-iv", iv.hex()], input=payload, capture_output=True)
        require(result.returncode == 0, "Resource AES decryption failed")
        payload = result.stdout
    plain, blocks = inflate(payload)
    return plain, {"flags": flags, "algorithm_selector": algorithm, "key_source": source,
                   "key_bytes": count, "iv_bytes": len(iv), "compressed": identity(payload),
                   "unpacked": identity(plain), "blocks": blocks}


def resources(data, entries):
    """Read known manifest offsets from hash-pinned PE/CLI resource directories."""
    sys.path.insert(0, str(ROOT / "analysis/toolchain/python"))
    import pefile

    pe = pefile.PE(data=data, fast_load=True)
    cli = pe.OPTIONAL_HEADER.DATA_DIRECTORY[14]
    cli_data = pe.get_data(cli.VirtualAddress, cli.Size)
    require(len(cli_data) >= 32, "Truncated CLI header")
    rva, size = struct.unpack_from("<II", cli_data, 24)
    require(size <= LIMIT, "Resource directory too large")
    directory = pe.get_data(rva, size)
    require(len(directory) == size, "Truncated resource directory")
    result = {}
    for name, offset in entries:
        reader = Reader(directory[offset:])
        result[name] = reader.take(reader.number("<I"))
        check(name, result[name])
    return result


def strings(data):
    reader, result = Reader(data), []
    while reader.pos < len(data):
        offset = reader.pos
        result.append({"offset": offset, "key": (offset + 27) ^ 0x6247,
                       "value": reader.string()})
    return result


def numeric_tables(data):
    """Decode all four arrays from the original 0x06000a59 CIL, not just i32."""
    reader, result = Reader(data), {}
    for name, count_key, fmt, value_key in [
            ("int32", 1489141855, "<i", 0x0A0E45CB),
            ("int64", 823301367, "<q", 1320422825),
            ("float32", 682003115, "<f", 0),
            ("float64", (158479524 + 317257137) ^ 133872560, "<d", 0)]:
        count = reader.number("<I") ^ count_key
        require(count <= LIMIT // struct.calcsize(fmt), "Numeric array too large")
        values = [reader.number(fmt) for _ in range(count)]
        result[name] = [value ^ value_key for value in values][::-1] if value_key else values[::-1]
    require(reader.pos == len(data), "Trailing numeric resource data")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    args = parser.parse_args()
    require(bool(args.openssl), "OpenSSL executable is required")
    binary = BASE / "payload/Data1/playstationaccessories.dll"
    trace = Trace(BASE / "managed-members.json", binary)
    # Preserve original-byte-checked listings, without interpreting/executing CIL.
    tokens = ["0x06000a4a", "0x06000a50", "0x06000a58", "0x06000a59", "0x06000a61",
              "0x06000a70", "0x06000a74", "0x06000a78", "0x06000a8f", "0x06000a90",
              "0x06000a94", "0x06000a95", "0x06000a96", "0x06000a97", "0x06000a98",
              "0x06000a9b", "0x06000aae", "0x06000aaf"]
    out = BASE / "crypto-audit"
    out.mkdir(exist_ok=True)
    for token in tokens:
        (out / (token + ".cil.txt")).write_text(trace.listing(token))
    outer = resources(trace.data, [("leyD", 0)])["leyD"]
    decoded, outer_info = unwrap(outer, args.openssl)
    reader = Reader(decoded)
    require(reader.number("<I") ^ 153677506 == 2, "Unexpected embedded assembly name count")
    names = ["".join(chr(ord(c) ^ 3893) for c in reader.string()) for _ in range(2)]
    require(names == ["tSHbR", "cDKfK"], "Unexpected embedded resource names")
    image = reader.take(reader.number("<I") ^ 362300432)
    require(reader.pos == len(decoded), "Trailing assembly envelope data")
    check("assembly", image)
    # Both are private embedded resources in the resource-only assembly's manifest.
    embedded = resources(image, [("tSHbR", 0), ("cDKfK", 40935)])
    decoded_strings, string_info = unwrap(embedded["tSHbR"], args.openssl)
    decoded_constants, constant_info = unwrap(embedded["cDKfK"], args.openssl)
    string_rows = strings(decoded_strings)
    require(len(string_rows) == 2560, "Unexpected string table size")
    constants = numeric_tables(decoded_constants)
    integers = constants["int32"]
    require({k: len(v) for k, v in constants.items()} ==
            {"int32": 349, "int64": 0, "float32": 7, "float64": 59},
            "Unexpected numeric table sizes")
    result = {"assembly_sha256": ASSEMBLY_SHA256,
              "operation": "Offline decoding of protected host resources; no target code execution",
              "cil_sha256": {t: trace.methods[t]["body"]["il_sha256"] for t in tokens},
              "resources": {"leyD": outer_info, "tSHbR": string_info, "cDKfK": constant_info},
              "resource_assembly": identity(image), "string_count": len(string_rows),
              "integer_count": len(integers),
              "numeric_table_counts": {k: len(v) for k, v in constants.items()},
              "integer_examples": {str(i): integers[i] for i in [0, 1, 3, 44, 151, 152]},
              "resource_inputs": {name: {"size": size, "sha256": sha}
                                  for name, (size, sha) in EXPECTED.items() if name != "assembly"},
              "firmware_file_strings": [x for x in string_rows if x["value"].startswith("FWUPDATE")],
              "limits": ["Only this exact host assembly and three expected resources are supported.",
                         "Resource AES keys are host protection data, not identified controller keys.",
                         "No firmware plaintext, authentication key, or custom execution route recovered."]}
    for name, data in [("protected-resources.dll", image), ("strings.bin", decoded_strings),
                       ("constants.bin", decoded_constants)]:
        (out / name).write_bytes(data)
    (out / "strings.json").write_text(json.dumps(string_rows, indent=2) + "\n")
    (out / "integers.json").write_text(json.dumps(integers, indent=2) + "\n")
    (out / "numeric-tables.json").write_text(json.dumps(constants, indent=2) + "\n")
    (out / "findings.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Decoded {len(string_rows)} host strings and {len(integers)} integer constants under {out}")


if __name__ == "__main__":
    main()
