/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_MUX_H
#define RIPPLE_MUX_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <time.h>
#include "../pty-limits.h"
#include "../resources.h"
#include "../../../graphics/terminal.h"
#define PANES 12
#define TABS 8
#define NODES (PANES*2)
#define QUEUE 4096
#define OUTPUT 8192
#define COPY_SIZE 8192
#define MIN_COLS 12
#define MIN_ROWS 3
#define RESTART_SIZE 1024
#define DIRECTORY_SIZE 256
struct line { struct vt_cell *cells; unsigned width; };
struct pane {
    int used,fd,pid,session,tab,node,eof,dead;
    struct vt vt;
    struct line *history;
    unsigned head,count,limit;
    unsigned x,y,w,h,shown,activity;
    char input[QUEUE]; unsigned queued;
    char directory[DIRECTORY_SIZE],restart[RESTART_SIZE];
};
struct node { int used,pane,a,b,parent,vertical;unsigned ratio,x,y,w,h; };
struct tab { int used,root,focus,zoom;char name[25]; };
struct options {unsigned history,prefix;char shell[96];};
static struct pane panes[PANES];
static struct node nodes[NODES];
static struct tab tabs[TABS];
static struct options options={200,2,"/bin/sh"};
static int current,listenfd=-1,client=-1,stopping,need_layout=1,full=1,dirty=1;
static unsigned cols=80,rows=24,prefix_pending,help,closing,mode,erase_character=127;
static unsigned copy_line,copy_top,copy_mark,mark_set;
static char clipboard[COPY_SIZE],message[160],prompt[RESTART_SIZE],search[96],session_name[25];
static unsigned copied,prompt_len;
static char endpoint[108];
static struct vt_cell *screen,*painted;
static unsigned screen_cols,screen_rows,scan,rendering,style=65535;
static char outgoing[OUTPUT];static unsigned out_start,out_count;
static volatile int terminated,resized,child_changed;
static void notice(const char *text){snprintf(message,sizeof(message),"%s",text);dirty=1;}
static int nonblock(int fd){int f=fcntl(fd,F_GETFL,0);return f<0?-1:fcntl(fd,F_SETFL,f|O_NONBLOCK);}
static void signal_stop(int n){terminated=n;signal(n,signal_stop);}
static void signal_resize(int n){resized=1;signal(n,signal_resize);}
static void signal_child(int n){child_changed=1;signal(n,signal_child);}
static int retry_error(void){return errno==EINTR||errno==EAGAIN;}
static struct pane *focused(void){return &panes[tabs[current].focus];}
static void disconnect(void);
static void layout(void);
static int add_pane(int tab,char **command);
static void remove_pane(int p);
static void compose(void);
static void render(void);
static void handle_input(unsigned c);
static int save_session(void);
#endif
