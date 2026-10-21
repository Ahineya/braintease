#!/usr/bin/env python3
"""Exercise the actual generated save codec with 8- and 16-bit octet cells.

The 16-bit-cell build uses a small logical-octet stream, deliberately making a
native fwrite(&long,sizeof(long),...) fail the canonical-record assertions.
"""
from pathlib import Path
import argparse
import importlib.util
import os
import re
import subprocess
import tempfile
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("save_adapt", ROOT / "ports/lgeneral/save_adapt.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

MEMORY_STREAM = r'''
typedef unsigned short Cell;
typedef struct { Cell data[16384]; size_t at, size; int error; } CellFile;
static CellFile storage;
static size_t cell_read(void *output, size_t size, size_t count, CellFile *f)
{
    Cell *out = output;
    size_t n = size * count, i;
    if (n > f->size - f->at) n = f->size - f->at;
    for (i = 0; i < n; ++i) out[i] = f->data[f->at++];
    return size ? n / size : 0;
}
static size_t cell_write(const void *input, size_t size, size_t count, CellFile *f)
{
    const Cell *in = input;
    size_t n = size * count, i;
    if (f->at + n > 16384) { f->error = 1; return 0; }
    for (i = 0; i < n; ++i) { assert(in[i] <= 255); f->data[f->at++] = in[i]; }
    if (f->at > f->size) f->size = f->at;
    return count;
}
static int cell_seek(CellFile *f, long offset, int whence)
{
    long at = offset;
    if (whence == SEEK_CUR) at += f->at;
    if (whence == SEEK_END) at += f->size;
    if (at < 0 || at > (long)f->size) return -1;
    f->at = at;
    return 0;
}
static int cell_getc(CellFile *f) { return f->at < f->size ? f->data[f->at++] : EOF; }
static size_t cell_strlen(const Cell *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int cell_error(CellFile *f) { return f->error; }
#define FILE CellFile
#define fread cell_read
#define fwrite cell_write
#define fseek cell_seek
#define fgetc cell_getc
#define ferror cell_error
#define strlen cell_strlen
'''

TESTS = r'''
static unsigned long checks;
static void check(int value) { ++checks; assert(value); }
static void zeros(FILE *f, long count)
{
    long i;
    for (i = 0; i < count; ++i) save_int(f, 0);
}
static void fixture(FILE *f, long campaign)
{
    char title[] = { 'P','o','r','t',0 };
    char scene[] = { 's','e','r','b','i','a',0 };
    char id[] = { 'u','n','i','t',0 };
    long i;
    slot_write_fixed(f, title, 256);
    slot_write_magic(f);
    save_int(f, StoreCanonicalOctets);
    save_int(f, campaign);
    if (campaign) {
        save_string(f, id); /* campaign name */
        save_string(f, id); /* campaign scenario id */
        save_int(f, 1); /* previous scenario core unit */
        save_string(f, scene);
        write_transfer_fixture(f);
    }
    save_string(f, scene);
    zeros(f, 6); /* five settings and current turn */
    save_int(f, 0); /* current player */
    save_int(f, 2);
    for (i = 0; i < 2; ++i) { zeros(f, 4); save_string(f, id); }
    save_int(f, 1); /* active units */
    write_unit_fixture(f);
    save_int(f, 0); /* reinforcements */
    save_int(f, 0); /* available */
    save_int(f, 2); save_int(f, 1); /* map */
    save_int(f, 0); save_int(f, -1); save_int(f, -1); save_int(f, -1);
    for (i = 0; i < 2; ++i) { save_string(f, id); save_string(f, id); zeros(f, 2); }
}

static void codec_tests(FILE *f)
{
    static long values[] = { 0, 1, -1, 32767, 32768, -32769,
                            65535, 151395, 2147483647L, (-2147483647L - 1L) };
    unsigned char octets[4];
    char text[] = { 0xd0, 0x96, 0xe2, 0x98, 0x83, 0 };
    char field[8];
    char *loaded;
    long i;
    static long reserved_offsets[] = {
        0,4,108,112,160,164, /* property pointer reservations and eval score */
        168,172,276,280,328,332, /* transporter property reservations */
        336,340,368,372,376, /* unit pointer reservations */
        416,420,448,468,504,508 /* derived unit scalar reservations */
    };
    for (i = 0; i < 10; ++i) save_int(f, values[i]);
    save_pointer(f, f);
    save_string(f, text);
    slot_write_fixed(f, text, 8);
    check(!lg_slot_error);
    check(fseek(f, 0, SEEK_SET) == 0);
    for (i = 0; i < 10; ++i) check(load_int(f) == values[i]);
    check(load_pointer(f) == 0);
    loaded = load_string(f);
    check(loaded && strlen(loaded) == 5);
    for (i = 0; i < 5; ++i) check((loaded[i] & 255) == (text[i] & 255));
    free(loaded);
    slot_read_fixed(f, field, 8);
    for (i = 5; i < 8; ++i) check(field[i] == 0);
    check(fseek(f, 8, SEEK_SET) == 0);
    check(fread(octets, 1, 4, f) == 4);
    for (i = 0; i < 4; ++i) check(octets[i] == 255); /* -1 canonical */
    check(fseek(f, 4L * 10L, SEEK_SET) == 0);
    check(fread(octets, 1, 4, f) == 4);
    for (i = 0; i < 4; ++i) check(octets[i] == 0); /* pointer not leaked */
    check(fseek(f, 0, SEEK_SET) == 0);
    write_unit_fixture(f); /* deliberately contains nonzero transient values */
    for (i = 0; i < (long)(sizeof(reserved_offsets)/sizeof(reserved_offsets[0])); ++i) {
        check(fseek(f, reserved_offsets[i], SEEK_SET) == 0);
        check(load_int(f) == 0);
    }
}
'''

HOST_MAIN = r'''
int main(void)
{
    FILE *f = tmpfile();
    FILE *bad;
    unsigned char bytes[16384], invalid[4] = {255,255,255,255};
    long length, n;
    assert(f);
    codec_tests(f);
    fclose(f);
    f = tmpfile(); assert(f);
    lg_slot_error = 0;
    fixture(f, 0);
    check(!lg_slot_error);
    check(slot_preflight(f));
    fseek(f, 0, SEEK_END); length = ftell(f);
    rewind(f); check(fread(bytes, 1, length, f) == (size_t)length);
    for (n = 0; n < length; ++n) {
        bad = tmpfile(); assert(bad);
        fwrite(bytes, 1, n, bad);
        check(!slot_preflight(bad));
        fclose(bad);
    }
    fseek(f, 268, SEEK_SET); fwrite(invalid, 1, 4, f);
    check(!slot_preflight(f)); /* negative string length */
    fclose(f);
    f = tmpfile(); fixture(f, 0);
    fseek(f, 256, SEEK_SET); fputc('X', f);
    check(!slot_preflight(f)); /* ambiguous/unknown native format */
    fclose(f);
    f = tmpfile(); fixture(f, 1);
    check(slot_preflight(f)); /* campaign and core unit transfer records */
    fclose(f);
    printf("LGENERAL_SAVE_TESTS octet_cells=8 checks=%lu PASS\n", checks);
    return 0;
}
'''

CELL_MAIN = r'''
int main(void)
{
    Cell original[16384];
    size_t length, n, i;
    CellFile *f = &storage;
    codec_tests(f);
    f->at = f->size = 0;
    lg_slot_error = 0;
    fixture(f, 0);
    check(!lg_slot_error);
    check(slot_preflight(f));
    length = f->size;
    for (i = 0; i < length; ++i) original[i] = f->data[i];
    for (n = 0; n < length; ++n) {
        f->size = n; f->at = 0;
        check(!slot_preflight(f));
    }
    f->size = length;
    for (i = 0; i < length; ++i) f->data[i] = original[i];
    for (i = 268; i < 272; ++i) f->data[i] = 255;
    check(!slot_preflight(f));
    for (i = 0; i < length; ++i) f->data[i] = original[i];
    f->data[256] = 'X';
    check(!slot_preflight(f));
    f->at = f->size = 0;
    fixture(f, 1);
    check(slot_preflight(f));
    printf("LGENERAL_SAVE_TESTS octet_cells=16 checks=%lu PASS\n", checks);
    return 0;
}
'''


def adapted_slot_source() -> str:
    archive = ROOT / "build/downloads/lgeneral-1.4.4.tar.gz"
    with tempfile.TemporaryDirectory(prefix="lgeneral-save-source-") as tmp:
        with tarfile.open(archive) as stream:
            stream.extractall(tmp, filter="data")
        source = Path(tmp) / "lgeneral-1.4.4"
        MODULE.adapt(source, True)
        text = (source / "src/slot.c").read_text()
    return text


def actual_unit_writer() -> str:
    text = adapted_slot_source()
    start = text.index("static void save_unit_lib_entry(")
    end = text.index("static void load_unit_lib_entry(", start)
    writers = text[start:end].replace('save_string( file, "none" )', 'save_string(file, none_text)')
    entry_fields = sorted(set(re.findall(r"entry->(\w+)", writers)) | {"eval_score"})
    pointer_fields = {"id", "name", "icon", "icon_tiny", "wav_move"}
    unit_fields = sorted(set(re.findall(r"unit->(\w+)", writers)) |
                         {"icon_offset", "icon_tiny_offset", "damage_bar_width",
                          "fresh_deploy", "eval_score", "target_score"})
    special_fields = {"prop", "trsp_prop", "sel_prop", "backup", "name", "tag",
                      "player", "nation", "terrain"}
    structures = "typedef struct { char *id; } SaveIdentity;\ntypedef struct {\n"
    for field in entry_fields:
        if field == "atks":
            structures += "    long atks[TARGET_TYPE_LIMIT];\n"
        else:
            structures += ("    char *" if field in pointer_fields else "    long ") + field + ";\n"
    structures += "} Unit_Lib_Entry;\ntypedef struct {\n"
    structures += ("    Unit_Lib_Entry prop, trsp_prop, *sel_prop;\n"
                   "    void *backup, *terrain;\n    char name[24], tag[32];\n"
                   "    SaveIdentity *player, *nation;\n")
    for field in unit_fields:
        if field not in special_fields:
            structures += "    long " + field + ";\n"
    structures += "} Unit;\n"
    structures += ("#define COMPILE_TIME_ASSERT_SYMBOL(x) ((void)0)\n"
                   "enum { StoreCoreUnitFlag = 1004 };\n"
                   "static char none_text[] = { 'n','o','n','e',0 };\n")
    return structures + writers + r'''
static void write_unit_fixture(FILE *file)
{
    char id[] = { 'u','n','i','t',0 };
    SaveIdentity identity;
    Unit unit;
    identity.id = id;
    memset(&unit, 0, sizeof(unit));
    unit.prop.id = id;
    unit.prop.eval_score = 32768;
    unit.trsp_prop.eval_score = 2147483647L;
    unit.icon_offset = unit.icon_tiny_offset = unit.damage_bar_width = 12345;
    unit.fresh_deploy = 1;
    unit.eval_score = unit.target_score = 55555;
    unit.player = unit.nation = &identity;
    save_unit(file, &unit);
}
static void write_transfer_fixture(FILE *file)
{
    char id[] = { 'u','n','i','t',0 };
    Unit_Lib_Entry entry;
    long i;
    memset(&entry, 0, sizeof(entry));
    entry.eval_score = 123456;
    save_unit_lib_entry(file, &entry);
    save_unit_lib_entry(file, &entry);
    for (i = 0; i < 5; ++i) save_string(file, id);
    save_int(file, 10); save_int(file, 150);
    save_string(file, id);
}
'''


def run(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    actual_writer = actual_unit_writer()
    for cells in (8, 16):
        codec, tests, writer = MODULE.CODEC, TESTS, actual_writer
        prelude = ""
        if cells == 16:
            prelude = MEMORY_STREAM
            codec = re.sub(r"\bunsigned char\b|\bchar\b", "Cell", codec)
            tests = re.sub(r"\bunsigned char\b|\bchar\b", "Cell", tests)
            writer = re.sub(r"\bunsigned char\b|\bchar\b", "Cell", writer)
        source = directory / f"save-cells-{cells}.c"
        source.write_text(
            "#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "enum { StoreCanonicalOctets = 1007, TARGET_TYPE_LIMIT = 10 };\n"
            + prelude + codec + writer + tests + (HOST_MAIN if cells == 8 else CELL_MAIN))
        output = directory / f"save-cells-{cells}"
        flags = []
        sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
        if sys.platform == "darwin" and not os.environ.get("SDKROOT") and sdk.exists():
            flags += ["-isysroot", str(sdk)]
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", "-Wall", "-Wextra",
                        "-Werror", "-fsanitize=undefined", *flags,
                        str(source), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    if args.build:
        run(args.build)
    else:
        with tempfile.TemporaryDirectory(prefix="lgeneral-saves-") as tmp:
            run(Path(tmp))
