#!/usr/bin/env python3
"""Inspect Intel's public TLV firmware container; never load or execute it.

Reports the ma-b0-gf-a0 firmware ABI and image sections needed for the HP port.
Layout reference: OpenBSD sys/dev/pci/if_iwxreg.h, iwx_tlv_ucode_header,
iwx_ucode_tlv and iwx_fw_cmd_version. Opaque payloads stay uninterpreted.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<II64sIIQ')
MAX_BYTES = 16 * 1024 * 1024


def inspect(data):
    if not HEADER.size <= len(data) <= MAX_BYTES:
        raise ValueError('firmware container size outside bounds')
    zero, magic, description, version, build, _ = HEADER.unpack_from(data)
    if zero or magic != 0x0a4c5749:
        raise ValueError('not an Intel TLV firmware container')
    counts = collections.Counter()
    sections, commands, revisions = [], [], []
    at = HEADER.size
    records = 0
    while at < len(data):
        if len(data) - at < 8:
            raise ValueError('truncated TLV header')
        kind, size = struct.unpack_from('<II', data, at)
        at += 8
        padded = (size + 3) & ~3
        if padded > len(data) - at:
            raise ValueError('TLV payload/padding exceeds container')
        if records >= 65536:
            raise ValueError('too many firmware records')
        records += 1
        counts[kind] += 1
        if kind in (19,20,21,24,25,26):
            if size < 4:
                raise ValueError('truncated image section address')
            sections.append(dict(type=kind, address=struct.unpack_from('<I',data,at)[0],
                                 file_offset=at+4, bytes=size-4))
        elif kind == 36:
            if size != 12:
                raise ValueError('invalid firmware version record')
            revisions.append(list(struct.unpack_from('<III',data,at)))
        elif kind == 48:
            if counts[kind] > 1 or size % 4:
                raise ValueError('invalid/duplicate command-version record')
            commands = [dict(command=data[p],group=data[p+1],version=data[p+2],notification=data[p+3])
                        for p in range(at,at+size,4)]
        at += padded
    return dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),
                description=description.split(b'\0',1)[0].decode('ascii',errors='replace'),
                header_version=version,build=build,firmware_revisions=revisions,
                tlv_counts=dict(sorted(counts.items())),sections=sections,commands=commands,
                hardware_status='container inspected only; no device initialized')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware',type=Path)
    args=parser.parse_args()
    # Read at most the explicit bound even for a malicious or mistaken input.
    with args.firmware.open('rb') as source:
        data=source.read(MAX_BYTES+1)
    try:
        print(json.dumps(inspect(data),indent=2))
    except ValueError as error:
        parser.exit(1,f'Wi-Fi firmware: {error}\n')


if __name__=='__main__':
    main()
