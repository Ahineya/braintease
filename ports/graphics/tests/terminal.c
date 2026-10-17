/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct vt t;
static char answer[64];
static int scrolled;
static void feed(const char *s){vt_feed(&t,s,strlen(s));}
static void reply(struct vt *v,const char *s){(void)v;strcpy(answer,s);}
static void scroll(struct vt *v,int n){assert(v->top==1&&v->bottom==3);scrolled+=n;}
static unsigned ch(unsigned y,unsigned x){return vt_cells(&t)[y*VT_COLS+x].ch;}
int main(void) {
    unsigned i;
    vt_init(&t);feed("hello\r\nworld");assert(ch(0,0)=='h'&&ch(1,0)=='w');
    feed("\033[");feed("2;3H\033[1;31;44m!");assert(ch(1,2)=='!');assert(vt_cells(&t)[82].colour==0x49);
    feed("\033[0m\033[2K");assert(ch(1,0)==' '&&ch(1,2)==' ');
    feed("\033[1;1Habcde\033[1;3H\033[2P");assert(ch(0,2)=='e'&&ch(0,3)==' ');
    feed("\033[2@XY");assert(ch(0,2)=='X'&&ch(0,3)=='Y'&&ch(0,4)=='e');
    feed("\033[?1049hother\033[?1049l");assert(ch(0,0)=='a'&&ch(0,4)=='e'&&t.x==4);
    feed("\033[9999999999999999;999999999999999H");assert(t.x==79&&t.y==29);
    feed("\033[2J\033[H");for(i=0;i<80;i++)feed("x");assert(t.x==79&&t.y==0&&t.pending);
    feed("z");assert(ch(1,0)=='z'&&t.x==1);
    feed("\033[2;4r\033[2;1HA\033[3;1HB\033[4;1HC");
    t.scroll=scroll;feed("\n");assert(scrolled==1&&ch(1,0)=='B'&&ch(2,0)=='C'&&ch(3,0)==' ');
    feed("\033[2;1H\033M");assert(scrolled==0&&ch(1,0)==' '&&ch(2,0)=='B');
    t.reply=reply;feed("\033[3;7H\033[6n");assert(!strcmp(answer,"\033[3;7R"));
    feed("\033]0;ignored title\033\\!");assert(ch(2,6)=='!');
    feed("\033[?25l\033[?1h");assert(!t.visible&&t.appcursor);
    t.scroll=0;
    /* Arbitrary fragmented streams must preserve all parser/model bounds. */
    for(i=0;i<20000;i++){char c=(i*109+71)%128;vt_feed(&t,&c,1);assert(t.x<80&&t.y<30&&t.top<=t.bottom&&t.bottom<30&&t.count<16);}
    puts("terminal: fragmented ANSI, colours, editing, margins, alternate screen and parser bounds passed");
    return 0;
}
