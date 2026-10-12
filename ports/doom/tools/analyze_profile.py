#!/usr/bin/env python3
"""Attribute PC samples using an exact linked image and its matching objects.

The binary DEBUG section omits static function entry labels. Object labels and
surviving block labels recover those boundaries without modifying the linker.
Unrecoverable instructions remain explicitly unattributed rather than being
silently assigned to a preceding public function.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

OPCODES = {0: 'HALT', 1: 'ADD', 2: 'SUB', 3: 'AND', 4: 'OR', 5: 'XOR',
           6: 'SLL', 7: 'SRL', 8: 'SLT', 9: 'SLTU', 10: 'ADDI', 11: 'ANDI',
           12: 'ORI', 13: 'XORI', 14: 'LI', 15: 'SLLI', 16: 'SRLI',
           17: 'LOAD', 18: 'STORE', 19: 'JAL', 20: 'JALR', 21: 'BEQ',
           22: 'BNE', 23: 'BLT', 24: 'BGE', 25: 'BRK', 26: 'MUL',
           27: 'DIV', 28: 'MOD', 29: 'MULI', 30: 'DIVI', 31: 'MODI',
           32: 'LOADC', 33: 'STORC', 66: 'NOP'}


def read_image(path):
    raw = path.read_bytes()
    if raw[:5] != b'RLINK':
        raise ValueError('expected RLINK image')
    bank, entry, count = struct.unpack_from('<HII', raw, 5)
    offset = 15 + 8 * count
    data_size, = struct.unpack_from('<I', raw, offset)
    offset += 4 + data_size
    if raw[offset:offset + 5] != b'DEBUG':
        raise ValueError('missing DEBUG symbols')
    offset += 5
    symbol_count, = struct.unpack_from('<I', raw, offset)
    offset += 4
    symbols = {}
    for _ in range(symbol_count):
        length, = struct.unpack_from('<I', raw, offset)
        offset += 4
        name = raw[offset:offset + length].decode()
        offset += length
        pc, = struct.unpack_from('<I', raw, offset)
        offset += 4
        symbols[name] = pc
    return raw, bank, entry, count, symbols


def object_ranges(obj, origin, symbols, bank, raw):
    labels = {name: label['absolute_address'] // 4 for name, label in obj['labels'].items()}
    length = len(obj['instructions'])
    starts = sorted({pc for name, pc in labels.items()
                     if not name.startswith(('L_', '.')) and pc < length})
    names = {pc: sorted(name for name, loc in labels.items() if loc == pc
                       and not name.startswith(('L_', '.'))) for pc in starts}
    if not starts:
        return []
    segments = [(0, length)] if length <= bank else list(zip(starts, starts[1:] + [length]))
    output = []
    for begin, end in segments:
        anchors = {symbols[name] - pc for name, pc in labels.items()
                   if begin <= pc < end and name in symbols}
        if not anchors:
            continue
        if len(anchors) != 1:
            raise ValueError(f'{origin}: inconsistent object/binary anchors; save matching objects')
        shift = anchors.pop()
        # Relocations change operand fields, but never opcodes. Check the whole
        # segment so a stale object snapshot fails instead of inventing names.
        for pc in range(begin, end):
            if raw[15 + (pc + shift) * 8] != obj['instructions'][pc]['opcode']:
                raise ValueError(f'{origin}: stale object at local PC {pc}')
        for index, start in enumerate(starts):
            stop = starts[index + 1] if index + 1 < len(starts) else length
            if begin <= start < end:
                full_name = names[start][0]
                name = re.sub(r'^__rcc_local_.*?_[0-9a-f]{32}_', '', full_name)
                output.append({'name': name, 'symbol': full_name, 'object': origin,
                               'start': start + shift, 'end': stop + shift})
    return output


def analyze(binary, hit_path, object_dir, archives=()):
    raw, bank, entry, count, symbols = read_image(binary)
    hit_bytes = hit_path.read_bytes()
    if len(hit_bytes) != count * 8:
        raise ValueError('PC hit count does not match instruction image')
    hits = [row[0] for row in struct.iter_unpack('<Q', hit_bytes)]
    functions = []
    for path in sorted(object_dir.glob('*.pobj')):
        functions += object_ranges(json.loads(path.read_text()), path.name, symbols, bank, raw)
    for path in archives:
        for row in json.loads(path.read_text())['objects']:
            functions += object_ranges(row['object'], path.name + ':' + row['name'], symbols, bank, raw)
    functions.sort(key=lambda f: f['start'])
    for previous, current in zip(functions, functions[1:]):
        if previous['end'] > current['start']:
            raise ValueError('overlapping object function ranges')
    starts = [f['start'] for f in functions]
    counts = [Counter() for _ in functions]
    spills = [0 for _ in functions]
    spill_costs = [0 for _ in functions]
    total = sum(hits)
    attributed = 0
    opcodes = Counter()
    hot = []
    for pc, times in enumerate(hits):
        if not times:
            continue
        op, _, a, b, c = struct.unpack_from('<BBHHH', raw, 15 + pc * 8)
        opname = OPCODES.get(op, str(op))
        opcodes[opname] += times
        index = bisect_right(starts, pc) - 1
        function = None
        if index >= 0 and pc < functions[index]['end']:
            function = functions[index]['name']
            attributed += times
            counts[index][opname] += times
            if op in (17, 18) and b == 28 and c == 27:
                spills[index] += times
                cost = times
                if pc >= 2:
                    first = struct.unpack_from('<BBHHH', raw, 15 + (pc - 2) * 8)
                    second = struct.unpack_from('<BBHHH', raw, 15 + (pc - 1) * 8)
                    if first[0] == 1 and first[2:] == (27, 30, 0) and second[0] == 10 and second[2:4] == (27, 27):
                        cost += hits[pc - 2] + hits[pc - 1]
                spill_costs[index] += cost
        hot.append({'pc': pc, 'hits': times, 'instruction': [opname, a, b, c], 'function': function})
    rows = []
    for index, function in enumerate(functions):
        instructions = sum(counts[index].values())
        if instructions:
            rows.append(dict(function, instructions=instructions, percent=instructions * 100 / total,
                             spill_load_store=spills[index], spill_with_address_setup=spill_costs[index],
                             opcodes=dict(counts[index].most_common())))
    rows.sort(key=lambda row: row['instructions'], reverse=True)
    return {'binary_sha256': hashlib.sha256(raw).hexdigest(), 'instruction_count': count,
            'bank_size': bank, 'entry': entry, 'total': total, 'attributed': attributed,
            'unattributed': total - attributed, 'functions': rows,
            'spill_load_store': sum(spills), 'spill_with_address_setup': sum(spill_costs),
            'opcodes': dict(opcodes.most_common()),
            'hot_instructions': sorted(hot, key=lambda row: row['hits'], reverse=True)[:100]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('hits', type=Path)
    parser.add_argument('objects', type=Path)
    parser.add_argument('--archive', action='append', default=[], type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.binary, args.hits, args.objects, args.archive)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"{result['total']:,} instructions, {result['unattributed']:,} unattributed")
    for row in result['functions'][:20]:
        print(f"{row['percent']:6.2f}% {row['instructions']:12,d} {row['name']}")


if __name__ == '__main__':
    main()
