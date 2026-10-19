/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Platform definitions for the retained NXDSKTOP bar/menu drawing routines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <syscalls.h>
#include "resources.h"
#include "nano-X.h"
#include "nxcolors.h"

#define TEXT_Y_OFFSET_MENU 14
#define TEXT_Y_OFFSET_TASKBAR 18
#define TASKBAR_HEIGHT 24
#define START_WIDTH 60
#define MENU_WIDTH 184
#define MENU_ITEM_HEIGHT 22
#define MENU_ITEM_EXIT_HEIGHT 22
static const char *apps[]={"About","Help","Calculator","Terminal","Edit file","Processes","Clock","Eyes","Roaches","Tetris","Image Viewer","Klotski"};
enum {MENU_ABOUT,MENU_HELP,MENU_CALCULATOR,MENU_TERMINAL,MENU_EDIT,MENU_PROCESSES,
      MENU_CLOCK,MENU_EYES,MENU_ROACHES,MENU_TETRIS,MENU_IMAGES,MENU_KLOTSKI,MENU_APP_COUNT,
      MENU_SYNC=MENU_APP_COUNT,MENU_EXIT,MENU_SHUTDOWN};
static const char *sys[]={"Sync disk","Exit to terminal","Shut down"};
#define APP_COUNT MENU_APP_COUNT
#define SYS_COUNT (sizeof(sys)/sizeof(sys[0]))
#define MENU_HEIGHT (APP_COUNT*MENU_ITEM_HEIGHT+SYS_COUNT*MENU_ITEM_EXIT_HEIGHT+8)
static GR_WINDOW_ID win,panel,popup;
static GR_GC_ID gc_bar,gc_text;
static int width,height=TASKBAR_HEIGHT,menu_open;
