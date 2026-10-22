#!/usr/bin/env python3
"""Offline factory-report audit and telemetry decoder. Never accesses USB.

The native audit checks actual PE bytes at disassembled direct call sites.
The telemetry layout is taken from the hash-pinned managed member inventory;
field labels do not establish time units or undocumented firmware semantics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent
BASE = ROOT / "analysis/followup"
NATIVE_SHA = "3bc6de1116a7ea29d0997f632613edfe7c3100b62efbf5bca6f32f9fb4bc4569"
MANAGED_SHA = "a5ccc41e04e960c867d479d3434e23d92044a731627f71646edb7b2b8dd4e409"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def telemetry_bytes(chunks):
    require(len(chunks) == 6, "Expected exactly six telemetry replies")
    payload = bytearray()
    for index, chunk in enumerate(chunks):
        require(len(chunk) == 64, "Telemetry reply must include 64 bytes")
        require(chunk[:3] == b"\x81\x70\x01", "Wrong telemetry reply selector")
        require(chunk[3] == (2 if index == 5 else 3), "Wrong telemetry continuation status")
        payload.extend(chunk[4:60])
    return bytes(payload)


def fields_from_evidence(path):
    evidence = json.loads(path.read_text())
    require(evidence["subject"]["digest"]["sha256"] == MANAGED_SHA, "Wrong managed inventory")
    normalized = evidence["normalized_result"]
    require(normalized["artifact"]["sha256"] == MANAGED_SHA, "Managed artifact mismatch")
    fields = [field for field in normalized["fields"] if field["declaring_type_token"] == "0x02000011"]
    require(len(fields) == 56 and fields[0]["name"] == "peripheralSerialNumber" and
            fields[0]["signature"]["field_type"] == "char[]", "Unexpected standard telemetry layout")
    return fields


def decode_telemetry(chunks, fields):
    data = telemetry_bytes(chunks)
    # Native RVA 0x3400 skips the leading byte when copying its assembled data.
    # FieldMarshal row for field 0x0400007e: 1e 11, fixed array of 17 elements;
    # ClassLayout is sequential, packing 1. Interpret this captured ASCII record.
    serial = data[1:18]
    require(len(serial) == 17 and all(32 <= byte < 127 for byte in serial), "Expected 17 printable identity bytes")
    position = 18
    values = {}
    for field in fields[1:]:
        kind = field["signature"]["field_type"]
        require(kind in ("u1", "u2", "u4"), "Unsupported telemetry field type")
        size = {"u1": 1, "u2": 2, "u4": 4}[kind]
        require(position + size <= len(data), "Truncated telemetry data")
        require(field["name"] not in values, "Duplicate telemetry field")
        values[field["name"]] = int.from_bytes(data[position:position + size], "little")
        position += size
    return {
        "format_byte": data[0], "assembled_bytes": len(data),
        "serial_redacted": True, "decoded_numeric_fields": values,
        "known_fields_end": position, "uninterpreted_tail_hex": data[position:].hex(),
        "limitations": ["Field labels follow the managed application's standard-controller structure.",
                        "Units and counter reset policies are not established.",
                        "Trailing fields are deliberately left uninterpreted."],
    }


def audit_native(binary, disassembly):
    sys.path.insert(0, str(ROOT / "analysis/toolchain/python"))
    import pefile
    raw = binary.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == NATIVE_SHA, "Unsupported native DLL")
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    targets = {0x1277a: "HidD_GetFeature_thunk", 0x12780: "HidD_SetFeature_thunk",
               0x10490: "factory_selector_sender"}
    sites = {name: [] for name in targets.values()}
    for line in disassembly.read_text().splitlines():
        match = re.match(r"^([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(.*)", line)
        if not match:
            continue
        code = bytes.fromhex(match[2])
        if len(code) != 5 or code[0] != 0xe8:
            continue
        rva = int(match[1], 16) - base
        target = rva + 5 + int.from_bytes(code[1:], "little", signed=True)
        if target not in targets:
            continue
        require(pe.get_data(rva, len(code)) == code, "Disassembly call bytes disagree with pinned PE")
        sites[targets[target]].append(f"0x{rva:x}")
    require([len(sites[name]) for name in targets.values()] == [10, 3, 1], "Unexpected direct HID call inventory")
    # Pin the positive factory telemetry packet construction and the misleading
    # 0xa0 immediate: this occurrence is a memset length, not a report ID.
    controls = {
        "telemetry_packet_header": (0x3496, "66c745908070b280c6459201"),
        "a0_memset_size": (0x6c05, "41b8a0000000"),
        "a0_memset_call": (0x6c2a, "e821ce0100"),
    }
    for name, (rva, expected) in controls.items():
        require(pe.get_data(rva, len(bytes.fromhex(expected))).hex() == expected, f"Changed {name}")
    return {"native_sha256": NATIVE_SHA, "direct_call_sites_rva": sites,
            "byte_checked_controls": {name: f"0x{rva:x}" for name, (rva, _) in controls.items()},
            "limitations": ["This is an inventory of explicit direct calls to selected boundaries, not a complete dynamic call graph.",
                            "No identified use of reports 0x0b/0x0c in reviewed HID paths does not prove firmware lacks handlers."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telemetry-log", type=Path, help="Existing workbench JSONL containing one complete telemetry request")
    args = parser.parse_args()
    out = BASE / "report-trace"
    out.mkdir(exist_ok=True)
    audit = audit_native(BASE / "payload/Data1/libfwupdater.dll", BASE / "libfwupdater-disassembly.txt")
    (out / "static-report-audit.json").write_text(json.dumps(audit, indent=2) + "\n")
    if args.telemetry_log:
        events = [json.loads(line) for line in args.telemetry_log.read_text().splitlines()]
        require(any(row.get("event") == "complete" and row.get("ok") for row in events), "Incomplete or failed telemetry run")
        chunks = [bytes.fromhex(row["hex"]) for row in events
                  if row.get("event") == "get_end" and row.get("id") == 0x81 and row.get("rc") == 0
                  and bytes.fromhex(row["hex"])[:3] == b"\x81\x70\x01"]
        decoded = decode_telemetry(chunks, fields_from_evidence(BASE / "managed-members.json"))
        decoded["transcript_sha256"] = hashlib.sha256(args.telemetry_log.read_bytes()).hexdigest()
        (out / "telemetry-decoded.json").write_text(json.dumps(decoded, indent=2) + "\n")
        print(f"Decoded {len(decoded['decoded_numeric_fields'])} numeric telemetry fields; identity redacted")
    print(f"Saved static report-call audit under {out}")


if __name__ == "__main__":
    main()
