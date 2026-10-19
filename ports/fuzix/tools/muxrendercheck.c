/* Decode actual mux output with the production VT; compare every cell. */
#include <sys/ioctl.h>
#include <assert.h>
#include "../ripple/mux/mux.h"
#include "../ripple/mux/model.inc"
#include "../ripple/mux/render.inc"
static void disconnect(void){abort();}
int main(void){
    struct vt decoded;unsigned x,y,bytes=0,rounds=0;struct vt_cell *actual;
    cols=240;rows=60;client=1;
    screen=calloc((unsigned long)cols*rows,sizeof(*screen));
    painted=malloc((unsigned long)cols*rows*sizeof(*painted));assert(screen&&painted);
    memset(painted,255,(unsigned long)cols*rows*sizeof(*painted));memset(damaged,1,rows);
    assert(!vt_init_size(&decoded,cols,rows));
    for(y=0;y<rows;y++)for(x=0;x<cols;x++){
        screen[y*cols+x].ch=' ';screen[y*cols+x].colour=((y/10)%8)*16+7;
    }
    for(y=1;y<rows-1;y++)label(5,y,"mux / Rift / Music",screen[y*cols].colour,cols);
    screen[5*cols+2].ch=0x416;screen[8*cols+3].ch='X';screen[8*cols+3].colour=0x107;
    rendering=1;
    while(rendering){
        render();assert(out_count);bytes+=out_count;vt_feed(&decoded,outgoing,out_count);out_count=out_start=0;
        if(rendering)assert(decoded.synchronized);
        assert(++rounds<20);
    }
    assert(!decoded.synchronized);actual=vt_cells(&decoded);
    for(x=0;x<cols*rows;x++)assert(actual[x].ch==screen[x].ch&&actual[x].colour==screen[x].colour);
    assert(bytes<4000);
    printf("MUX_RENDER_OK: %u cells, %u ANSI octets, %u bounded rounds; every cell/style matches\n",cols*rows,bytes,rounds);
    tabs[0].used=1;tabs[0].focus=0;panes[0].used=panes[0].shown=1;
    panes[0].x=0;panes[0].y=1;panes[0].w=cols;panes[0].h=rows-2;
    assert(!vt_init_size(&panes[0].vt,cols,rows-2));
    vt_feed(&panes[0].vt,"\033[?2026hZ",9);full=0;dirty=1;
    compose();assert(screen[cols].ch==' '&&panes[0].vt.dirty[0]);
    vt_feed(&panes[0].vt,"\033[?2026l",8);dirty=1;
    compose();assert(screen[cols].ch=='Z');vt_destroy(&panes[0].vt);
    vt_destroy(&decoded);free(screen);free(painted);return 0;
}
