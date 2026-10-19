/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest.h"
#include "doomstat.h"
#include "m_argv.h"
#include "tables.h"
#include "info.h"
#include "i_system.h"

char *doom_guest_wad_path;
unsigned long doom_guest_frames;
int doom_guest_fast;
int doom_guest_stats;
void D_DoomMain(void);
void doomgeneric_Tick(void);

int main(int argc, char **argv)
{
    int i, have_iwad = 0;
    char *slash, *default_wad;
    char **arguments;
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        puts("Doom for Fuse-X / FUZIX\n"
             "Usage: doom [-iwad PATH] [-warp 1 1] [Doom options]\n"
             "Defaults to doom1.wad beside the executable. Keyboard controls:\n"
             "arrows: move/turn, Ctrl: fire, Space: use, Shift: run, Esc: menu.\n"
             "Validation: --frames N [--fast] exits after N presented frames.\n"
             "Diagnostics: --stats reports frame time and audio underruns.");
        return 0;
    }
    arguments = malloc(((size_t)argc + 3UL) * sizeof(*arguments));
    default_wad = malloc(strlen(argv[0]) + 11UL);
    if (!arguments || !default_wad) I_Error("Cannot allocate arguments");
    strcpy(default_wad, argv[0]);
    slash = strrchr(default_wad, '/');
    if (slash) strcpy(slash + 1, "doom1.wad");
    else strcpy(default_wad, "doom1.wad");
    doom_guest_wad_path = default_wad;
    myargc = 1;
    arguments[0] = argv[0];
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames")) {
            char *end;
            if (++i == argc) I_Error("--frames requires a positive number");
            doom_guest_frames = strtoul(argv[i], &end, 10);
            if (!argv[i][0] || argv[i][0] == '-' || *end || !doom_guest_frames)
                I_Error("--frames requires a positive number");
        } else if (!strcmp(argv[i], "--fast")) {
            doom_guest_fast = 1;
        } else if (!strcmp(argv[i], "--stats")) {
            doom_guest_stats = 1;
        } else {
            if (!strcmp(argv[i], "-iwad")) {
                if (i + 1 == argc) I_Error("-iwad requires a path");
                doom_guest_wad_path = argv[i + 1];
                have_iwad = 1;
            }
            arguments[myargc++] = argv[i];
        }
    }
    if (!have_iwad) {
        arguments[myargc++] = "-iwad";
        arguments[myargc++] = doom_guest_wad_path;
    }
    arguments[myargc] = NULL;
    myargv = arguments;
    if (!Doom_InitTables() || !Doom_InitInfo()) I_Error("Cannot allocate engine tables");
    singletics = true;
    D_DoomMain();
    for (;;) doomgeneric_Tick();
    return 0;
}
