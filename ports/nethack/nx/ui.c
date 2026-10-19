/* NetHack Nano-X display. See ../upstream/dat/license. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nano-X.h"
#include "fde_nano.h"
#include "ui.h"

#define TILE 16
#define ATLAS_COLS 40
#define MAP_Y 80
#define BG GR_RGB(18,28,31)
#define PANEL GR_RGB(26,39,42)
#define FG GR_RGB(218,227,218)
#define MUTED GR_RGB(154,179,173)
#define ACCENT GR_RGB(234,193,110)
static GR_WINDOW_ID window, atlas;
static GR_GC_ID gc;
static int connected, width, height, tile_count;
static int map[21][80], pets[21][80], dirty[21][80];
static int full = 1, messages_dirty = 1, status_dirty = 1, prompt_dirty = 1;
static int vx = 1, vy, cols, rows, fx = 1, fy;
static char messages[3][256], status[2][512], prompt[512];
static const char **overlay;
static const char *overlay_title, *overlay_footer;
static int overlay_count, overlay_top, overlay_focus, overlay_dirty;

static void box(int x,int y,int w,int h,GR_COLOR color)
{ if(w>0&&h>0){GrSetGCForeground(gc,color);GrFillRect(window,gc,x,y,w,h);} }
static void text(int x,int y,const char *s,int limit,GR_COLOR color)
{
    int n=strlen(s);if(n>limit)n=limit;
    if(n>0){GrSetGCForeground(gc,color);GrText(window,gc,x,y,s,n,GR_TFASCII|GR_TFTOP);}
}
int nhnx_columns(void) { int n=(width-32)/8;return n<1?1:n>240?240:n; }
int nhnx_rows(void) { int n=(height-100)/16;return n<1?1:n; }
static void layout(void)
{
    int oldx=vx,oldy=vy;
    cols=(width-24)/TILE;if(cols<1)cols=1;if(cols>79)cols=79;
    rows=(height-88-MAP_Y)/TILE;if(rows<1)rows=1;if(rows>21)rows=21;
    if(fx<vx+2||fx>=vx+cols-2)vx=fx-cols/2;
    if(fy<vy+2||fy>=vy+rows-2)vy=fy-rows/2;
    if(vx<1)vx=1;if(vx>80-cols)vx=80-cols;
    if(vy<0)vy=0;if(vy>21-rows)vy=21-rows;
    if(oldx!=vx||oldy!=vy)full=1;
}
static int octet(FILE *f) { int c=fgetc(f);return c==EOF?-1:c&255; }
static int load_tiles(const char *path)
{
    FILE *f=fopen(path,"rb");char magic[6];int i,j,x,y,lo,hi,count,w,result=-1;
    unsigned short *pixels=0;FDE_IMAGE image=0;struct fde_rect rect;
    if(!f)return -1;
    if(fread(magic,1,5,f)!=5||memcmp(magic,"NXT1\n",5)){fclose(f);return -1;}
    lo=octet(f);hi=octet(f);tile_count=lo+(hi<<8);
    if(lo<0||hi<0||tile_count!=1082){fclose(f);return -1;}
    atlas=GrNewPixmapEx(ATLAS_COLS*TILE,((tile_count+ATLAS_COLS-1)/ATLAS_COLS)*TILE,MWIF_RGB565,NULL);
    if(!atlas){fclose(f);return -1;}
    pixels=malloc(20UL*256*sizeof(*pixels));if(!pixels)goto done;
    for(i=0;i<tile_count;i+=count){
        count=tile_count-i;if(count>20)count=20;w=count*TILE;
        for(j=0;j<count;j++)for(y=0;y<TILE;y++)for(x=0;x<TILE;x++){
            lo=octet(f);hi=octet(f);if(lo<0||hi<0)goto done;
            pixels[y*w+j*TILE+x]=(unsigned)lo|((unsigned)hi<<8);
        }
        /* The existing bounded RGB565 image protocol populates the pixmap
         * in half-row strips, with only one small temporary resource live. */
        image=fde_image_load(w,TILE,pixels,NULL);if(!image)goto done;
        rect.x=(i%ATLAS_COLS)*TILE;rect.y=(i/ATLAS_COLS)*TILE;
        rect.sx=rect.sy=0;rect.sw=rect.width=w;rect.sh=rect.height=TILE;
        if(fde_image_draw(image,atlas,gc,&rect,1,0)<0)goto done;
        if(fde_image_destroy(image)<0)goto done;image=0;
        if(getenv("NETHACK_TRACE")&&i%200==0)fprintf(stderr,"NHNX_ATLAS %d/%d\n",i+count,tile_count);
    }
    result=octet(f)<0?0:-1;
done:
    if(image)fde_image_destroy(image);free(pixels);fclose(f);return result;
}
int nhnx_open(const char *tiles)
{
    GR_SCREEN_INFO screen;
    if(GrOpen()<0)return -1;
    connected=1;GrGetScreenInfo(&screen);
    width=screen.cols-24;height=screen.rows-60;
    if(width>1100)width=1100;if(height>700)height=700;
    if(width<480)width=480;if(height<320)height=320;
    window=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"NetHack",GR_ROOT_WINDOW_ID,8,8,width,height,BG);
    gc=GrNewGC();
    if(!window||!gc||load_tiles(tiles)<0){nhnx_close();return -1;}
    GrSetGCUseBackground(gc,GR_FALSE);
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN|
        GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_UPDATE);
    nhnx_clear_map();layout();GrMapWindow(window);GrSetFocus(window);nhnx_paint();
    return 0;
}
void nhnx_close(void)
{
    if(!connected)return;
    if(atlas)GrDestroyWindow(atlas);
    if(window)GrDestroyWindow(window);
    if(gc)GrDestroyGC(gc);
    GrClose();connected=0;atlas=window=gc=0;
}
void nhnx_clear_map(void)
{
    int x,y;for(y=0;y<21;y++)for(x=0;x<80;x++){map[y][x]=-1;pets[y][x]=0;dirty[y][x]=1;}
    full=1;
}
void nhnx_tile(int x,int y,int tile,int pet)
{
    if(x<1||x>=80||y<0||y>=21)return;
    if(tile!=map[y][x]||pet!=pets[y][x]){map[y][x]=tile;pets[y][x]=pet;dirty[y][x]=1;}
}
void nhnx_focus(int x,int y)
{
    if(x<1||x>=80||y<0||y>=21)return;
    dirty[fy][fx]=1;fx=x;fy=y;dirty[fy][fx]=1;layout();
}
static void copy_text(char *out,const char *in,unsigned size)
{ if(!in)in="";strncpy(out,in,size-1);out[size-1]=0; }
void nhnx_message(const char *s)
{ memcpy(messages[0],messages[1],256);memcpy(messages[1],messages[2],256);copy_text(messages[2],s,256);messages_dirty=1; }
void nhnx_clear_messages(void) { memset(messages,0,sizeof(messages));messages_dirty=1; }
void nhnx_status(int line,const char *s)
{ if(line>=0&&line<2){copy_text(status[line],s,512);status_dirty=1;} }
void nhnx_prompt(const char *s) { copy_text(prompt,s,512);prompt_dirty=1;overlay_dirty=1; }
void nhnx_overlay(const char *title,const char **lines,int count,int top,int focus,const char *footer)
{
    overlay=lines;overlay_title=title;overlay_count=count;overlay_top=top;
    overlay_focus=focus;overlay_footer=footer;overlay_dirty=1;
    if(!lines)full=1;
}
static void paint_messages(void)
{
    char lines[3][256];int i,n,used=0,take,limit=nhnx_columns();const char *s;
    memset(lines,0,sizeof(lines));
    for(i=0;i<3;i++){
        s=messages[i];while(*s){
            n=strlen(s);take=n>limit?limit:n;
            if(n>take){int end=take;while(end>take/2&&s[end]!=' ')end--;if(end>take/2)take=end;}
            if(used==3){memcpy(lines[0],lines[1],256);memcpy(lines[1],lines[2],256);used=2;}
            memcpy(lines[used],s,take);lines[used][take]=0;used++;s+=take;while(*s==' ')s++;
        }
    }
    box(0,24,width,52,PANEL);
    for(i=0;i<used;i++)text(12,26+i*16,lines[i],limit,FG);
}
static void paint_status(void)
{
    int i,part,n,limit=nhnx_columns();
    box(0,height-88,width,64,PANEL);
    for(i=0;i<2;i++){
        n=strlen(status[i]);
        for(part=0;part<2&&part*limit<n;part++)text(12,height-86+i*32+part*16,status[i]+part*limit,limit,FG);
    }
}
void nhnx_paint(void)
{
    int x,y,px,py,tile,i,limit=nhnx_columns();char position[70];
    if(!connected)return;
    if(overlay){
        if(!full&&!overlay_dirty)return;
        box(0,0,width,height,BG);box(0,0,width,36,PANEL);
        text(16,10,overlay_title?overlay_title:"NetHack",limit,ACCENT);
        for(i=0;i<nhnx_rows()&&overlay_top+i<overlay_count;i++){
            if(overlay_top+i==overlay_focus)box(8,44+i*16,width-16,16,GR_RGB(48,76,77));
            text(16,44+i*16,overlay[overlay_top+i],limit,FG);
        }
        box(0,height-40,width,40,PANEL);
        text(16,height-36,*prompt?prompt:overlay_footer?overlay_footer:"",limit,*prompt?ACCENT:MUTED);
        snprintf(position,sizeof(position),"%d-%d / %d",overlay_count?overlay_top+1:0,
            overlay_top+nhnx_rows()<overlay_count?overlay_top+nhnx_rows():overlay_count,overlay_count);
        text(16,height-20,position,limit,MUTED);
        overlay_dirty=full=0;GrFlush();return;
    }
    if(full){
        box(0,0,width,height,BG);box(0,0,width,24,PANEL);
        text(12,4,"NETHACK",limit,ACCENT);
        text(100,4,"Arrows: move   i: inventory   ?: help   S: save",limit-11,MUTED);
    }
    if(full||messages_dirty)paint_messages();
    for(y=vy;y<vy+rows;y++)for(x=vx;x<vx+cols;x++)if(full||dirty[y][x]){
        px=12+(x-vx)*TILE;py=MAP_Y+(y-vy)*TILE;tile=map[y][x];
        if(tile>=0&&tile<tile_count)GrCopyArea(window,gc,px,py,TILE,TILE,atlas,(tile%ATLAS_COLS)*TILE,(tile/ATLAS_COLS)*TILE,MWROP_COPY);
        else box(px,py,TILE,TILE,GR_RGB(0,0,0));
        if(pets[y][x]){GrSetGCForeground(gc,GR_RGB(113,201,190));GrRect(window,gc,px,py,TILE,TILE);}
        if(x==fx&&y==fy){GrSetGCForeground(gc,ACCENT);GrRect(window,gc,px+1,py+1,TILE-2,TILE-2);}
        dirty[y][x]=0;
    }
    if(full||status_dirty)paint_status();
    if(full||prompt_dirty){box(0,height-24,width,24,BG);text(12,height-20,*prompt?prompt:"h j k l / y u b n: move   .: wait   Ctrl-P: messages",limit,*prompt?ACCENT:MUTED);}
    full=messages_dirty=status_dirty=prompt_dirty=0;GrFlush();
}
void nhnx_input(struct nx_input *input)
{
    GR_EVENT e;int k;memset(input,0,sizeof(*input));
    for(;;){
        nhnx_paint();GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_EXPOSURE){full=1;continue;}
        if(e.type==GR_EVENT_TYPE_CLOSE_REQ){input->key=NX_CLOSE;return;}
        if(e.type==GR_EVENT_TYPE_UPDATE&&e.update.utype==GR_UPDATE_SIZE){
            width=e.update.width;height=e.update.height;
            if(width<320||height<240){if(width<320)width=320;if(height<240)height=240;GrResizeWindow(window,width,height);}
            layout();full=1;input->key=NX_RESIZE;return;
        }
        if(e.type==GR_EVENT_TYPE_BUTTON_DOWN&&(e.button.changebuttons&GR_BUTTON_L)){
            input->key=NX_CLICK;input->row=-1;
            if(overlay){if(e.button.y>=44&&e.button.y<height-40)input->row=overlay_top+(e.button.y-44)/16;}
            else if(e.button.x>=12&&e.button.x<12+cols*TILE&&e.button.y>=MAP_Y&&e.button.y<MAP_Y+rows*TILE){
                input->x=vx+(e.button.x-12)/TILE;input->y=vy+(e.button.y-MAP_Y)/TILE;
            }
            return;
        }
        if(e.type!=GR_EVENT_TYPE_KEY_DOWN)continue;
        k=e.keystroke.ch;
        switch(k){
        case MWKEY_UP:k=NX_UP;break;case MWKEY_DOWN:k=NX_DOWN;break;
        case MWKEY_LEFT:k=NX_LEFT;break;case MWKEY_RIGHT:k=NX_RIGHT;break;
        case MWKEY_HOME:k=NX_HOME;break;case MWKEY_END:k=NX_END;break;
        case MWKEY_PAGEUP:k=NX_PAGEUP;break;case MWKEY_PAGEDOWN:k=NX_PAGEDOWN;break;
        case MWKEY_F1:k='?';break;
        default:
            if(k>255)continue;
            if(e.keystroke.modifiers&MWKMOD_CTRL){if((k>='a'&&k<='z')||(k>='@'&&k<='_'))k&=31;else if(k==' ')k=0;}
            else if((e.keystroke.modifiers&MWKMOD_ALT)&&k>=32&&k<127)k|=128;
        }
        input->key=k;return;
    }
}
