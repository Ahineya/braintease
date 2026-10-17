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
static unsigned ch(unsigned y,unsigned x){return vt_cells(&t)[y*t.cols+x].ch;}
static unsigned pattern(unsigned plane,unsigned y,unsigned x){return '!'+(plane*17+y*11+x)%90;}
static void sizes(void) {
    static const unsigned dims[][2]={{1,1},{7,4},{17,9},{64,20},{80,30},{40,30},{80,1}};
    static struct vt saved;
    unsigned a,b,p,x,y,oldcols,oldrows,cols,rows;
    vt_init(&t);saved=t;
    assert(vt_init_size(&t,0,1)==-1&&vt_resize(&t,81,20)==-1);
    assert(vt_init_size(&t,80,31)==-1&&vt_resize(&t,1,0)==-1);
    assert(!memcmp(&t,&saved,sizeof(t)));
    for(a=0;a<sizeof(dims)/sizeof(dims[0]);a++)for(b=0;b<sizeof(dims)/sizeof(dims[0]);b++) {
        oldcols=dims[a][0];oldrows=dims[a][1];cols=dims[b][0];rows=dims[b][1];
        assert(vt_init_size(&t,oldcols,oldrows)==0);
        for(p=0;p<2;p++)for(y=0;y<oldrows;y++)for(x=0;x<oldcols;x++) {
            t.cells[p][y*oldcols+x].ch=pattern(p,y,x);
            t.cells[p][y*oldcols+x].colour=0x12+p;
        }
        t.alternate=1;t.x=t.sx=t.primary_x=oldcols-1;t.y=t.sy=t.primary_y=oldrows-1;
        memset(t.dirty,0,sizeof(t.dirty));
        assert(vt_resize(&t,cols,rows)==0&&t.alternate==1);
        assert(t.x<cols&&t.y<rows&&t.sx<cols&&t.sy<rows&&t.primary_x<cols&&t.primary_y<rows);
        for(p=0;p<2;p++)for(y=0;y<rows;y++)for(x=0;x<cols;x++) {
            assert(t.cells[p][y*cols+x].ch==((x<oldcols&&y<oldrows)?pattern(p,y,x):' '));
            assert(t.cells[p][y*cols+x].colour==((x<oldcols&&y<oldrows)?0x12+p:7));
        }
        if(cols!=oldcols||rows!=oldrows)for(y=0;y<rows;y++)assert(t.dirty[y]);
    }
    assert(vt_init_size(&t,7,4)==0);feed("1234567Z");assert(ch(0,6)=='7'&&ch(1,0)=='Z');
    t.reply=reply;feed("\033[999;999H\033[6n");assert(!strcmp(answer,"\033[4;7R"));
    feed("\033[?1049h\033[4;7H\0337");assert(vt_resize(&t,3,2)==0);
    feed("\0338\033[?1049l");assert(t.x==2&&t.y==1&&t.cols==3&&t.rows==2);
    assert(vt_resize(&t,7,4)==0);assert(ch(1,3)==' '&&ch(3,6)==' ');
    feed("\033c");assert(t.cols==7&&t.rows==4&&t.bottom==3&&ch(0,0)==' ');
    for(a=0;a<20000;a++) {
        char c=(a*109+71)%128;
        if(a%31==0){b=(a/31)%7;assert(vt_resize(&t,dims[b][0],dims[b][1])==0);}
        vt_feed(&t,&c,1);
        assert(t.x<t.cols&&t.y<t.rows&&t.top<=t.bottom&&t.bottom<t.rows&&t.count<16);
    }
}
int main(void) {
    unsigned i;
    vt_init(&t);feed("\033(");feed("0lqk\033(Babc");
    assert(ch(0,0)==218&&ch(0,1)==196&&ch(0,2)==191&&ch(0,3)=='a');
    feed("\033)0\016x\017x");assert(ch(0,6)==179&&ch(0,7)=='x');
    feed("\033cqx");assert(ch(0,0)=='q'&&ch(0,1)=='x');
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
    sizes();
    puts("terminal: ANSI/editing, configurable geometry, both-screen resize preservation, cursor clamping and fragmented parser/resize bounds passed");
    return 0;
}
