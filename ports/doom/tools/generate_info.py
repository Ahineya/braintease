#!/usr/bin/env python3
"""Move Doom's canonical state/object initializers into bounded heap fills.

Usage: generate_info.py upstream/doomgeneric/info.c build/generated
Compile info_fill_*.c, info_init.c and upstream info.c with
DOOM_GENERATED_INFO=1, then call Doom_InitInfo before starting the engine.
Initializer expressions are preserved verbatim; no serialized layout is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

STATE_FIELDS = ('sprite', 'frame', 'tics', 'action.acv', 'nextstate', 'misc1', 'misc2')
MOBJ_FIELDS = ('doomednum', 'spawnstate', 'spawnhealth', 'seestate', 'seesound',
               'reactiontime', 'attacksound', 'painstate', 'painchance', 'painsound',
               'meleestate', 'missilestate', 'deathstate', 'xdeathstate', 'deathsound',
               'speed', 'radius', 'height', 'mass', 'damage', 'activesound', 'flags',
               'raisestate')
TABLES = (('states', 'state_t', 'NUMSTATES', 967, STATE_FIELDS, 16),
          ('mobjinfo', 'mobjinfo_t', 'NUMMOBJTYPES', 137, MOBJ_FIELDS, 4))
FUNCTIONS_PER_FILE = 4
BANNER = '''/* Generated from canonical Doom info.c by generate_info.py.
 * Original data: id Software / Simon Howard, GPL-2.0-or-later.
 * See upstream COPYING/LICENSE and info.c copyright notice.
 * Do not edit; regenerate from the pinned upstream source. */
#include <stddef.h>
#include "info.h"
#include "sounds.h"
#include "m_fixed.h"
#include "p_mobj.h"
#include "info_actions.h"

'''


def split_items(text: str) -> list[str]:
    """Split only top-level commas; union initializer braces remain together."""
    depth = 0
    start = 0
    parts = []
    for i, char in enumerate(text):
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth < 0:
                raise ValueError('unbalanced initializer braces')
        elif char == ',' and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    if depth:
        raise ValueError('unbalanced initializer braces')
    if text[start:].strip():
        parts.append(text[start:].strip())
    return parts


def read_records(source: str, name: str, count: int, fields: tuple) -> list[list[str]]:
    cleaned = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.DOTALL)
    found = re.search(rf'\b\w+\s+{name}\s*\[[^]]+\]\s*=\s*\{{(.*?)\}}\s*;', cleaned, re.DOTALL)
    if not found:
        raise ValueError(f'cannot locate canonical {name} initializer')
    records = []
    for record in split_items(found.group(1)):
        if not record.startswith('{') or not record.endswith('}'):
            raise ValueError(f'{name}: expected a record initializer')
        values = split_items(record[1:-1])
        if len(values) != len(fields):
            raise ValueError(f'{name}: expected {len(fields)} fields, found {len(values)}')
        if name == 'states':
            if not re.fullmatch(r'\{\s*(NULL|A_\w+)\s*\}', values[3]):
                raise ValueError('unsupported state action union initializer')
            values[3] = values[3][1:-1].strip()
        records.append(values)
    if len(records) != count:
        raise ValueError(f'{name}: expected {count} records, found {len(records)}')
    return records


def generate(source_path: Path, output: Path) -> dict:
    source_bytes = source_path.read_bytes()
    source = source_bytes.decode('utf-8')
    output.mkdir(parents=True, exist_ok=True)
    functions, prototypes = [], []
    calls = {}
    manifest = {'format_version': 1, 'source_sha256': hashlib.sha256(source_bytes).hexdigest(),
                'tables': {}, 'sources': []}
    callbacks = set()
    for name, ctype, count_symbol, count, fields, chunk in TABLES:
        records = read_records(source, name, count, fields)
        if name == 'states':
            callbacks.update(record[3] for record in records if record[3] != 'NULL')
        normalized = json.dumps(records, separators=(',', ':')).encode()
        manifest['tables'][name] = {'entries': count, 'fields': list(fields),
                                   'chunk_records': chunk,
                                   'expressions_sha256': hashlib.sha256(normalized).hexdigest()}
        calls[name] = []
        for index in range(0, count, chunk):
            symbol = f'Doom_Fill_{name}_{index // chunk:03d}'
            prototypes.append(f'{ctype} *{symbol}({ctype} *out);')
            body = [f'{ctype} *{symbol}({ctype} *out)', '{']
            for record in records[index:index + chunk]:
                body.extend(f'    out->{field} = {value};' for field, value in zip(fields, record))
                body.append('    ++out;')
            body.extend(['    return out;', '}', ''])
            functions.append('\n'.join(body))
            calls[name].append(f'    cursor = {symbol}(cursor);')
    callbacks = sorted(callbacks)
    headers = {'info_actions.h': '#ifndef DOOM_INFO_ACTIONS_H\n#define DOOM_INFO_ACTIONS_H\n'
               + ''.join(f'void {name}();\n' for name in callbacks) + '#endif\n'}
    headers['info_test_actions.h'] = ('/* Test-only stable callback identities. */\n'
                                    '#define DOOM_INFO_ACTIONS(X) \\\n'
                                    + ' \\\n'.join(f'    X({i + 1}, {name})' for i, name in enumerate(callbacks)) + '\n')
    generated = {}
    for index in range(0, len(functions), FUNCTIONS_PER_FILE):
        generated[f'info_fill_{index // FUNCTIONS_PER_FILE:03d}.c'] = BANNER + '\n'.join(functions[index:index + FUNCTIONS_PER_FILE])
    driver = [BANNER, '#include <stdlib.h>\n', *prototypes, '',
              'state_t *states;', 'mobjinfo_t *mobjinfo;', '']
    for name, ctype, _, _, _, _ in TABLES:
        driver.extend([f'static void Init_{name}(void)', '{', f'    {ctype} *cursor = {name};',
                       *calls[name], '    (void) cursor;', '}', ''])
    driver.extend(['void Doom_FreeInfo(void)', '{', '    free(states);', '    free(mobjinfo);',
                   '    states = NULL;', '    mobjinfo = NULL;', '}', '',
                   'int Doom_InitInfo(void)', '{', '    if (states != NULL && mobjinfo != NULL) return 1;',
                   '    Doom_FreeInfo();',
                   '    states = malloc((size_t) NUMSTATES * sizeof(*states));',
                   '    mobjinfo = malloc((size_t) NUMMOBJTYPES * sizeof(*mobjinfo));',
                   '    if (states == NULL || mobjinfo == NULL)', '    {',
                   '        Doom_FreeInfo();', '        return 0;', '    }',
                   '    Init_states();', '    Init_mobjinfo();', '    return 1;', '}', ''])
    generated['info_init.c'] = '\n'.join(driver)
    manifest['sources'] = sorted(generated)
    manifest['actions'] = callbacks
    for previous in output.glob('info_fill_*.c'):
        if previous.name not in generated:
            previous.unlink()
    for name, content in {**generated, **headers}.items():
        (output / name).write_text(content)
    (output / 'info_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    manifest = generate(args.source, args.output)
    print(f"Generated {sum(t['entries'] for t in manifest['tables'].values())} info records "
          f"in {len(manifest['sources'])} bounded C files")


if __name__ == '__main__':
    main()
