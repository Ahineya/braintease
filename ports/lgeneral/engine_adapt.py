"""Checked LGeneral 1.4.4 parser and engine portability edits.

Run on a pristine source tree before the build's C16 integer-width pass. These
changes also run in the native reference profile so both exercise identical
parser bounds and temporary-buffer lifetimes.
"""
from pathlib import Path


def _replace(text: str, old: str, new: str, path: Path) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"{path}: expected one engine-adapter match: {old[:90]!r}")
    return text.replace(old, new, 1)


def adapt(source_root: Path, c16: bool) -> None:
    path = source_root / 'src/parser.h'
    text = path.read_text()
    text = _replace(text, 'int lineno:24;', 'long lineno;', path)
    path.write_text(text)

    path = source_root / 'src/parser.c'
    text = path.read_text()
    text = _replace(text, '    free(ctd->filename);\n}',
                    '    free(ctd->filename);\n    free(ctd);\n}', path)
    text = _replace(text,
        '''\t    const unsigned buflen = next_slash - string;
\t    /* don't bust the stack. call alloca only on long strings */
\t    char *buffer = buflen < sizeof small_buf ? small_buf : alloca(buflen + 1);
            memcpy( buffer, string, buflen );
\t    buffer[buflen] = 0;
            list_add( list, strdup( buffer ) );''',
        '''            const size_t buflen = (size_t)(next_slash - string);
            char *buffer = buflen < sizeof small_buf ? small_buf : malloc(buflen + 1);
            if (!buffer) { list_delete(list); return 0; }
            memcpy(buffer, string, buflen);
            buffer[buflen] = 0;
            list_add(list, strdup(buffer));
            if (buffer != small_buf) free(buffer);''', path)
    text = _replace(text, '                parser_read_file_compact( st, pd );',
        '''                if (!parser_read_file_compact(st, pd)) {
                    parser_free(&pd);
                    return 0;
                }''', path)
    text = _replace(text,
        '    while ( ( line = parser_get_next_line(st) ) ) {\n        switch ( line[0] ) {',
        '    while ( ( line = parser_get_next_line(st) ) ) {\n        useNewTokens = 0;\n        switch ( line[0] ) {', path)
    text = _replace(text, '    int size;\n    char magic = 0;',
                    '    long size;\n    char magic = 0;', path)
    text = _replace(text,
        '''        /* get the whole contents -- 1 and CBUFFER_SIZE are switched */
        fseek( st->file, 0, SEEK_END ); size = ftell( st->file ) - 2;
        if ( size >= CBUFFER_SIZE ) {
            fprintf( stderr, tr("%s: file's too big to fit the compact buffer (%dKB)\\n"), fname, CBUFFER_SIZE / 1024 );
            size = CBUFFER_SIZE - 1;
        }
        fseek( st->file, 2, SEEK_SET );
        _fread( cbuffer, 1, size, st->file );
        cbuffer[size] = 0;''',
        '''        /* Compact resources are octet streams; unit databases exceed 64 KiB.
         * Reject an incomplete resource instead of parsing a truncated tree. */
        if (fseek(st->file, 0, SEEK_END) != 0 ||
            (size = ftell(st->file)) < 2) {
            parser_set_parse_error(st, "invalid compact resource length");
            goto failure;
        }
        size -= 2;
        if ((unsigned long)size >= (unsigned long)CBUFFER_SIZE) {
            parser_set_parse_error(st, "compact resource exceeds 256 KiB buffer");
            goto failure;
        }
        if (fseek(st->file, 2, SEEK_SET) != 0 ||
            fread(cbuffer, 1, (size_t)size, st->file) != (size_t)size) {
            parser_set_parse_error(st, "incomplete compact resource");
            goto failure;
        }
        cbuffer[(size_t)size] = 0;''', path)
    path.write_text(text)

    path = source_root / 'src/map.c'
    text = path.read_text()
    text = _replace(text, 'static List *deploy_fields;',
        '''static List *deploy_fields;
/* AI danger-mask scratch follows the map lifetime, not the C stack. */
static short *danger_dist_scratch;
static MapCoord *danger_airfields_scratch;
static size_t danger_scratch_count;''', path)
    text = _replace(text, '    map = 0; mask = 0;\n    map_w = map_h = 0;',
        '''    map = 0; mask = 0;
    free(danger_dist_scratch); danger_dist_scratch = 0;
    free(danger_airfields_scratch); danger_airfields_scratch = 0;
    danger_scratch_count = 0;
    map_w = map_h = 0;''', path)
    text = _replace(text,
        '''    short *dist_air_mask = alloca(map_w*map_h*sizeof dist_air_mask[0]);
    MapCoord *airfields = alloca(map_w*map_h*sizeof airfields[0]);
    int airfield_count = map_write_friendly_depot_list( unit, airfields );''',
        '''    size_t cells;
    short *dist_air_mask;
    MapCoord *airfields;
    int airfield_count;
    if (map_w <= 0 || map_h <= 0 ||
        (size_t)map_w > (size_t)-1 / (size_t)map_h) return 0;
    cells = (size_t)map_w * (size_t)map_h;
    if (cells > (size_t)-1 / sizeof(MapCoord)) return 0;
    if (cells > danger_scratch_count) {
        short *new_dist = malloc(cells * sizeof(*new_dist));
        MapCoord *new_airfields = malloc(cells * sizeof(*new_airfields));
        if (!new_dist || !new_airfields) {
            free(new_dist); free(new_airfields);
            fprintf(stderr, "LGeneral: cannot allocate AI danger-mask scratch\\n");
            return 0;
        }
        free(danger_dist_scratch); free(danger_airfields_scratch);
        danger_dist_scratch = new_dist;
        danger_airfields_scratch = new_airfields;
        danger_scratch_count = cells;
    }
    dist_air_mask = danger_dist_scratch;
    airfields = danger_airfields_scratch;
    airfield_count = map_write_friendly_depot_list(unit, airfields);''', path)
    path.write_text(text)

    path = source_root / 'src/engine.c'
    text = path.read_text()
    text = _replace(text,
        '''        char *txt = alloca((camp_cur_scen->title ? strlen(camp_cur_scen->title) + 2 : 0)
                           + (last_debriefing ? strlen(last_debriefing) + 1 : 0)
                           + (camp_cur_scen->brief ? strlen(camp_cur_scen->brief) : 0) + 2 + 1);
        strcpy(txt, "");''',
        '''        char *txt = malloc((camp_cur_scen->title ? strlen(camp_cur_scen->title) + 2 : 0)
                           + (last_debriefing ? strlen(last_debriefing) + 1 : 0)
                           + (camp_cur_scen->brief ? strlen(camp_cur_scen->brief) : 0) + 2 + 1);
        if (!txt) {
            fprintf(stderr, "LGeneral: cannot allocate campaign briefing\\n");
            return;
        }
        strcpy(txt, "");''', path)
    text = _replace(text, 'memset( merge_units, 0, sizeof( int ) * MAP_MERGE_UNIT_LIMIT );',
                    'memset(merge_units, 0, sizeof merge_units);', path)
    text = _replace(text, '        gui_set_message_pane_text(camp_pane, txt);',
                    '        gui_set_message_pane_text(camp_pane, txt);\n        free(txt);', path)
    text = _replace(text, 'int modify_fog = 0;',
        '#ifdef LGENERAL_ENGINE_TEST\nstatic int lgeneral_testing;\n#endif\nint modify_fog = 0;', path)
    for function in ('engine_show_turn_info', 'engine_show_final_message'):
        marker = 'static void ' + function + '()\n{'
        text = _replace(text, marker,
            marker + '\n#ifdef LGENERAL_ENGINE_TEST\n    if (lgeneral_testing) return;\n#endif', path)
    # Test code uses the real action engine's static functions. Production builds
    # do not include it, and the public game API remains unchanged.
    text += '\n#ifdef LGENERAL_ENGINE_TEST\n#include "engine-integration.inc"\n#endif\n'
    path.write_text(text)
    import shutil
    shutil.copyfile(Path(__file__).parent / 'tests/engine-integration.inc', source_root / 'src/engine-integration.inc')

    # Saved unit properties retain custom/merged statistics, while their AI
    # evaluation cache must use the same domain scales as the loaded library.
    path = source_root / 'src/unit_lib.h'
    text = path.read_text()
    text = _replace(text, 'void unit_lib_eval_unit( Unit_Lib_Entry *unit );',
        'void unit_lib_eval_unit( Unit_Lib_Entry *unit );\n'
        'void unit_lib_eval_unit_normalized(Unit_Lib_Entry *unit);', path)
    path.write_text(text)
    path = source_root / 'src/unit_lib.c'
    text = path.read_text()
    text = _replace(text, 'List *unit_lib = 0;',
        'List *unit_lib = 0;\n'
        'static int eval_best_ground, eval_best_air, eval_best_sea;', path)
    text = _replace(text,
        '    unit->eval_score = ( 2 * attack + 2 * defense + misc ) / 5;\n}',
        '''    unit->eval_score = ( 2 * attack + 2 * defense + misc ) / 5;
}

/* Rebuild the cache after loading saved (possibly merged) unit properties. */
void unit_lib_eval_unit_normalized(Unit_Lib_Entry *unit)
{
    int best;
    unit_lib_eval_unit(unit);
    best = (unit->flags & FLYING) ? eval_best_air :
           (unit->flags & SWIMMING) ? eval_best_sea : eval_best_ground;
    if (best > 0) unit->eval_score = unit->eval_score * 1000L / best;
}''', path)
    text = _replace(text,
        '    list_reset( unit_lib );\n'
        '    while ( ( unit = list_next( unit_lib ) ) ) {\n'
        '        unit->eval_score *= 1000;',
        '    eval_best_ground = best_ground;\n'
        '    eval_best_air = best_air;\n'
        '    eval_best_sea = best_sea;\n'
        '    list_reset( unit_lib );\n'
        '    while ( ( unit = list_next( unit_lib ) ) ) {\n'
        '        unit->eval_score *= 1000;', path)
    text = _replace(text, 'void unit_lib_delete( void )\n{\n    int i;',
        'void unit_lib_delete( void )\n{\n    int i;\n'
        '    eval_best_ground = eval_best_air = eval_best_sea = 0;', path)
    path.write_text(text)

    if c16:
        path = source_root / 'util/hashtable.c'

        text = path.read_text()
        text = _replace(text, '(1u << 30)', '(1UL << 30)', path)
        path.write_text(text)
