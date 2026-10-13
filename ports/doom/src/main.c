#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "doom_port.h"
#include "doomstat.h"
#include "m_argv.h"
#include "m_menu.h"
#include "tables.h"
#include "info.h"
#include "i_system.h"

void D_DoomMain(void);
void doomgeneric_Tick(void);

int main(int argc, char **argv)
{
    static char *engine_args[7] = {"doom-ripple", "-iwad", DOOM_WAD_PATH, "-warp", "1", "1", NULL};
    int start_map = 0;
    int start_demo = 0;
#ifdef DOOM_START_MAP
    start_map = 1;
#endif
#ifdef DOOM_START_DEMO
    start_demo = 1;
#endif
#ifndef RIPPLE
    if (argc < 2) {
        fprintf(stderr, "Usage: engine OUTPUT.rgb565 [FRAME] [title|map|demo1] [INPUT.txt]\n");
        return 1;
    }
    Doom_ConfigureCapture(argv[1], argc > 2 ? strtoul(argv[2], NULL, 10) : 1UL,
                          argc > 4 ? argv[4] : NULL);
    if (argc > 3) {
        start_map = strcmp(argv[3], "map") == 0;
        start_demo = strcmp(argv[3], "demo1") == 0;
    }
#endif
    if (!Doom_InitTables()) I_Error("Doom numeric table allocation failed");
    if (!Doom_InitInfo()) I_Error("Doom state table allocation failed");
    myargc = start_map ? 6 : 3;
    if (start_demo) {
        engine_args[3] = "-iwaddemo";
        engine_args[4] = "demo1";
        engine_args[5] = NULL;
        myargc = 5;
    }
    myargv = engine_args;
    singletics = true;
#ifdef DOOM_LOW_DETAIL
    detailLevel = 1;
#endif
    printf("DOOM PORT WAD %s\n", DOOM_WAD_SHA256);
    D_DoomMain();
    for (;;) doomgeneric_Tick();
    return 0;
}
