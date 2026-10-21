/* Route real SGR packets through the production mux parser and split model. */
#include <assert.h>
#include <sys/ioctl.h>
#include "../ripple/mux/mux.h"
#include "../ripple/mux/model.inc"
#include "../ripple/mux/render.inc"
static int save_session(void){return 0;}
static void disconnect(void){client=-1;}
#include "../ripple/mux/input.inc"
static void input(const char *s){while(*s)handle_input((unsigned char)*s++);}
int main(void){
    unsigned old;
    cols=80;rows=24;current=0;tabs[0].used=1;tabs[0].root=0;
    assert(new_node()==0&&new_node()==1&&new_node()==2);
    nodes[0].a=1;nodes[0].b=2;nodes[0].vertical=1;
    nodes[1].pane=0;nodes[2].pane=1;nodes[1].parent=nodes[2].parent=0;
    panes[0].used=panes[1].used=1;panes[0].pid=10;panes[1].pid=11;
    panes[0].node=1;panes[1].node=2;panes[0].fd=panes[1].fd=-1;
    geometry_node(0,0,1,80,22);
    assert(!vt_init_size(&panes[0].vt,panes[0].w,panes[0].h));
    assert(!vt_init_size(&panes[1].vt,panes[1].w,panes[1].h));
    vt_feed(&panes[1].vt,"\033[?1006h\033[?1002h",16);
    input("\033[<0;45;");assert(tabs[0].focus==0);input("6M");
    assert(tabs[0].focus==1&&mouse_pane==1);
    assert(panes[1].queued==9&&!memcmp(panes[1].input,"\033[<0;5;5M",9));
    input("\033[<32;3;6M\033[<0;3;6m");
    assert(mouse_pane==-1&&tabs[0].focus==1);
    assert(panes[1].queued==28&&!memcmp(panes[1].input+9,"\033[<32;1;5M\033[<0;1;5m",19));
    panes[1].queued=0;input("\033[<35;45;6M");assert(!panes[1].queued);
    input("\033[<0;45;6M");panes[1].queued=QUEUE-100;
    input("\033[<32;46;6M");assert(panes[1].queued==QUEUE-100);
    input("\033[<0;46;6m");assert(panes[1].queued>QUEUE-100&&!panes[1].vt.mouse_buttons);
    panes[1].queued=0;
    input("\033[<64;45;6M");assert(panes[1].queued==10);
    old=nodes[0].ratio;input("\033[<0;40;6M\033[<32;50;6M\033[<0;50;6m");
    assert(nodes[0].ratio>old&&need_layout&&mouse_split==-1);
    panes[1].queued=0;input("\033[A");assert(panes[1].queued==3&&!memcmp(panes[1].input,"\033[A",3));
    panes[1].queued=0;input("\033[200~\033[<0;1;1M\033[201~");
    assert(tabs[0].focus==1&&panes[1].queued==21&&!mouse_paste);
    panes[1].queued=0;input("\033");mouse_deadline=0;mouse_timeout();assert(panes[1].queued==1&&panes[1].input[0]==27);
    input("\033[<0;45;6M");cancel_mouse_capture();assert(!panes[1].vt.mouse_buttons&&mouse_pane==-1);
    tabs[1].used=1;strcpy(tabs[1].name,"other");
    input("\033[<0;7;1M");assert(current==1);
    input("\033[<0;2;1M");assert(current==0);
    vt_destroy(&panes[0].vt);vt_destroy(&panes[1].vt);
    puts("MUX_MOUSE_OK: fragmented input, focus, local coordinates, drag capture, wheel, split resize, paste and cancellation");
    return 0;
}
