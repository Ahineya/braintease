/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct vt t;
static void init(void){vt_destroy(&t);assert(vt_init_size(&t,80,30)==0);}
static char answer[64];
static int scrolled;
static void feed(const char *s){vt_feed(&t,s,strlen(s));}
static void reply(struct vt *v,const char *s){(void)v;strcpy(answer,s);}
static void scroll(struct vt *v,int n){assert(v->top==1&&v->bottom==3);scrolled+=n;}
static char osc_text[64];static unsigned osc_length,osc_completed,osc_aborted;
static void observe_osc(struct vt *v,unsigned c,int event){
    (void)v;
    if(event<0){osc_length=0;osc_aborted++;}
    else if(event>0){osc_text[osc_length]=0;osc_completed++;}
    else if(osc_length+1<sizeof(osc_text))osc_text[osc_length++]=c;
}
static void osc_stream(void){
    const char *packet="\033]777;mux;1;12;2f;2d\033\\";unsigned i,n=strlen(packet);
    for(i=0;i<=n;i++){
        init();t.osc=observe_osc;osc_completed=osc_aborted=0;
        vt_feed(&t,packet,i);vt_feed(&t,packet+i,n-i);
        assert(osc_completed==1&&osc_aborted==1&&!strcmp(osc_text,"777;mux;1;12;2f;2d")&&t.x==0);
    }
    feed("\033]idle\007");assert(osc_completed==2&&!strcmp(osc_text,"idle"));
    feed("\033]cancel\030X");assert(osc_completed==2&&osc_aborted==4);
    feed("\033Pnot-an-osc\033\\Y");assert(osc_completed==2&&osc_aborted==4);
}
static uint32_t ch(unsigned y,unsigned x){return vt_cells(&t)[y*t.cols+x].ch;}
static unsigned pattern(unsigned plane,unsigned y,unsigned x){return '!'+(plane*17+y*11+x)%90;}
static void sizes(void) {
    static const unsigned dims[][2]={{1,1},{7,4},{17,9},{64,20},{80,30},{40,30},{80,1},{120,40},{240,100}};
    static struct vt saved;
    unsigned a,b,p,x,y,oldcols,oldrows,cols,rows;
    init();saved=t;
    assert(vt_init_size(&t,0,1)==-1&&vt_resize(&t,VT_MAX_COLS+1,20)==-1);
    assert(vt_init_size(&t,80,VT_MAX_ROWS+1)==-1&&vt_resize(&t,1,0)==-1);
    assert(!memcmp(&t,&saved,sizeof(t)));
    for(a=0;a<sizeof(dims)/sizeof(dims[0]);a++)for(b=0;b<sizeof(dims)/sizeof(dims[0]);b++) {
        oldcols=dims[a][0];oldrows=dims[a][1];cols=dims[b][0];rows=dims[b][1];
        vt_destroy(&t);assert(vt_init_size(&t,oldcols,oldrows)==0);
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
    vt_destroy(&t);assert(vt_init_size(&t,7,4)==0);feed("1234567Z");assert(ch(0,6)=='7'&&ch(1,0)=='Z');
    t.reply=reply;feed("\033[999;999H\033[6n");assert(!strcmp(answer,"\033[4;7R"));
    feed("\033[?1049h\033[4;7H\0337");assert(vt_resize(&t,3,2)==0);
    feed("\0338\033[?1049l");assert(t.x==2&&t.y==1&&t.cols==3&&t.rows==2);
    assert(vt_resize(&t,7,4)==0);assert(ch(1,3)==' '&&ch(3,6)==' ');
    feed("\033c");assert(t.cols==7&&t.rows==4&&t.bottom==3&&ch(0,0)==' ');
    for(a=0;a<20000;a++) {
        char c=(a*109+71)%128;
        if(a%31==0){b=(a/31)%9;assert(vt_resize(&t,dims[b][0],dims[b][1])==0);}
        vt_feed(&t,&c,1);
        assert(t.x<t.cols&&t.y<t.rows&&t.top<=t.bottom&&t.bottom<t.rows&&t.count<16);
    }
}
int main(void) {
    init();feed("\033[?2026h");assert(t.synchronized);
    feed("frame");assert(t.synchronized&&ch(0,0)=='f');
    feed("\033[?2026l");assert(!t.synchronized);
    unsigned i;
    /* Each block occupies one cell, even across all possible read splits. */
    for(i=0;i<=9;i++) {
        const char *blocks="\342\226\200\342\226\204\342\226\210";
        init();feed("\033[30;107m");
        vt_feed(&t,blocks,i);vt_feed(&t,blocks+i,9-i);
        assert(t.x==3&&ch(0,0)==0x2580&&ch(0,1)==0x2584&&ch(0,2)==0x2588);
        assert(vt_cells(&t)[0].colour==0xf0);
    }
    init();feed("\342\033[2;2HX");
    assert(ch(0,0)==RU_REPLACEMENT&&ch(1,1)=='X');
    init();feed("\342\226\033cX");
    assert(ch(0,0)=='X'&&!t.utf8.need);
    init();feed("\033[1;80H\342\226\204X");
    assert(ch(0,79)==0x2584&&ch(1,0)=='X');
    init();feed("\033(");feed("0lqk\033(Babc");
    assert(ch(0,0)==0x250c&&ch(0,1)==0x2500&&ch(0,2)==0x2510&&ch(0,3)=='a');
    feed("\033)0\016x\017x");assert(ch(0,6)==0x2502&&ch(0,7)=='x');
    feed("\033cqx");assert(ch(0,0)=='q'&&ch(0,1)=='x');
    init();feed("hello\r\nworld");assert(ch(0,0)=='h'&&ch(1,0)=='w');
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
    init();feed("\033[4;5mX\033[24;25mY\033[6 q");
    assert(vt_cells(&t)[0].colour==(7|256|512)&&vt_cells(&t)[1].colour==7&&t.cursor_shape==6);
    feed("\033Pignored\033\\Z");assert(ch(0,2)=='Z');
    init();feed("\033[38;5;196;48;2;0;0;255mX");
    assert((vt_cells(&t)[0].colour&15)==1&&(vt_cells(&t)[0].colour>>4)==4);
    /* UTF-8 reads can split at every octet; cells retain scalars/graphemes. */
    for(i=0;i<=14;i++) {
        const char *mixed="A\320\226e\314\201\344\270\255\360\237\231\202Z";
        init();vt_feed(&t,mixed,i);vt_feed(&t,mixed+i,14-i);
        assert(t.x==8&&ch(0,0)=='A'&&ch(0,1)==0x416);
        assert(ru_scalar(ch(0,2),0)=='e'&&ru_scalar(ch(0,2),1)==0x301);
        assert(ch(0,3)==0x4e2d&&ch(0,4)==RU_CONTINUATION);
        assert(ch(0,5)==0x1f642UL&&ch(0,6)==RU_CONTINUATION&&ch(0,7)=='Z');
    }
    init();feed("\344\270\255\033[1;2HX");assert(ch(0,0)==' '&&ch(0,1)=='X');
    init();feed("\344\270\255\033[1;2H\033[K");assert(ch(0,0)==' '&&ch(0,1)==' ');
    init();feed("\344\270\255\033[1;2H\033[P");assert(ch(0,0)==RU_REPLACEMENT&&ch(0,1)==' ');
    init();feed("\033[1;80H\344\270\255");assert(ch(1,0)==0x4e2d&&ch(1,1)==RU_CONTINUATION);
    init();feed("\033[1;79H\344\270\255");assert(vt_resize(&t,79,30)==0&&ch(0,78)==RU_REPLACEMENT);
    init();feed("\342\320\226");assert(ch(0,0)==RU_REPLACEMENT&&ch(0,1)==0x416);
    {
        char report[40];init();
        assert(!vt_mouse_report(&t,report,0,2,3,0,0,0));
        feed("\033[?1006h\033[?1002h");
        assert(t.mouse_mode==1002&&t.mouse_sgr);
        assert(vt_mouse_report(&t,report,0,2,3,0,0,16)&&!strcmp(report,"\033[<16;3;4M"));
        assert(t.mouse_buttons==1);
        assert(vt_mouse_report(&t,report,0,3,3,0,1,0)&&!strcmp(report,"\033[<32;4;4M"));
        assert(!vt_mouse_report(&t,report,0,3,3,0,1,0));
        assert(vt_mouse_report(&t,report,0,-4,999,1,0,0)&&!strcmp(report,"\033[<0;1;30m")&&!t.mouse_buttons);
        assert(!vt_mouse_report(&t,report,3,4,3,0,1,0));
        feed("\033[?1003h");assert(vt_mouse_report(&t,report,3,4,3,0,1,0));
        assert(!strcmp(report,"\033[<35;5;4M"));
        assert(vt_mouse_report(&t,report,64,4,3,0,0,0)&&!strcmp(report,"\033[<64;5;4M"));
        assert(!vt_mouse_report(&t,report,64,4,3,1,0,0));
        feed("\033[?1003l\033[?1000h\033[?1006l");
        assert(!vt_mouse_report(&t,report,0,4,3,0,1,0));
        assert(vt_mouse_report(&t,report,2,4,3,0,0,0)==6&&!memcmp(report,"\033[M\042\045\044",6));
        feed("\033c");assert(!t.mouse_mode&&!t.mouse_sgr&&!t.mouse_buttons);
    }
    osc_stream();sizes();vt_destroy(&t);
    puts("terminal: ANSI/editing, configurable geometry, both-screen resize preservation, cursor clamping and fragmented parser/resize bounds passed");
    return 0;
}
