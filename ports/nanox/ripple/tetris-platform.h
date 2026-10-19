/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <signal.h>
#include <unistd.h>
static nstate *tetris_state;
static int tetris_connected,tetris_trace,tetris_wake[2]={-1,-1};
static unsigned tetris_seed;
static volatile int tetris_stopping;
static char tetris_score_path[192],tetris_temp_path[192];
static unsigned long tetris_milliseconds(void);
static long tetris_read_score(void);
static void tetris_save_score(nstate *state);
static void tetris_keyboard(nstate *state);
static void tetris_mouse(nstate *state);
static int tetris_main(int argc,char **argv);
static void tetris_no_resource(void)
{
    fputs("nxtetris: no graphics resources available\n",stderr);exit(1);
}
static GR_WINDOW_ID tetris_window(GR_WINDOW_ID parent,GR_COORD x,GR_COORD y,
    GR_SIZE width,GR_SIZE height,GR_SIZE border,GR_COLOR background,GR_COLOR foreground)
{
    GR_WINDOW_ID id=GrNewWindow(parent,x,y,width,height,border,background,foreground);
    if(!id)tetris_no_resource();return id;
}
static GR_GC_ID tetris_gc(void)
{
    GR_GC_ID id=GrNewGC();if(!id)tetris_no_resource();return id;
}
