#!/usr/bin/env python3
"""Reject physical images that would overwrite the kernel or user allocation."""
from pathlib import Path
import struct
import sys

BANK = 16384
INIT_CODE = 10
APP_CODE = 12
IMAGE_BANKS = 2


def check(kind, data):
    if kind == 'kernel':
        if len(data) < 15 or data[:5] != b'RLINK':
            raise ValueError('invalid kernel header')
        bank, entry, count = struct.unpack_from('<HII', data, 5)
        if bank != BANK or entry != 4 or not count or count > INIT_CODE * BANK:
            raise ValueError('kernel must start after vector 0 and fit below code bank 10')
    elif kind in ('init', 'program'):
        if len(data) < 24 or data[:4] != b'SYS1':
            raise ValueError('invalid init header')
        bank, code, gp, stack, entry, count, size = struct.unpack_from('<4H3I', data, 4)
        if (bank, code, gp, stack) != (BANK, INIT_CODE if kind == 'init' else APP_CODE, 3, 4):
            raise ValueError('init uses the wrong physical layout')
        if not count or count > IMAGE_BANKS * BANK or entry >= count or size > BANK:
            raise ValueError('init exceeds its physical allocation')
        if len(data) != 24 + count * 8 + size:
            raise ValueError('invalid init section lengths')
    else:
        raise ValueError('unknown image kind')


if __name__ == '__main__':
    try:
        check(sys.argv[1], Path(sys.argv[2]).read_bytes())
    except (ValueError, OSError) as error:
        raise SystemExit(f'physical image check failed: {error}')
