"""Canonical save records for LGeneral's C16 and future octet-native builds.

Upstream serializes native ``int`` and pointer widths, including obsolete
pointer values. The port writes logical octets explicitly; LGC1 is deliberately
distinct from those ambiguous native-layout save files.
"""
from pathlib import Path
import re


CODEC = r'''
/* LGC1: all lengths/scalars are signed little-endian 32-bit values. A C16
 * char is an octet carrier, not a packed host byte. Reserved pointers are four
 * zero octets. No native object representation is written to the file. */
static int lg_slot_error;
#define LG_SAVE_STRING_LIMIT 65535L
#define LG_SAVE_RECORD_LIMIT 262144L

static long try_load_int(FILE *file, long deflt)
{
    unsigned char octets[4];
    unsigned long value;
    if (fread(octets, 1, 4, file) != 4) {
        lg_slot_error = 1;
        return deflt;
    }
    value = (unsigned long)(octets[0] & 255)
          | ((unsigned long)(octets[1] & 255) << 8)
          | ((unsigned long)(octets[2] & 255) << 16)
          | ((unsigned long)(octets[3] & 255) << 24);
    if (value & 0x80000000UL)
        return -1L - (long)(0xffffffffUL - value);
    return (long)value;
}

static void save_int(FILE *file, long value)
{
    unsigned char octets[4];
    unsigned long bits = (unsigned long)value;
    if (value < (-2147483647L - 1L) || value > 2147483647L) {
        lg_slot_error = 1;
        return;
    }
    octets[0] = bits & 255;
    octets[1] = (bits >> 8) & 255;
    octets[2] = (bits >> 16) & 255;
    octets[3] = (bits >> 24) & 255;
    if (fwrite(octets, 1, 4, file) != 4) lg_slot_error = 1;
}

static long load_int(FILE *file) { return try_load_int(file, 0); }
static void save_pointer(FILE *file, const void *unused)
{
    (void)unused;
    save_int(file, 0);
}
static void *load_pointer(FILE *file)
{
    if (load_int(file) != 0) lg_slot_error = 1;
    return 0;
}

static void slot_write_fixed(FILE *file, const char *text, long width)
{
    char octets[256];
    long i;
    int ended = 0;
    if (width < 1 || width > 256) { lg_slot_error = 1; return; }
    for (i = 0; i < width; ++i) {
        /* Always terminate the field and zero its unused/padding cells. */
        if (!text || ended || i == width - 1) octets[i] = 0;
        else {
            octets[i] = text[i] & 255;
            if (!octets[i]) ended = 1;
        }
    }
    if (fwrite(octets, 1, width, file) != (size_t)width)
        lg_slot_error = 1;
}

static void slot_read_fixed(FILE *file, char *text, long width)
{
    if (fread(text, 1, width, file) != (size_t)width) {
        lg_slot_error = 1;
        text[0] = 0;
    }
    text[width - 1] = 0;
}

static void save_string(FILE *file, char *str)
{
    size_t length = str ? strlen(str) : 0;
    if (length > LG_SAVE_STRING_LIMIT) { lg_slot_error = 1; return; }
    save_int(file, (long)length);
    if (length && fwrite(str, 1, length, file) != length)
        lg_slot_error = 1;
}

static char *load_string(FILE *file)
{
    long length = load_int(file);
    char *str;
    if (lg_slot_error || length < 0 || length > LG_SAVE_STRING_LIMIT) {
        lg_slot_error = 1;
        return 0;
    }
    str = calloc((size_t)length + 1, sizeof(char));
    if (!str) { lg_slot_error = 1; return 0; }
    if (length && fread(str, 1, length, file) != (size_t)length) {
        lg_slot_error = 1;
        free(str);
        return 0;
    }
    str[length] = 0;
    return str;
}

static int slot_skip_bytes(FILE *file, long count)
{
    unsigned char scratch[256];
    while (count > 0 && !lg_slot_error) {
        long part = count > 256 ? 256 : count;
        if (fread(scratch, 1, part, file) != (size_t)part)
            lg_slot_error = 1;
        count -= part;
    }
    return !lg_slot_error;
}

static int slot_scan_string(FILE *file)
{
    long count = load_int(file);
    if (count < 0 || count > LG_SAVE_STRING_LIMIT) lg_slot_error = 1;
    return !lg_slot_error && slot_skip_bytes(file, count);
}

static long slot_scan_count(FILE *file)
{
    long count = load_int(file);
    if (count < 0 || count > LG_SAVE_RECORD_LIMIT) lg_slot_error = 1;
    return count;
}

static int slot_scan_unit_lib(FILE *file)
{
    /* 27 scalar fields, TARGET_TYPE_LIMIT attacks, five reserved pointers. */
    return slot_skip_bytes(file, (27L + TARGET_TYPE_LIMIT + 5L) * 4L);
}

static int slot_scan_unit(FILE *file)
{
    long i;
    slot_scan_unit_lib(file);
    slot_scan_unit_lib(file);
    slot_skip_bytes(file, 5L * 4L + 24L + 32L + 25L * 4L);
    for (i = 0; i < 4 && !lg_slot_error; ++i) slot_scan_string(file);
    return !lg_slot_error;
}

static int slot_read_magic(FILE *file)
{
    unsigned char magic[4];
    return fread(magic, 1, 4, file) == 4
        && magic[0] == 'L' && magic[1] == 'G'
        && magic[2] == 'C' && magic[3] == '1';
}

static void slot_write_magic(FILE *file)
{
    unsigned char magic[4] = { 'L', 'G', 'C', '1' };
    if (fwrite(magic, 1, 4, file) != 4) lg_slot_error = 1;
}

/* Read the whole record layout before changing the current scenario. This
 * catches truncated saves, oversized lengths/counts and invalid map indices.
 * Counts in an upstream native-layout file cannot be interpreted safely. */
static int slot_preflight(FILE *file)
{
    long campaign, count, i, j, group, player_count, current_player;
    long unit_count = 0, map_width, map_height, cells, index;
    lg_slot_error = 0;
    if (fseek(file, 256, SEEK_SET) != 0 || !slot_read_magic(file)) return 0;
    if (load_int(file) != StoreCanonicalOctets) return 0;
    campaign = load_int(file);
    if (campaign != 0 && campaign != 1) return 0;
    if (campaign) {
        slot_scan_string(file);
        slot_scan_string(file);
        count = slot_scan_count(file);
        if (count > 0) slot_scan_string(file);
        for (i = 0; i < count && !lg_slot_error; ++i) {
            slot_scan_unit_lib(file);
            slot_scan_unit_lib(file);
            for (j = 0; j < 5 && !lg_slot_error; ++j) slot_scan_string(file);
            slot_skip_bytes(file, 8);
            slot_scan_string(file);
        }
    }
    slot_scan_string(file); /* scenario filename */
    slot_skip_bytes(file, 6L * 4L);
    current_player = load_int(file);
    player_count = slot_scan_count(file);
    if (player_count < 1 || current_player < 0 || current_player >= player_count)
        return 0;
    for (i = 0; i < player_count && !lg_slot_error; ++i) {
        slot_skip_bytes(file, 4L * 4L);
        slot_scan_string(file);
    }
    for (group = 0; group < 3 && !lg_slot_error; ++group) {
        count = slot_scan_count(file);
        if (!group) unit_count = count;
        for (i = 0; i < count && !lg_slot_error; ++i) slot_scan_unit(file);
    }
    map_width = load_int(file);
    map_height = load_int(file);
    if (map_width < 1 || map_width > 4096 || map_height < 1 || map_height > 4096)
        return 0;
    cells = map_width * map_height;
    if (cells > 1048576L) return 0;
    for (i = 0; i < cells * 2 && !lg_slot_error; ++i) {
        index = load_int(file);
        if (index < -1 || index >= unit_count) lg_slot_error = 1;
    }
    for (i = 0; i < cells && !lg_slot_error; ++i) {
        slot_scan_string(file);
        slot_scan_string(file);
        slot_skip_bytes(file, 8);
    }
    if (lg_slot_error || fgetc(file) != EOF || ferror(file)) return 0;
    return fseek(file, 0, SEEK_SET) == 0;
}
'''


def _once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError(f"LGeneral save source changed: {before[:80]!r}")
    return text.replace(before, after, 1)


def adapt(source_root: Path, c16: bool) -> None:
    """Run before the common game-source integer widening/format adaptation."""
    path = source_root / "src" / "slot.c"
    text = path.read_text()
    text = _once(text, "    /* insert new versions before this comment */",
                 "    StoreCanonicalOctets, /* portable LGC1 records */\n"
                 "    /* insert new versions before this comment */")
    start = text.index("/*\n====================================================================\nload a single integer with fallback")
    end = text.index("/*\n====================================================================\nSave/load a unit from/to file.", start)
    text = text[:start] + CODEC + "\n" + text[end:]
    # The name helpers precede the codec, so provide declarations there.
    text = _once(text, "static void slot_read_name( Slot *slot, FILE *file )",
                 "static void slot_read_fixed(FILE *, char *, long);\n"
                 "static void slot_write_fixed(FILE *, const char *, long);\n"
                 "static void slot_read_name( Slot *slot, FILE *file )")
    text = _once(text, "    _fread( slot->name, sizeof( slot->name ), 1, file );",
                 "    slot_read_fixed(file, slot->name, 256);")
    text = _once(text, "    fwrite( slot->name, sizeof( slot->name ), 1, file );",
                 "    slot_write_fixed(file, slot->name, 256);")
    # The file format no longer depends on native struct padding or pointers.
    text = re.sub(r"enum UnitStoreParams \{.*?\n\};", "", text, count=1, flags=re.S)
    text = re.sub(r"    COMPILE_TIME_ASSERT\(sizeof\(Unit(?:_Lib_Entry)?\) == Unit(?:Lib)?Size\);\n", "", text)
    text = _once(text, "    fwrite(unit->name, UnitNameSize, 1, file);",
                 "    slot_write_fixed(file, unit->name, 24);")
    text = _once(text, "    fwrite(unit->tag, UnitTagSize, 1, file);",
                 "    slot_write_fixed(file, unit->tag, 32);")
    # These upstream fields are explicitly reserved, ignored on reads, or
    # recalculated from current properties. Their transient values must not
    # make identical gameplay states produce different canonical records.
    for expression in ("entry->eval_score", "unit->icon_offset", "unit->icon_tiny_offset",
                       "unit->damage_bar_width", "unit->fresh_deploy",
                       "unit->eval_score", "unit->target_score"):
        text = _once(text, f"    save_int(file, {expression});",
                     "    save_int(file, 0); /* reserved scalar */")
    text = _once(text, "    unit_lib_eval_unit( entry );",
                 "    unit_lib_eval_unit_normalized(entry);")
    text = _once(text, "    _fread(unit->name, UnitNameSize, 1, file);\n    unit->name[UnitNameSize - 1] = 0;",
                 "    slot_read_fixed(file, unit->name, 24);")
    text = _once(text, "    _fread(unit->tag, UnitTagSize, 1, file);\n    unit->tag[UnitTagSize - 1] = 0;",
                 "    slot_read_fixed(file, unit->tag, 32);")
    text = _once(text, "    return *i < SLOT_COUNT;", "    return *i >= 0 && *i < SLOT_COUNT;")
    text = text.replace("    if ( id >= SLOT_COUNT ) id = 0;", "    if ( id < 0 || id >= SLOT_COUNT ) id = 0;")
    # Both public entry points validate slot ids and reset the codec state.
    text = text.replace("    /* get file name */\n    snprintf( path, 1024, \"%s/lg_save_%i\", config.dir_name, id );",
                        "    if (id < 0 || id >= SLOT_COUNT) return 0;\n"
                        "    lg_slot_error = 0;\n    /* get file name */\n"
                        "    snprintf( path, 1024, \"%s/lg_save_%i\", config.dir_name, id );")
    # The previous slot remains intact until a complete temporary file closes
    # successfully and is atomically renamed over it. The temporary component
    # (lg_save_N.tmp) stays below the guest filesystem's 30-character limit.
    text = _once(text, "int slot_save( int id, char *name )\n{\n    FILE *file = 0;\n    char path[1024];",
                 "int slot_save( int id, char *name )\n{\n    FILE *file = 0;\n"
                 "    char path[1024], temp_path[1024], pending_name[256];")
    text = _once(text, "    /* open file */\n    if ( ( file = fopen( path, \"wb\" ) ) == 0 ) {",
                 "    if (snprintf(temp_path, sizeof(temp_path), \"%s/lg_save_%i.tmp\", config.dir_name, id) >= (int)sizeof(temp_path)) return 0;\n"
                 "    /* Write beside the final slot so rename stays on its filesystem. */\n"
                 "    if ( ( file = fopen( temp_path, \"wb\" ) ) == 0 ) {")
    text = _once(text, "    strcpy( slots[id].name, name );",
                 "    snprintf(pending_name, sizeof(pending_name), \"%s\", name);")
    text = _once(text, "    slot_write_name( &slots[id], file );",
                 "    slot_write_fixed(file, pending_name, 256);")
    text = _once(text, "    /* write version */\n    save_int(file, StoreHighestSupportedVersion);",
                 "    slot_write_magic(file);\n    /* write version */\n"
                 "    save_int(file, StoreCanonicalOctets);")
    text = _once(text, "    /* players */\n    list_reset( players );\n    for ( i = 0; i < players->count; i++ )\n        save_player",
                 "    /* players: explicit count for canonical preflight validation */\n"
                 "    save_int(file, players->count);\n    list_reset( players );\n"
                 "    for ( i = 0; i < players->count; i++ )\n        save_player")
    text = _once(text, "    fclose( file );\n    /* is valid now */",
                 "    if (ferror(file)) lg_slot_error = 1;\n"
                 "    if (fclose(file) != 0) lg_slot_error = 1;\n"
                 "    if (lg_slot_error) { remove(temp_path); return 0; }\n"
                 "    if (rename(temp_path, path) != 0) { remove(temp_path); return 0; }\n"
                 "    snprintf(slots[id].name, sizeof(slots[id].name), \"%s\", pending_name);\n"
                 "    snprintf(slots[id].fname, sizeof(slots[id].fname), \"%s\", path);\n"
                 "    /* is valid now */")
    text = _once(text, "    /* read slot identification -- won't change anything but the file handle needs to move */",
                 "    if (!slot_preflight(file)) {\n"
                 "        fprintf(stderr, \"%s: invalid or unsupported save (expected portable LGC1)\\n\", path);\n"
                 "        fclose(file);\n        return 0;\n    }\n"
                 "    /* read slot identification -- won't change anything but the file handle needs to move */")
    start = text.index("    /* read version */", text.index("int slot_load("))
    end = text.index("    /* reject game if it's too new */", start)
    text = text[:start] + "    slot_read_magic(file);\n    store_version = load_int(file);\n    endian_need_swap = 0;\n\n" + text[end:]
    text = _once(text, "    /* players */\n    list_reset( players );\n    for ( i = 0; i < players->count; i++ )\n        load_player",
                 "    if (load_int(file) != players->count) { fclose(file); return 0; }\n"
                 "    /* players */\n    list_reset( players );\n"
                 "    for ( i = 0; i < players->count; i++ )\n        load_player")
    text = _once(text, "        camp_load( str );\n        free( str );",
                 "        if (!camp_load(str)) { free(str); fclose(file); return 0; }\n        free(str);")
    text = _once(text, "        camp_set_cur( str );\n        free( str );",
                 "        camp_set_cur(str);\n        free(str);\n"
                 "        if (!camp_cur_scen) { fclose(file); return 0; }")
    # Reject stale save coordinates/dimensions before indexing the loaded map.
    text = _once(text, "    unit->terrain = map[unit->x][unit->y].terrain;",
                 "    if (unit->x == -1 && unit->y == -1) unit->terrain = 0;\n"
                 "    else if (unit->x < 0 || unit->x >= map_w || unit->y < 0 || unit->y >= map_h) {\n"
                 "        lg_slot_error = 1;\n        free(unit->backup);\n        free(unit);\n        return 0;\n    }\n"
                 "    else unit->terrain = map[unit->x][unit->y].terrain;")
    text = _once(text, "    unit->prop.id = lib_entry->id;",
                 "    if (!lib_entry) { lg_slot_error = 1; free(str); free(unit->backup); free(unit); return 0; }\n"
                 "    unit->prop.id = lib_entry->id;")
    text = _once(text, "    unit->nation = nation_find( str );\n    free( str );",
                 "    unit->nation = nation_find(str);\n    free(str);\n"
                 "    if (!unit->player || !unit->nation) { lg_slot_error = 1; free(unit->backup); free(unit); return 0; }")
    for name in ("units", "reinf", "avail_units"):
        text = _once(text, f"    for ( i = 0; i < unit_count; i++ )\n        list_add( {name}, load_unit( file ) );",
                     f"    for ( i = 0; i < unit_count; i++ ) {{\n"
                     "        Unit *loaded_unit = load_unit(file);\n"
                     "        if (!loaded_unit || lg_slot_error) { fclose(file); return 0; }\n"
                     f"        list_add({name}, loaded_unit);\n    }}")
    text = _once(text, "    map_w = load_int( file );\n    map_h = load_int( file );",
                 "    i = load_int(file);\n    j = load_int(file);\n"
                 "    if (i != map_w || j != map_h) { fclose(file); return 0; }")
    text = _once(text, "    fclose( file );\n    return 1;\n}\n\n/*\n====================================================================\nReturn True if slot is loadable.",
                 "    fclose(file);\n    return !lg_slot_error;\n}\n\n/*\n====================================================================\nReturn True if slot is loadable.")
    text = _once(text, "    return slots[id].valid;", "    return id >= 0 && id < SLOT_COUNT && slots[id].valid;")
    path.write_text(text)
