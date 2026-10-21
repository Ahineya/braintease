/* Production desktop composition/input: no PTY or guest is required here. */
#include <assert.h>
#include <sys/ioctl.h>
#include "../ripple/mux/mux.h"
#include "../ripple/mux/model.inc"
#include "../ripple/mux/render.inc"
static int save_session(void){return 0;}
static void disconnect(void){abort();}
#include "../ripple/mux/input.inc"
static void input(const char *s){while(*s){handle_input((unsigned char)*s++);if(need_layout)layout();}}
static void packet(unsigned code,int x,int y,int release){char b[40];snprintf(b,sizeof(b),"\033[<%u;%d;%d%c",code,x+1,y+1,release?'m':'M');input(b);layout();}
static void solid(int i,unsigned ch){unsigned n;for(n=0;n<panes[i].vt.cols*panes[i].vt.rows;n++){vt_cells(&panes[i].vt)[n].ch=ch;vt_cells(&panes[i].vt)[n].colour=7;}vt_dirty(&panes[i].vt);dirty=1;}
static uint32_t cell(int x,int y){return screen[y*cols+x].ch;}
static unsigned frame(struct vt *decoded){
    unsigned bytes=0,i;struct vt_cell *cells;
    compose();while(rendering){render();bytes+=out_count;vt_feed(decoded,outgoing,out_count);out_start=out_count=0;}
    cells=vt_cells(decoded);for(i=0;i<cols*rows;i++)assert(cells[i].ch==screen[i].ch&&cells[i].colour==screen[i].colour);
    return bytes;
}
int main(void){
    struct vt decoded;unsigned x,y,w,h,bytes;int i;
    assert(!screen_size(80,24));client=1;
    assert(!vt_init_size(&decoded,80,24));
    tabs[0].used=tabs[0].floating=1;tabs[0].root=0;tabs[0].focus=1;strcpy(tabs[0].name,"desktop");
    for(i=0;i<3;i++)assert(new_node()==i);
    nodes[0].a=1;nodes[0].b=2;nodes[1].parent=nodes[2].parent=0;
    for(i=0;i<2;i++){
        struct pane *p=&panes[i];p->used=1;p->fd=-1;p->pid=100+i;p->node=i+1;p->z=i;
        p->fx=i?15:2;p->fy=i?4:2;p->fw=i?24:50;p->fh=i?10:14;
        nodes[i+1].pane=i;assert(!vt_init_size(&p->vt,1,1));strcpy(p->title,i?"front":"back");
    }
    layout();solid(0,'A');solid(1,'B');frame(&decoded);
    assert(cell(20,6)=='B'&&cell(5,6)=='A');
    assert(screen[cols-1].colour==0x8f&&screen[(rows-1)*cols].colour==0x87);
    assert(screen[(panes[1].y-1)*cols+panes[1].x].colour==0x74);
    assert(screen[panes[1].y*cols+panes[1].x-1].colour==0x0f);
    assert(screen[(panes[1].y+panes[1].h)*cols+panes[1].x].colour==0x0f);
    vt_feed(&panes[1].vt,"\033[?2026h",8);solid(1,'C');dirty=1;
    assert(frame(&decoded)<100&&cell(20,6)=='B');
    floating_set(&panes[1],17,5,24,10);layout();bytes=frame(&decoded);
    assert(cell(20,7)=='B'&&cell(16,6)=='A'&&bytes<2400);
    vt_feed(&panes[1].vt,"\033[?2026l",8);dirty=1;frame(&decoded);assert(cell(20,7)=='C');
    solid(0,'D');frame(&decoded);assert(cell(20,7)=='C'&&cell(5,7)=='D');
    /* Split a wide glyph at both sides of the upper window. */
    y=7-panes[0].y;
    vt_cells(&panes[0].vt)[y*panes[0].w+16-panes[0].x].ch=0x754c;
    vt_cells(&panes[0].vt)[y*panes[0].w+17-panes[0].x].ch=RU_CONTINUATION;
    vt_cells(&panes[0].vt)[y*panes[0].w+40-panes[0].x].ch=0x754c;
    vt_cells(&panes[0].vt)[y*panes[0].w+41-panes[0].x].ch=RU_CONTINUATION;
    vt_dirty(&panes[0].vt);frame(&decoded);
    assert(cell(16,7)==' '&&cell(17,7)==0x2502&&cell(40,7)==0x2502&&cell(41,7)==' ');
    packet(0,4,rows-2,0);packet(0,4,rows-2,1);frame(&decoded);
    assert(tabs[0].focus==0&&panes[0].z==1&&cell(20,7)=='D');
    packet(0,8,2,0);packet(32,11,4,0);packet(0,11,4,1);
    assert(panes[0].fx==5&&panes[0].fy==4&&mouse_window==-1);
    packet(0,54,17,0);packet(32,58,19,0);packet(0,58,19,1);
    assert(panes[0].fw==54&&panes[0].fh==16);
    input("\002m");assert(mode==6);input("l\033[B\r");layout();assert(!mode&&panes[0].fx==6&&panes[0].fy==5);
    input("\002vlj\r");layout();assert(!mode&&panes[0].fw==55&&panes[0].fh==17);
    input("\002t\025My window\r");assert(!strcmp(panes[0].title,"My window"));
    packet(0,10,5,0);assert(mouse_window==0);panes[0].pid++;packet(32,12,6,0);assert(mouse_window==-1&&panes[0].fx==6);
    /* A content drag remains captured by its child across another window. */
    packet(0,60,rows-2,0);assert(tabs[0].focus==1);
    vt_feed(&panes[1].vt,"\033[?1006h\033[?1002h",16);panes[1].queued=0;
    packet(0,20,7,0);packet(32,2,2,0);packet(0,2,2,1);
    assert(panes[1].queued&&!panes[0].queued&&mouse_pane==-1);
    x=panes[0].fx;y=panes[0].fy;w=panes[0].fw;h=panes[0].fh;
    assert(!screen_size(10,5));layout();assert(!focused()->framed&&focused()->w==10);
    assert(!screen_size(80,24));layout();assert(panes[0].fx==x&&panes[0].fy==y&&panes[0].fw==w&&panes[0].fh==h);
    /* Hide without killing/restarting the PTY, keep stable task slots and dim
     * text, then restore by click or keyboard even when all windows are hidden. */
    raise_pane(1);layout();frame(&decoded);assert(!panes[1].minimized);
    packet(0,panes[1].x+panes[1].w-8,panes[1].y-1,0);packet(0,0,0,1);layout();frame(&decoded);
    assert(panes[1].minimized&&!panes[1].shown&&tabs[0].focus==0);
    assert(screen[(rows-2)*cols+60].colour==0x87&&desktop_task(60)==1);
    input("\002M");layout();frame(&decoded);assert(panes[0].minimized&&!panes[0].shown&&!panes[1].shown);
    panes[0].queued=panes[1].queued=0;input("NO_HIDDEN_INPUT\033[200~PASTE\033[201~");
    assert(!panes[0].queued&&!panes[1].queued);
    for(y=1;y<rows-2;y++)for(x=0;x<cols;x++)assert(cell(x,y)==' ');
    packet(0,60,rows-2,0);layout();frame(&decoded);assert(!panes[1].minimized&&panes[1].shown&&panes[0].minimized);
    input("\002o");layout();frame(&decoded);assert(!panes[0].minimized&&panes[0].shown);
    for(i=0;i<(int)(cols*rows);i++)assert(((screen[i].colour>>4)&15)!=1);
    /* Automatic foreground titles decode quoted paths; manual names persist. */
    panes[0].title_override=0;program_title(&panes[0],"'/bin/ri'\\''ft' 'file'");assert(!strcmp(panes[0].title,"ri'ft"));
    program_title(&panes[0],"");assert(!strcmp(panes[0].title,"shell"));
    input("\002t\025Mine\r");program_title(&panes[0],"'rift'");assert(!strcmp(panes[0].title,"Mine"));
    input("\002t\025\r");program_title(&panes[0],"'rift'");assert(!strcmp(panes[0].title,"rift")&&!panes[0].title_override);
    /* The modal owns mouse/keyboard input and bracketed paste does not launch. */
    input("\002e");frame(&decoded);assert(mode==9);
    input("\r");frame(&decoded);assert(mode==9&&strstr(message,"Enter a program"));
    {unsigned left,top,width;assert(run_box(&left,&top,&width));assert(cell(left,top)==0x250c);
     packet(0,2,0,0);assert(mode==9&&current==0);
     input("\033[200~echo Ж\n\033[201~");assert(mode==9&&!strcmp(prompt,"echo Ж"));
     input("\177");assert(!strcmp(prompt,"echo "));
     packet(0,left+12,top+5,0);packet(0,left+12,top+5,1);assert(!mode);}
    frame(&decoded);
    /* Closing the top window selects the remaining one without stale z slots. */
    raise_pane(1);layout();panes[1].pid=panes[1].session=0;
    packet(0,panes[1].x+panes[1].w-2,panes[1].y-1,0);packet(0,0,0,1);
    assert(!panes[1].used&&!closing&&!mouse_decoration&&tabs[0].used&&tabs[0].focus==0&&panes[0].z==0);
    x=panes[0].fx;y=panes[0].fy;
    packet(0,x+3,y,0);packet(32,x+2,y,0);packet(0,x+2,y,1);
    assert(panes[0].fx==x-1&&!closing&&mouse_window==-1);
    w=panes[0].fw;h=panes[0].fh;x=panes[0].fx;y=panes[0].fy;
    packet(0,x+w-1,y+h-1,0);packet(32,x+w-2,y+h-2,0);packet(0,x+w-2,y+h-2,1);
    assert(panes[0].fw==w-1&&panes[0].fh==h-1&&!closing&&mouse_window==-1);
    input("\002x");assert(closing);input("n");assert(!closing&&panes[0].used);
    free(panes[0].committed);vt_destroy(&panes[0].vt);vt_destroy(&decoded);free(screen);free(painted);
    puts("MUX_DESKTOP_OK: overlap, retained diff, synchronized frames, Unicode occlusion, dock, move/resize, keyboard, capture, tiny resize and close");
    return 0;
}
