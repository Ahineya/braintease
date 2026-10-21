#!/usr/bin/env python3
"""Fault-inject the actual adapted slot_save, preserving the previous slot."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_saves import adapted_slot_source, MODULE


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
enum { StoreCanonicalOctets=1007, StoreHighestSupportedVersion=1007,
       StoreVersionLimit=65535, TARGET_TYPE_LIMIT=10, SLOT_COUNT=2 };
static int fault;
static unsigned long checks;
static size_t write_with_fault(const void *p, size_t s, size_t n, FILE *f)
{
    if (fault == 1 && s*n) {
        size_t partial = n > 1 ? n/2 : 0;
        if (partial) fwrite(p, s, partial, f);
        errno = ENOSPC;
        return partial;
    }
    return fwrite(p,s,n,f);
}
static int close_with_fault(FILE *f)
{
    int result = fclose(f);
    if (fault == 2) { errno = ENOSPC; return EOF; }
    return result;
}
static int rename_with_fault(const char *from, const char *to)
{
    if (fault == 3) { errno = EACCES; return -1; }
    return rename(from,to);
}
#define fwrite write_with_fault
#define fclose close_with_fault
#define rename rename_with_fault
#define tr(x) (x)
#define COMPILE_TIME_ASSERT(x) ((void)0)
typedef struct { char name[256], fname[256]; int valid; } Slot;
static Slot slots[SLOT_COUNT];
static struct {
    char *dir_name;
    int fog_of_war,supply,weather,deploy_turn,purchase;
} config;
typedef struct { int count; } List;
static List empty_list;
static List *players=&empty_list, *units=&empty_list,
            *reinf=&empty_list, *avail_units=&empty_list;
typedef struct { char *id; } Player;
static Player current_player;
static Player *cur_player = &current_player;
static struct { char *id; } campaign;
static void *camp_cur_scen_void;
#define camp_cur_scen (&campaign)
static int camp_loaded,turn,map_w=1,map_h=1;
static char *camp_fname;
static struct { char *fname; } scenario;
#define scen_info (&scenario)
static void list_reset(List *list) { (void)list; }
static void *list_next(List *list) { (void)list; return 0; }
static int player_get_index(Player *player) { (void)player; return 0; }
static void *map_tile(int x, int y) { (void)x; (void)y; return 0; }
static void save_player(FILE *file, void *player) { (void)file; (void)player; }
static void save_unit(FILE *file, void *unit) { (void)file; (void)unit; }
static void save_prev_scen_core_units(FILE *file) { (void)file; }
static void save_map_tile_units(FILE *file, void *tile) { (void)file; (void)tile; }
static void save_map_tile_flag(FILE *file, void *tile) { (void)file; (void)tile; }
'''

TEST = r'''
static void check(int value) { ++checks; assert(value); }
static size_t read_save(const char *name, unsigned char *bytes, size_t capacity)
{
    FILE *file = fopen(name, "rb");
    size_t count;
    assert(file);
    count = fread(bytes, 1, capacity, file);
    check(!ferror(file));
    fclose(file);
    return count;
}
int main(int argc, char **argv)
{
    char path[1024], temp_path[1024];
    unsigned char before[4096], after[4096];
    size_t before_size, after_size;
    Slot previous;
    int mode;
    (void)camp_cur_scen_void;
    assert(argc == 2);
    config.dir_name = argv[1];
    scenario.fname = "serbia";
    snprintf(path, sizeof(path), "%s/lg_save_0", argv[1]);
    snprintf(temp_path, sizeof(temp_path), "%s/lg_save_0.tmp", argv[1]);
    check(slot_save(0, "Previous good save"));
    check(slots[0].valid && !strcmp(slots[0].name, "Previous good save"));
    check(!strcmp(slots[0].fname, path));
    check(access(temp_path,F_OK) != 0);
    before_size = read_save(path,before,sizeof(before));
    previous = slots[0];
    turn = 9;
    for (mode=1; mode<=3; ++mode) {
        fault=mode;
        check(!slot_save(0, "Replacement must not leak"));
        fault=0;
        after_size = read_save(path,after,sizeof(after));
        check(after_size == before_size && !memcmp(before,after,before_size));
        check(!memcmp(&previous,&slots[0],sizeof(previous)));
        check(access(temp_path,F_OK) != 0);
    }
    /* A failed first save must not make an empty slot appear loadable. */
    fault=1;
    check(!slot_save(1,"First save failed"));
    fault=0;
    check(!slots[1].valid && !slots[1].name[0] && !slots[1].fname[0]);
    snprintf(temp_path,sizeof(temp_path),"%s/lg_save_1.tmp",argv[1]);
    check(access(temp_path,F_OK) != 0);
    check(slot_save(0,"Replacement committed"));
    check(slots[0].valid && !strcmp(slots[0].name,"Replacement committed"));
    after_size=read_save(path,after,sizeof(after));
    check(after_size != before_size || memcmp(before,after,before_size));
    check(!memcmp(after,"Replacement committed",21));
    printf("LGENERAL_SAVE_TRANSACTION checks=%lu PASS\n",checks);
    return 0;
}
'''


def run(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    text = adapted_slot_source()
    start = text.index("int slot_save(")
    end = text.index("int slot_load(", start)
    source = folder / "save-transaction.c"
    source.write_text(PRELUDE + MODULE.CODEC + text[start:end] + TEST)
    output = folder / "save-transaction"
    flags = []
    sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
    if sys.platform == "darwin" and not os.environ.get("SDKROOT") and sdk.exists():
        flags += ["-isysroot", str(sdk)]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-function", "-fsanitize=undefined", *flags,
                    str(source), "-o", str(output)], check=True)
    with tempfile.TemporaryDirectory(prefix="lgeneral-save-failure-") as tmp:
        subprocess.run([str(output), tmp], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    if args.build:
        run(args.build)
    else:
        with tempfile.TemporaryDirectory(prefix="lgeneral-save-transaction-") as tmp:
            run(Path(tmp))
