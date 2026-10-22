#!/usr/bin/env python3
"""Bounded, offline test of host-resource key reuse; never a controller flasher.

No guessed passwords or key-space search. Checks only three observed resource
keys and three whole strings that happen to decode to AES-sized Base64 values.
These tests do not establish the unknown controller cipher, mode or format.
"""

import argparse
import base64
import json
import re
import shutil
import subprocess
import zlib

from analyze import compression_probe, entropy, vector_candidates
from trace_updater import ASSEMBLY_SHA256, BASE, ROOT, Trace, require
from unpack_resources import Reader, check, digest, identity, inflate, resources, strings, unwrap

FIRMWARE = "firmware/dualsense/type-0004/0x0630/FWUPDATE0004.bin"
FIRMWARE_SHA = "861ad95a99b16805ff20f5690372988662c4f69c6ad0b266e793d7fc2292315a"


def encoded_candidates(rows):
    found = []
    for row in rows:
        value = row["value"]
        if re.fullmatch(r"(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{48}|[0-9a-fA-F]{64})", value):
            found.append((f"string-hex-{row['key']:x}", bytes.fromhex(value), bytes(16)))
        elif re.fullmatch(r"[A-Za-z0-9+/]{20,44}={0,2}", value):
            try:
                raw = base64.b64decode(value, validate=True)
            except ValueError:
                continue  # Invalid Base64 is not a candidate.
            if len(raw) in (16, 24, 32):
                found.append((f"string-base64-{row['key']:x}", raw, bytes(16)))
    return found


def resource_key(data):
    reader = Reader(data)
    header = reader.take(reader.number("<H"))
    iv = reader.take(reader.number("B"))
    require(len(iv) == 16, "Unsupported IV")
    header = Reader(bytes(v ^ iv[i % len(iv)] for i, v in enumerate(header)))
    require(header.string() == "licF8865AC8", "Unknown resource signature")
    flags, algorithm, source, size = [header.number("B") for _ in range(4)]
    require(flags in (1, 3) and algorithm == 2 and source < 64 and size == 32,
            "Unexpected resource parameters")
    key = header.take(size)
    require(header.pos == len(header.data), "Unexpected resource suffix")
    return key, iv


def openssl_run(executable, arguments, data):
    result = subprocess.run([executable, *arguments], input=data, capture_output=True)
    require(result.returncode == 0, f"OpenSSL operation failed: {result.stderr.decode(errors='replace')}")
    return result.stdout


def crypto_edges(trace):
    refs = {r["token"]: r for r in trace.n["member_refs"]
            if r.get("parent_name", "").startswith("System.Security.Cryptography.")}
    result = []
    for edge in trace.n["call_edges"]:
        if edge["target_token"] in refs:
            ref = refs[edge["target_token"]]
            result.append({"caller": edge["caller_token"], "target": edge["target_token"],
                           "label": ref["parent_name"] + "." + ref["name"]})
    for edge in trace.edges():
        if edge["encoded_metadata_target"] in refs:
            result.append({"caller": edge["caller_token"], "target": edge["encoded_metadata_target"],
                           "label": edge["target_label"], "via": "proxy"})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    args = parser.parse_args()
    require(bool(args.openssl), "OpenSSL executable required")
    trace = Trace(BASE / "managed-members.json", BASE / "payload/Data1/playstationaccessories.dll")
    trace.recover()
    calls = crypto_edges(trace)
    for token in sorted({r["caller"] for r in calls}):
        (BASE / "crypto-audit" / (token + ".cil.txt")).write_text(trace.listing(token))
    outer = resources(trace.data, [("leyD", 0)])["leyD"]
    decoded, _ = unwrap(outer, args.openssl)
    image = decoded[40:]
    check("assembly", image)
    embedded = resources(image, [("tSHbR", 0), ("cDKfK", 40935)])
    plain, _ = unwrap(embedded["tSHbR"], args.openssl)
    candidates = [(name, *resource_key(raw)) for name, raw in [("leyD", outer), *embedded.items()]]
    candidates += encoded_candidates(strings(plain))
    firmware = (ROOT / FIRMWARE).read_bytes()
    require(len(firmware) == 950272 and digest(firmware) == FIRMWARE_SHA, "Unsupported firmware bytes")
    samples = [(0, offset) for offset in (0x100, 0x110)]
    samples += [(region, region + offset) for region in (0x4F000, 0xAF000)
                for offset in (0x60, 0x70, 0xC0)]
    rows, cmac_rows = [], []
    for name, key, resource_iv in candidates:
        for region, start in samples:
            sample = firmware[start:start + 65536]
            variants = [("ecb", "unused", None)]
            for mode in ("cbc", "ctr"):
                variants += [(mode, label, iv) for label, iv in
                             [("zero", bytes(16)), ("preceding-block", firmware[start - 16:start]),
                              ("resource-iv", resource_iv)]]
            for mode, iv_label, iv in variants:
                arguments = ["enc", "-d", f"-aes-{len(key) * 8}-{mode}", "-nopad", "-K", key.hex()]
                if iv is not None:
                    arguments += ["-iv", iv.hex()]
                output = openssl_run(args.openssl, arguments, sample)
                try:
                    unpacked, _ = inflate(output)
                    codec = identity(unpacked)
                except ValueError:
                    codec = None  # Rejection of an unrecognized stream is a negative probe result.
                rows.append({"candidate": name, "region": region, "start": start,
                             "mode": mode, "iv_hypothesis": iv_label, "output": identity(output),
                             "entropy": entropy(output), "zlib_ratio": len(zlib.compress(output)) / len(output),
                             "conventional_vectors": vector_candidates(output),
                             "validated_zlib_gzip": compression_probe(output)["validated_zlib_gzip_streams"],
                             "host_resource_codec": codec})
        for label, data in [("header-before-f0", firmware[:0xF0]),
                            ("header-with-zero-f0", firmware[:0xF0] + bytes(16))]:
            mac = openssl_run(args.openssl, ["mac", "-macopt", f"cipher:AES-{len(key) * 8}-CBC",
                                            "-macopt", "hexkey:" + key.hex(), "CMAC"], data)
            cmac_rows.append({"candidate": name, "coverage_hypothesis": label,
                              "matches_f0_hypothesis": bytes.fromhex(mac.decode().strip()) == firmware[0xF0:0x100]})
    result = {"operation": "Finite host-resource key reuse hypotheses, files only",
              "host_sha256": ASSEMBLY_SHA256, "firmware_file": FIRMWARE, "firmware_sha256": FIRMWARE_SHA,
              "candidates": [{"name": name, "key_bytes": len(key), "key_sha256": digest(key)}
                             for name, key, _ in candidates],
              "sample_bytes_per_trial": 65536, "trials": rows, "header_cmac_hypotheses": cmac_rows,
              "recovered_crypto_call_edges": calls,
              "crypto_caller_cil_sha256": {t: trace.methods[t]["body"]["il_sha256"]
                                           for t in sorted({r["caller"] for r in calls})},
              "limits": ["Controller cipher, key derivation, authenticated ranges and payload offsets are unknown.",
                         "Only explicit key bytes were tested; no derived keys, truncated keys or expanded key search.",
                         "No vector table, compression stream or entropy metric is a universal code detector.",
                         "CBC/CTR/ECB and three IV hypotheses do not cover every firmware transformation."]}
    out = BASE / "crypto-audit/key-reuse-probes.json"
    out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"candidates": len(candidates), "trials": len(rows),
                      "conventional_vector_hits": sum(len(r["conventional_vectors"]) for r in rows),
                      "validated_zlib_gzip_hits": sum(len(r["validated_zlib_gzip"]) for r in rows),
                      "host_codec_hits": sum(r["host_resource_codec"] is not None for r in rows),
                      "header_cmac_matches": sum(r["matches_f0_hypothesis"] for r in cmac_rows),
                      "entropy_range": [min(r["entropy"] for r in rows), max(r["entropy"] for r in rows)],
                      "report": str(out)}, indent=2))


if __name__ == "__main__":
    main()
