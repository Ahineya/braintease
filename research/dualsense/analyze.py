#!/usr/bin/env python3
"""Offline container inspection; never executes firmware or accesses hardware."""
import argparse
from collections import Counter
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import struct
import zlib

from fetch import ROOT, catalog, verify

COPYRIGHT = b"Copyright (C) "
BLOCK = 16


def entropy(data):
    if not data:
        return 0.0
    return -sum((n / len(data)) * math.log2(n / len(data)) for n in Counter(data).values())


def cstring(data):
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace").rstrip()


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def header(data):
    if len(data) < 256 or not data.startswith(COPYRIGHT):
        raise ValueError("missing observed main header")
    return {
        "build_date": cstring(data[0x40:0x50]),
        "build_time": cstring(data[0x50:0x60]),
        "format_word_observed": struct.unpack_from("<H", data, 0x60)[0],
        "usb_product_id": struct.unpack_from("<H", data, 0x62)[0],
        "word_64_unknown": u32(data, 0x64),
        "word_68_region_size_candidate": u32(data, 0x68),
        "word_6c_unknown": u32(data, 0x6C),
        "internal_firmware_version": u32(data, 0x70),
        "updater_family": u32(data, 0x74),
        "update_version": u32(data, 0x78),
        "description": cstring(data[0x80:0xE0]),
        "packaging_timestamp_candidate": cstring(data[0xE0:0xF0]),
        "opaque_last_16_header_bytes": data[0xF0:0x100].hex(),
    }


def decode_firmware_report(data):
    if len(data) != 64 or data[0] != 0x20:
        raise ValueError("expected 64-byte firmware feature report 0x20")
    return {"build_date": cstring(data[1:12]), "build_time": cstring(data[12:20]),
            "hardware_version": u32(data, 24), "internal_firmware_version": u32(data, 28),
            "update_version": struct.unpack_from("<H", data, 44)[0]}


def without_uniform_tail(data):
    if not data or data[-1] not in (0, 255):
        return data
    end = len(data.rstrip(bytes([data[-1]])))
    return data[:end] if len(data) - end >= 64 else data


def regions(data):
    # Observed copyright markers at page boundaries define candidate regions.
    # This is deliberately not a claim that the format has been fully decoded.
    starts = [m.start() for m in re.finditer(re.escape(COPYRIGHT), data)
              if m.start() % 4096 == 0]
    if not starts or starts[0] != 0:
        raise ValueError("missing first region")
    result = []
    for index, (start, end) in enumerate(zip(starts, starts[1:] + [len(data)])):
        skip = 0x100 if index == 0 else 0x60
        if end - start < skip:
            raise ValueError("truncated region header")
        region = {
            "index": index, "offset": start, "end": end, "size": end - start,
            "label": "main" if index == 0 else cstring(data[start + 0x38:start + 0x40]),
            "payload_offset_hypothesis": start + skip,
            "clear_header_hex": data[start:start + skip].hex(),
            "secondary_words_40_60": [] if index == 0 else
                [u32(data, p) for p in range(start + 0x40, start + 0x60, 4)],
        }
        if index:
            is_betty = region["label"].startswith("Betty")
            length_offset = 0x58 if is_betty else 0x48
            version_offset = 0x5C if is_betty else 0x4C
            time_offset = 0x48 if is_betty else 0x50
            active_end = start + skip + len(without_uniform_tail(data[start + skip:end]))
            region.update({"secondary_update_version": u32(data, start + 0x40),
                "secondary_timestamp_candidate": cstring(data[start + time_offset:start + time_offset + 16]),
                "secondary_length_candidate": u32(data, start + length_offset),
                "secondary_version_word": u32(data, start + version_offset),
                "active_end_before_uniform_tail": active_end,
                "unexplained_overhead_above_length_field": active_end - start - u32(data, start + length_offset)})
        result.append(region)
    return result


def blocks(data):
    return [data[p:p + BLOCK] for p in range(0, len(data) - BLOCK + 1, BLOCK)]


def summarize_payload(data):
    counts = Counter(blocks(data))
    duplicated = {b: n for b, n in counts.items() if n > 1}
    windows = [entropy(data[p:p + 4096]) for p in range(0, len(data), 4096)
               if len(data[p:p + 4096]) == 4096]
    strings = list(re.finditer(rb"[ -~]{12,}", data))
    runs = [{"offset": m.start(), "length": len(m.group()), "value": m.group()[0]}
            for m in re.finditer(rb"\x00{64,}|\xff{64,}", data)]
    active = without_uniform_tail(data)
    return {
        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "entropy_bits_per_byte": entropy(data),
        "entropy_4k_min": min(windows) if windows else None,
        "entropy_4k_max": max(windows) if windows else None,
        "entropy_4k_mean": sum(windows) / len(windows) if windows else None,
        "zlib_compressed_ratio": len(zlib.compress(data, 9)) / len(data) if data else None,
        "repeated_aligned_16byte_values": len(duplicated),
        "extra_repeated_aligned_16byte_blocks": sum(n - 1 for n in duplicated.values()),
        "repeated_nonuniform_aligned_16byte_values": sum(
            b not in (b"\0" * 16, b"\xff" * 16) for b in duplicated),
        "bytes_excluding_uniform_tail": len(active),
        "entropy_excluding_uniform_tail": entropy(active),
        "zlib_ratio_excluding_uniform_tail": len(zlib.compress(active, 9)) / len(active) if active else None,
        "uniform_runs_64plus": runs,
        "ascii_runs_12plus": [{"offset": m.start(), "text": m.group().decode("ascii")}
                               for m in strings],
    }


def vector_candidates(data):
    """Conventional Cortex-M4 table filter, not an architecture detector.

    Test every word-aligned offset. RAM range is deliberately broad; require
    reserved exception entries zero and plausible Thumb handler addresses.
    No result does not exclude relocated/nonstandard/encrypted ARM code.
    """
    found = []
    for off in range(0, len(data) - 63, 4):
        sp, reset = struct.unpack_from("<II", data, off)
        if not (0x20000000 <= sp < 0x40000000 and sp % 8 == 0):
            continue
        if not (reset & 1 and 0 < reset < 0x20000000):
            continue
        words = struct.unpack_from("<16I", data, off)
        if any(words[i] for i in (7, 8, 9, 10, 13)):
            continue
        handlers = [words[i] for i in (1, 2, 3, 4, 5, 6, 11, 12, 14, 15)]
        if any(p and not (p & 1 and p < 0x20000000) for p in handlers):
            continue
        if sum(bool(p) for p in handlers) < 4:
            continue
        found.append({"offset": off, "stack": sp, "reset": reset})
    return found


def compression_probe(data):
    signatures = {
        "elf": b"\x7fELF", "gzip": b"\x1f\x8b\x08", "xz": b"\xfd7zXZ\0",
        "zip": b"PK\x03\x04", "zstd": b"\x28\xb5\x2f\xfd", "lz4_frame": b"\x04\x22\x4d\x18",
    }
    hits = {name: [m.start() for m in re.finditer(re.escape(sig), data)]
            for name, sig in signatures.items()}
    # Validate zlib/gzip wrappers with checksums and a bounded output. A magic
    # match or successful short prefix alone never counts as a decoded stream.
    candidates = [(p, "gzip", 31) for p in hits["gzip"]]
    candidates += [(p, "zlib", 15) for p in range(len(data) - 1)
                   if data[p] == 0x78 and (data[p] * 256 + data[p + 1]) % 31 == 0]
    decoded = []
    for p, name, mode in candidates:
        try:
            decoder = zlib.decompressobj(mode)
            output = decoder.decompress(data[p:], 8 * 1024 * 1024 + 1)
            if decoder.eof and len(output) <= 8 * 1024 * 1024:
                decoded.append({"offset": p, "format": name, "output_bytes": len(output),
                                "input_bytes": len(data) - p - len(decoder.unused_data)})
        except zlib.error:
            pass
    return {"signature_hits_only": hits, "zlib_gzip_candidates": len(candidates),
            "validated_zlib_gzip_streams": decoded}


def compare(left, right):
    if len(left) != len(right):
        raise ValueError("comparison requires equal region sizes")
    a, b = blocks(left), blocks(right)
    best, start, longest, equal = [], 0, 0, 0
    for i, (x, y) in enumerate(zip(left, right)):
        if x == y:
            equal += 1
            if i == 0 or left[i - 1] != right[i - 1]:
                start = i
            length = i - start + 1
            longest = max(longest, length)
            if i == len(left) - 1 or left[i + 1] != right[i + 1]:
                if length >= 16:
                    best.append({"offset": start, "length": length})
    return {
        "bytes": len(left), "equal_bytes": equal,
        "equal_byte_ratio": equal / len(left) if left else None,
        "longest_equal_run": longest,
        "equal_runs_16plus": sorted(best, key=lambda r: -r["length"])[:12],
        "equal_aligned_16byte_blocks": sum(x == y for x, y in zip(a, b)),
        "shared_aligned_16byte_values_at_any_block_index": len(set(a) & set(b)),
        "shared_nonuniform_aligned_16byte_values": len(
            (set(a) & set(b)) - {b"\0" * 16, b"\xff" * 16}),
        "total_16byte_blocks": len(a),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis")
    args = parser.parse_args()
    sources, entries = catalog()
    images, summary = {}, []
    args.output.mkdir(parents=True, exist_ok=True)
    for entry in sorted(entries, key=lambda e: (e["updaterId"], int(e["version"], 16))):
        data = (ROOT / entry["file"]).read_bytes()
        verify(data, entry)
        main_header = header(data)
        if main_header["update_version"] != int(entry["version"], 16):
            raise ValueError("catalog/header version mismatch")
        if main_header["updater_family"] != int(entry["updaterId"][-4:], 16):
            raise ValueError("catalog/header family mismatch")
        rs = regions(data)
        for region in rs:
            region["payload_statistics"] = summarize_payload(data[region["payload_offset_hypothesis"]:region["end"]])
        item = {"file": entry["file"], "sha256": entry["sha256"].lower(),
                "family": entry["updaterId"], "version": entry["version"],
                "header": main_header, "regions": rs,
                "conventional_cortex_m4_vector_candidates": vector_candidates(data),
                "compression": compression_probe(data)}
        images[entry["file"]] = data
        summary.append(item)
        # Detailed entropy map is kept out of version control.
        stem = entry["updaterId"] + "-" + entry["version"]
        with (args.output / (stem + "-entropy.csv")).open("w") as out:
            out.write("offset,bytes,entropy\n")
            for p in range(0, len(data), 4096):
                chunk = data[p:p + 4096]
                out.write(f"{p},{len(chunk)},{entropy(chunk):.8f}\n")
        print(stem, "regions", [r["label"] for r in rs], flush=True)

    pairs = set()
    for _, group in itertools.groupby(summary, key=lambda e: e["family"]):
        group = list(group)
        pairs.update((a["file"], b["file"]) for a, b in zip(group, group[1:]))
    for a, b in itertools.combinations(summary, 2):
        if a["version"] == b["version"] and a["family"] != b["family"]:
            pairs.add((a["file"], b["file"]))
    # Compare current 0004 and Rhino families, which both label audio VenomB.
    pairs.add(("firmware/dualsense/type-0004/0x0630/FWUPDATE0004.bin",
               "firmware/dualsense/type-000E/0x0641/FWUPDATE000E.bin"))
    diffs = []
    for left, right in sorted(pairs):
        a, b = images[left], images[right]
        ra, rb = regions(a), regions(b)
        result = {"left": left, "right": right, "whole_file": compare(a, b), "regions": []}
        if [(r["offset"], r["end"]) for r in ra] == [(r["offset"], r["end"]) for r in rb]:
            for x, y in zip(ra, rb):
                sa, sb = x["payload_offset_hypothesis"], y["payload_offset_hypothesis"]
                result["regions"].append({"index": x["index"], "left_label": x["label"],
                    "right_label": y["label"], "payload_start": sa,
                    **compare(a[sa:x["end"]], b[sb:y["end"]])})
        diffs.append(result)
    # Check all image pairs for shared blocks independently of the selected
    # detailed bytewise diffs. Only 16-byte-aligned blocks are covered here.
    sharing = []
    for index in range(3):
        sets = {}
        for image in summary:
            r = image["regions"][index]
            payload = images[image["file"]][r["payload_offset_hypothesis"]:r["end"]]
            sets[image["file"]] = set(blocks(payload)) - {b"\0" * 16, b"\xff" * 16}
        checked = 0
        matches = []
        for left, right in itertools.combinations(sets, 2):
            checked += 1
            common = sets[left] & sets[right]
            if common:
                matches.append({"left": left, "right": right, "shared_values": len(common)})
        sharing.append({"region_index": index, "pairs_checked": checked, "pairs_with_shared_blocks": matches})
    device = json.loads((ROOT / "controller.json").read_text())
    decoded = decode_firmware_report(bytes.fromhex(device["feature_report_20_hex"]))
    match_keys = ("build_date", "build_time", "internal_firmware_version", "update_version")
    matches = [i["file"] for i in summary if all(i["header"][k] == decoded[k] for k in match_keys)]
    report = {"analysis_version": 2, "archive_commit": sources["archive_commit"],
              "input_hashes": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                               for name in ("analyze.py", "fetch.py", "catalog.json", "controller.json")},
              "controller_report_decoded": decoded, "controller_metadata_matches": matches,
              "images": summary, "comparisons": diffs, "all_pairs_block_sharing": sharing}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Wrote", args.output / "results.json")


if __name__ == "__main__":
    main()
