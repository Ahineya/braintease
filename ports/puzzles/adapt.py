#!/usr/bin/env python3
"""Verified, out-of-tree C16 adaptations of the pinned puzzle sources."""
import hashlib
import json
from pathlib import Path
import re
import sys

PORT = Path(__file__).resolve().parent
GAMES = json.loads((PORT / 'catalog.json').read_text())
COMMON = 'midend drawing draw-poly random misc malloc dsf tree234 sort findloop'.split()
HELPERS = 'combi divvy grid latin laydomino loopgen matching tdq penrose penrose-legacy hat spectre'.split()
EXTRA = {game['id']: HELPERS for game in GAMES}


def generate(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    catalog = ('/* Generated from ports/puzzles/catalog.json. */\n'
               'struct puzzle_info { const char *id, *name, *objective, *controls; };\n'
               'static const struct puzzle_info puzzle_catalog[] = {\n' +
               ''.join('    {' + ','.join(json.dumps(g[k]) for k in ('id','name','objective','controls')) + '},\n' for g in GAMES) +
               '};\n#define PUZZLE_COUNT (sizeof(puzzle_catalog)/sizeof(puzzle_catalog[0]))\n')
    catalog_path = output / 'puzzle_catalog.h'
    if not catalog_path.exists() or catalog_path.read_text() != catalog:
        catalog_path.write_text(catalog)
    engines = ('/* Generated engine references for the regression probe. */\n' +
               ''.join('extern const game ' + g['id'] + ';\n' for g in GAMES) +
               'static const game *const puzzle_engines[] = {\n' +
               ''.join('    &' + g['id'] + ',\n' for g in GAMES) + '};\n')
    engines_path = output / 'puzzle_engines.h'
    if not engines_path.exists() or engines_path.read_text() != engines:
        engines_path.write_text(engines)
    combined = ''.join('GAME(' + g['id'] + ')\n' for g in GAMES)
    combined_path = output / 'generated-games.h'
    if not combined_path.exists() or combined_path.read_text() != combined:
        combined_path.write_text(combined)
    manifest = json.loads((PORT / 'source.json').read_text())
    for name, expected in manifest['files'].items():
        source = PORT / 'upstream' / name
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError(f'puzzles: modified upstream source: {source}')
        if source.suffix not in ('.c', '.h'):
            continue
        text = data.decode()
        # RCC currently misparses expressions using locals that shadow an
        # outer typedef. Red regression: test_puzzles_typedef_shadow_expression.
        if name in ('penrose-legacy.h', 'penrose-legacy.c'):
            text = re.sub(r'\bvector\b', 'legacy_vector', text)
        if name == 'grid.c':
            text = re.sub(r'\bvector(?=\s*\*)', 'legacy_vector', text)
        if name == 'tdq.c':
            text = re.sub(r'(?<!struct )\btdq\b(?!\s*\*)', 'queue', text)
        if name in ('grid.h', 'grid.c', 'mines.c', 'loopy.c', 'pearl.c', 'loopgen.c', 'loopgen.h'):
            text = re.sub(r'(?<!struct )\bgrid(?=\s*\*)', 'puzzle_grid', text)
            text = text.replace('} grid;', '} puzzle_grid;')
            text = text.replace('snew(grid)', 'snew(puzzle_grid)')
        if name in ('galaxies.c', 'inertia.c', 'range.c'):
            alias = {'galaxies.c': 'space', 'inertia.c': 'soln', 'range.c': 'move'}[name]
            replacement = 'puzzle_' + alias
            text = re.sub(r'(?<!struct )\b' + alias + r'(?=\s*\*)', replacement, text)
            text = text.replace('} ' + alias + ';', '} ' + replacement + ';')
            text = text.replace('snew(' + alias + ')', 'snew(' + replacement + ')')
            text = text.replace('sizeof(' + alias + ')', 'sizeof(' + replacement + ')')
            text = re.sub(r'(\bsnewn\([^\n]*,\s*)' + alias + r'\)', r'\g<1>' + replacement + ')', text)
        if name == 'guess.c':
            # Red regression: test_puzzles_pointer_typedef_tag.
            text = text.replace('struct pegrow', 'struct puzzle_pegrow')
        if name == 'palisade.c':
            # Red regression: test_puzzles_sizeof_postfix.
            text = re.sub(r'sizeof \((ptr|dst)\)\[0\]', r'sizeof ((\1)[0])', text)
            # Red regression: test_puzzles_unreachable_after_return. Keep the
            # unreachable type assertion, but terminate its dead IR block.
            anchor = 'seems silly to not put it where the assumption is used. */\n    }\n}'
            text = text.replace(anchor, anchor[:-1] + '    return NULL;\n}')
            # Nine cursor-location bits start at bit 14 in the draw cache.
            text = text.replace('typedef int dsflags;', 'typedef long dsflags;')
            # Keep BORDER/BIT as int: they are also passed to %d varargs.
            text = text.replace('#define CONTAINS_CURSOR(x) ((x) << 14)',
                                '#define CONTAINS_CURSOR(x) ((unsigned long)(x) << 14)')
            text = text.replace('int i = r * w + c, clue = state->shared->clues[i], flags, dir;',
                                'int i = r * w + c, clue = state->shared->clues[i], dir; long flags;')
        if name == 'pearl.c':
            # Red regression: test_nested_struct_tag_completion. A definition
            # nested in another struct is not completed in RCC's tag table.
            nested = re.search(r'    struct pearl_loopgen_bias_ctx_boundary \{.*?    \} boundaries\[2\];', text, re.S)
            if not nested:
                raise RuntimeError('puzzles: Pearl nested structure anchor changed')
            definition = nested[0].replace('    } boundaries[2];', '    };')
            text = text.replace(nested[0], '    struct pearl_loopgen_bias_ctx_boundary boundaries[2];')
            text = text.replace('struct pearl_loopgen_bias_ctx {', definition + '\n\nstruct pearl_loopgen_bias_ctx {')
        if name in ('blackbox.c', 'pearl.c', 'tracks.c'):
            # Packed state/drawing flags use up to 24 bits, not C16 int.
            text = re.sub(r'\bunsigned int\b', 'unsigned long', text)
            text = re.sub(r'\bunsigned(?=\s+(?!long\b|char\b|short\b)[A-Za-z_*])', 'unsigned long', text)
            text = re.sub(r'\b1\s*<<', '1UL <<', text)
            text = re.sub(r'\b(d|ad|R|U|L|D)\s*<<', r'(unsigned long)\1 <<', text)
        if name == 'dominosa.c':
            text = re.sub(r'\bint flags\b', 'long flags', text)
            text = text.replace('int x, int y, int type, int highlight_1, int highlight_2)',
                                'int x, int y, long type, int highlight_1, int highlight_2)')
        if name == 'blackbox.c':
            text = text.replace('int gs_tile = GRID(gs, gx, gy), ds_tile = GRID(ds, gx, gy);',
                                'unsigned long gs_tile = GRID(gs, gx, gy), ds_tile = GRID(ds, gx, gy);')
        if name == 'tracks.c':
            # These temporaries hold the high edge fields, not booleans.
            # Narrowing either to int makes the generator miss every edge.
            text = text.replace('int t = (f &', 'unsigned long t = (f &')
            text = text.replace('(S_E_DIRS(state, x, y, E_NOTRACK) << DS_NSHIFT)',
                                '((unsigned long)S_E_DIRS(state, x, y, E_NOTRACK) << DS_NSHIFT)')
        if name == 'solo.c':
            # Symbols 16..31 need real 32-bit candidate masks on C16.
            text = text.replace('unsigned int', 'unsigned long')
            text = re.sub(r'\b1\s*<<', '1UL <<', text)
            text = text.replace('struct gridgen_coord { int x, y, r; };', 'struct gridgen_coord { int x, y; long r; };')
            text = text.replace('int i, j, n, sx, sy, bestm, bestr;', 'int i, j, n, sx, sy, bestm; long bestr;')
            # A 16x16 board has 256 cells: area*area is 65536, not zero.
            text = text.replace('digit *grid, int *steps)', 'digit *grid, long *steps)')
            text = text.replace('random_state *rs, int maxsteps)', 'random_state *rs, long maxsteps)')
            text = text.replace('grid, rs, area*area)', 'grid, rs, (long)area*area)')
        if name == 'net.c':
            # The top drawing-state fields start at bits 17 and 19.
            text = re.sub(r'\bedgeval\s*<<', '(unsigned long)edgeval <<', text)
            text = re.sub(r'\b([123])\s*<<\s*TILE_ENDPOINT_SHIFT', r'\1UL << TILE_ENDPOINT_SHIFT', text)
            # Preserve all 29 bits through comparison, drawing and caching.
            # Narrowing curr loses endpoint/lock/animation and downward wire
            # flags; sign extension also redraws unchanged tiles on every pass.
            for variable, accessor in (('prev', 'visible'), ('curr', 'todraw')):
                anchor = f'int {variable} = {accessor}(ds, dx, dy);'
                if text.count(anchor) != 1:
                    raise RuntimeError('puzzles: Net render-state anchor changed')
                text = text.replace(anchor, 'unsigned long ' + anchor[4:])
        if name == 'draw-poly.c':
            text = text.replace('#define ONEHALF (1 <<', '#define ONEHALF (1L <<')
            text = text.replace('sizeof(int)*CHAR_BIT', 'sizeof(long)*CHAR_BIT')
            text = text.replace('((edge->x2 - edge->x1) << FRACBITS)', '((long)(edge->x2 - edge->x1) << FRACBITS)')
        if name == 'mines.c':
            # Initial port covers classic square Mines. The newer tiling
            # generators have a separate, much wider geometry/weight audit.
            text = text.replace('ret[3].u.choices.choicenames = GRID_CONFIGS;', 'ret[3].u.choices.choicenames = ":Square";')
            anchor = '    /* General constraints for all grids. */'
            if text.count(anchor) != 1:
                raise RuntimeError('puzzles: Mines validation anchor changed')
            text = text.replace(anchor, '    if (params->type != MINES_GRID_SQUARE)\n        return "This port currently supports classic square Mines boards";\n\n' + anchor)
            # The generator uses all 31 random bits as a stable sort key.
            text = text.replace('int tile, type, random;', 'int tile, type; long random;')
            # Multiply before dividing without overflowing a C16 int.
            text = text.replace('((x) - ds->xoff) * TILE_SIZE', '((long)(x) - ds->xoff) * TILE_SIZE')
            text = text.replace('((y) - ds->yoff) * TILE_SIZE', '((long)(y) - ds->yoff) * TILE_SIZE')
            text = text.replace('((x) - BORDER) * ds->grid->tilesize', '((long)(x) - BORDER) * ds->grid->tilesize')
            text = text.replace('((y) - BORDER) * ds->grid->tilesize', '((long)(y) - BORDER) * ds->grid->tilesize')
            text = text.replace('grid_width * tilesize /', '(long)grid_width * tilesize /')
            text = text.replace('grid_height * tilesize /', '(long)grid_height * tilesize /')
        if name == 'grid.h':
            text = text.replace('#define SQ(x) ( (x) * (x) )', '#define SQ(x) ( (long)(x) * (x) )')
        target = output / name
        if not target.exists() or target.read_text() != text:
            target.write_text(text)
    return output


if __name__ == '__main__':
    generate(sys.argv[1])
