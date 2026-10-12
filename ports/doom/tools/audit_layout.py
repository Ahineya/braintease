#!/usr/bin/env python3
"""Inventory GP storage and startup code in the provided RCC assembly files.

Counts target cells and target instructions, never host file bytes. Supply every
linked assembly unit (including runtime units) for a complete data inventory.
This is not a replacement for the linker's placement/relocation checks.
"""
import argparse
import json
import re
from pathlib import Path


def inspect(path):
    data = False
    initializer = False
    symbols = []
    label = None
    init_instructions = 0
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        if line == '.data':
            data = True
            continue
        if line == '.code':
            data = False
            continue
        if line.endswith(':'):
            label = line[:-1]
            if label.startswith('__rcc_init_'):
                initializer = True
            continue
        if data:
            match = re.fullmatch(r'\.space\s+(0x[0-9a-fA-F]+|[0-9]+)', line)
            if not match:
                raise ValueError(f'{path}:{number}: unrecognized data directive: {line}')
            symbols.append({'name': label, 'cells': int(match[1], 0)})
        elif initializer and not line.startswith('.'):
            init_instructions += 1
            if line.split()[0] == 'RET':
                initializer = False
    return {'file': str(path), 'data_cells': sum(s['cells'] for s in symbols),
            'initializer_instructions': init_instructions,
            'symbols': sorted(symbols, key=lambda s: s['cells'], reverse=True)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('assembly', type=Path, nargs='+')
    parser.add_argument('--bank-size', type=int, default=64000)
    parser.add_argument('--data-offset', type=int, default=1032)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--check-limit', action='store_true')
    args = parser.parse_args()
    if args.bank_size <= 0 or args.data_offset < 0:
        parser.error('bank-size must be positive; data-offset must be nonnegative')
    units = [inspect(path) for path in args.assembly]
    total = sum(unit['data_cells'] for unit in units)
    result = {'units': units, 'total_data_cells': total,
              'assumed_data_offset': args.data_offset,
              'bank_size': args.bank_size,
              'fits_single_gp_bank': total + args.data_offset <= args.bank_size}
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for unit in sorted(units, key=lambda unit: unit['data_cells'], reverse=True):
            print(f"{unit['file']}: {unit['data_cells']} data cells; "
                  f"{unit['initializer_instructions']} initializer instructions")
            for symbol in unit['symbols'][:5]:
                print(f"  {symbol['cells']:>8}  {symbol['name']}")
        print(f'Total: {total} data cells; {total + args.data_offset}/{args.bank_size} '
              'cells including assumed data offset')
    return int(args.check_limit and not result['fits_single_gp_bank'])


if __name__ == '__main__':
    raise SystemExit(main())
