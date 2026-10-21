/* SPDX-License-Identifier: MIT */
/* Shared Nano-X frontend for Simon Tatham's Portable Puzzle Collection. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "nano-X.h"
#include "fde_nano.h"
#include "puzzles.h"
#include "puzzle_catalog.h"
#include "../graphics/fonts/rom8x16.inc"

#define BG GR_RGB(26,33,39)
#define PANEL GR_RGB(35,44,53)
#define FG GR_RGB(226,233,235)
#define MUTED GR_RGB(151,170,180)
#define GOLD GR_RGB(235,193,110)
#define SELECTED GR_RGB(51,72,84)
#define TOP 82
#define FOOT 46

struct frontend {
    midend *me;
    GR_WINDOW_ID win, canvas;
    GR_GC_ID gc, copygc;
    GR_REGION_ID clip;
    GR_TIMER_ID timer;
    FDE_IMAGE font;
    GR_COLOR *colours;
    int ncolours, w, h, pw, ph, bx, by;
    int full, hud, dirty, x0, y0, x1, y1, active_timer, savedirty;
    int mode, choice, first, count, cfgrow, cfgcount, cfgwhich;
    int save_blocked, io_failed, trace, held, exit_code;
    unsigned long lasttick, saveat, frames;
    char status[256], message[256], path[512], entry[2048];
    struct preset_menu_entry **presets;
    const struct puzzle_info *info;
    config_item *cfg;
    char *cfgtitle;
};
struct blitter { GR_WINDOW_ID pix; int w,h,sx,sy,cw,ch; };
static frontend fe;
static int wakefd[2] = {-1,-1};
static volatile int stopping;
static const char *buttons[] = {"New", "Type", "Undo", "Redo", "Restart", "Solve", "Game ID", "Help"};

static unsigned long now_ms(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)fatal("cannot read monotonic clock");
    return (unsigned long)t.tv_sec*1000UL+(unsigned long)t.tv_nsec/1000000UL;
}
static void caught(int sig)
{
    int old=errno;char c=1;signal(sig,caught);stopping=1;
    if(wakefd[1]>=0)write(wakefd[1],&c,1);errno=old;
}
void activate_timer(frontend *f) { f->active_timer=1; }
void deactivate_timer(frontend *f) { f->active_timer=0; }
static void timer_sync(frontend *f)
{
    int needed=f->active_timer||(f->savedirty&&!f->save_blocked);
    if(needed&&!f->timer){
        f->lasttick=now_ms();f->timer=GrCreateTimer(f->win,100);
        if(!f->timer)fatal("cannot create game timer");
    }else if(!needed&&f->timer){GrDestroyTimer(f->timer);f->timer=0;}
}
static void box(GR_WINDOW_ID target,int x,int y,int w,int h,GR_COLOR c)
{
    if(w>0&&h>0){GrSetGCForeground(fe.gc,c);GrFillRect(target,fe.gc,x,y,w,h);}
}
static void ui_text(int x,int y,const char *s,GR_COLOR c)
{
    int n=strlen(s),limit=(fe.w-x-12)/8;
    if(n>limit)n=limit;
    if(n>0){GrSetGCFont(fe.gc,0);GrSetGCForeground(fe.gc,c);GrText(fe.win,fe.gc,x,y,s,n,GR_TFASCII|GR_TFTOP);}
}
static void message(const char *s)
{
    snprintf(fe.message,sizeof(fe.message),"%s",s);fe.hud=1;
}
static void error(const char *s) { message(s);fprintf(stderr,"%s: %s\n",thegame.htmlhelp_topic,s); }
static void colour(frontend *f,int c)
{
    if(c<0||c>=f->ncolours)fatal("invalid drawing colour %d",c);
    GrSetGCForeground(f->gc,f->colours[c]);
}
static void p_rect(drawing *dr,int x,int y,int w,int h,int c)
{
    frontend *f=dr->handle;
    if(w>0&&h>0){colour(f,c);GrFillRect(f->canvas,f->gc,x,y,w,h);}
}
static void p_line(drawing *dr,int x1,int y1,int x2,int y2,int c)
{ frontend *f=dr->handle;colour(f,c);GrLine(f->canvas,f->gc,x1,y1,x2,y2); }
static void p_text(drawing *dr,int x,int y,int type,int size,int align,int c,const char *s)
{
    frontend *f=dr->handle;int i,n=0,len=strlen(s),cw;struct fde_rect rects[9];
    (void)type;
    if(size<6)size=6;if(size>64)size=64;
    cw=(size+1)/2;
    if(align&ALIGN_HCENTRE)x-=cw*len/2;else if(align&ALIGN_HRIGHT)x-=cw*len;
    if(align&ALIGN_VCENTRE)y-=size/2;else y-=size*3/4;
    colour(f,c);
    for(i=0;i<len;i++){
        struct fde_rect *r=rects+n++;unsigned ch=(unsigned char)s[i];if(ch>127)ch='?';
        r->x=x+i*cw;r->y=y;r->sx=(ch%16)*8;r->sy=(ch/16)*16;r->sw=8;r->sh=16;r->width=cw;r->height=size;
        /* The image protocol accepts at most nine rectangles per request. */
        if(n==9||i+1==len){if(fde_image_draw(f->font,f->canvas,f->gc,rects,n,FDE_IMAGE_TINT)<0)fatal("cannot paint puzzle text (status %u)",fde_image_status());n=0;}
    }
}
static void font_upload(void)
{
    unsigned short mask[1024];int c,y;
    for(y=0;y<16;y++)for(c=0;c<128;c+=2)mask[((c/16)*16+y)*8+(c%16)/2]=gx_font[c*16+y]|(gx_font[(c+1)*16+y]>>8);
    fe.font=fde_image_load(128,128,NULL,mask);if(!fe.font)fatal("cannot upload puzzle font atlas");
}
static void p_circle(drawing *dr,int cx,int cy,int radius,int fill,int outline)
{
    /* Midpoint rasterisation: horizontal spans, never one request per pixel. */
    int x=radius-1,y=0,e=1-x;
    if(radius<1)return;
    while(x>=y){
        if(fill>=0){
            p_line(dr,cx-x,cy+y,cx+x,cy+y,fill);p_line(dr,cx-x,cy-y,cx+x,cy-y,fill);
            p_line(dr,cx-y,cy+x,cx+y,cy+x,fill);p_line(dr,cx-y,cy-x,cx+y,cy-x,fill);
        }
        /* Fill all spans first; outlines are drawn in a separate pass. */
        y++;if(e<0)e+=2*y+1;else{x--;e+=2*(y-x)+1;}
    }
    x=radius-1;y=0;e=1-x;
    while(x>=y){
        p_line(dr,cx+x,cy+y,cx+x,cy+y,outline);p_line(dr,cx-x,cy+y,cx-x,cy+y,outline);
        p_line(dr,cx+x,cy-y,cx+x,cy-y,outline);p_line(dr,cx-x,cy-y,cx-x,cy-y,outline);
        p_line(dr,cx+y,cy+x,cx+y,cy+x,outline);p_line(dr,cx-y,cy+x,cx-y,cy+x,outline);
        p_line(dr,cx+y,cy-x,cx+y,cy-x,outline);p_line(dr,cx-y,cy-x,cx-y,cy-x,outline);
        y++;if(e<0)e+=2*y+1;else{x--;e+=2*(y-x)+1;}
    }
}
static void p_update(drawing *dr,int x,int y,int w,int h)
{
    frontend *f=dr->handle;
    if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
    if(x+w>f->pw)w=f->pw-x;if(y+h>f->ph)h=f->ph-y;
    if(w<=0||h<=0)return;
    if(!f->dirty){f->x0=x;f->y0=y;f->x1=x+w;f->y1=y+h;}
    else {if(x<f->x0)f->x0=x;if(y<f->y0)f->y0=y;if(x+w>f->x1)f->x1=x+w;if(y+h>f->y1)f->y1=y+h;}
    f->dirty=1;
}
static void p_unclip(drawing *dr)
{
    frontend *f=dr->handle;GrSetGCRegion(f->gc,0);
    if(f->clip){GrDestroyRegion(f->clip);f->clip=0;}
}
static void p_clip(drawing *dr,int x,int y,int w,int h)
{
    frontend *f=dr->handle;GR_RECT r;GR_REGION_ID region;
    p_unclip(dr);
    r.x=x;r.y=y;r.width=w;r.height=h;region=GrNewRegion();
    if(!region)fatal("cannot create clip region");
    /* Nano-X stores the region ID, not a copy. Keep it alive until unclip. */
    GrUnionRectWithRegion(region,&r);GrSetGCRegion(f->gc,region);f->clip=region;
}
static void p_start(drawing *dr) { p_unclip(dr); }
static void p_end(drawing *dr) { p_unclip(dr); }
static void p_status(drawing *dr,const char *s)
{
    frontend *f=dr->handle;snprintf(f->status,sizeof(f->status),"%s",s);f->hud=1;
}
static blitter *p_blitter_new(drawing *dr,int w,int h)
{
    blitter *b=snew(blitter);(void)dr;memset(b,0,sizeof(*b));b->w=w;b->h=h;
    b->pix=GrNewPixmapEx(w,h,MWIF_RGB565,NULL);if(!b->pix)fatal("cannot allocate blitter");return b;
}
static void p_blitter_free(drawing *dr,blitter *b) { (void)dr;GrDestroyWindow(b->pix);sfree(b); }
static void p_blitter_save(drawing *dr,blitter *b,int x,int y)
{
    frontend *f=dr->handle;b->sx=x<0?-x:0;b->sy=y<0?-y:0;
    b->cw=min(b->w-b->sx,f->pw-max(x,0));b->ch=min(b->h-b->sy,f->ph-max(y,0));
    /* Saving a snapshot is independent of the game's current drawing clip. */
    if(b->cw>0&&b->ch>0)GrCopyArea(b->pix,f->copygc,b->sx,b->sy,b->cw,b->ch,f->canvas,max(x,0),max(y,0),MWROP_COPY);
}
static void p_blitter_load(drawing *dr,blitter *b,int x,int y)
{
    frontend *f=dr->handle;
    if(b->cw>0&&b->ch>0)GrCopyArea(f->canvas,f->gc,x+b->sx,y+b->sy,b->cw,b->ch,b->pix,b->sx,b->sy,MWROP_COPY);
}
static const drawing_api api={
    .version=1,.draw_text=p_text,.draw_rect=p_rect,.draw_line=p_line,
    .draw_polygon=draw_polygon_fallback,.draw_circle=p_circle,.draw_update=p_update,
    .clip=p_clip,.unclip=p_unclip,.start_draw=p_start,.end_draw=p_end,.status_bar=p_status,
    .blitter_new=p_blitter_new,.blitter_free=p_blitter_free,.blitter_save=p_blitter_save,.blitter_load=p_blitter_load
};

static bool file_read(void *ctx,void *buf,int len) { return fread(buf,1,len,ctx)==(size_t)len; }
static void file_write(void *ctx,const void *buf,int len)
{ if(fwrite(buf,1,len,ctx)!=(size_t)len)fe.io_failed=1; }
static int save(void)
{
    FILE *f;char temp[544];int fail=0;
    if(fe.save_blocked||!fe.savedirty)return !fe.save_blocked;
    snprintf(temp,sizeof(temp),"%s.tmp-%d",fe.path,(int)getpid());f=fopen(temp,"w");
    if(!f){error("Cannot save progress (S retries). Current game remains in memory.");return 0;}
    fe.io_failed=0;midend_serialise(fe.me,file_write,f);
    if(fe.io_failed||fflush(f))fail=1;
    /* The guest exposes a synchronous filesystem-wide flush. */
    if(!fail)sync();
    if(fclose(f))fail=1;
    if(!fail&&rename(temp,fe.path)==0){
        sync();
        fe.savedirty=0;
        if(fe.trace)fprintf(stderr,"PUZZLES_SAVE %s\n",thegame.htmlhelp_topic);
        return 1;
    }
    unlink(temp);error("Cannot save progress (S retries). Previous save kept.");return 0;
}
static void modified(void)
{
    fe.savedirty=1;fe.saveat=now_ms()+700;
    if(fe.trace)fprintf(stderr,"PUZZLES_CHANGE %s undo=%d redo=%d status=%d\n",thegame.htmlhelp_topic,midend_can_undo(fe.me),midend_can_redo(fe.me),midend_status(fe.me));
}
static void flatten(struct preset_menu *m)
{
    int i;for(i=0;i<m->n_entries;i++){
        if(m->entries[i].submenu)flatten(m->entries[i].submenu);
        else if(!thegame.validate_params(m->entries[i].params,true)){
            fe.presets=sresize(fe.presets,fe.count+1,struct preset_menu_entry *);
            fe.presets[fe.count++]=m->entries+i;
        }
    }
}
static void colours(void)
{
    int i;float *rgb=midend_colours(fe.me,&fe.ncolours);sfree(fe.colours);fe.colours=snewn(fe.ncolours,GR_COLOR);
    for(i=0;i<fe.ncolours;i++){
        int r=rgb[i*3]*255,g=rgb[i*3+1]*255,b=rgb[i*3+2]*255;
        fe.colours[i]=GR_RGB(max(0,min(r,255)),max(0,min(g,255)),max(0,min(b,255)));
    }
    sfree(rgb);
}
static void layout(void)
{
    GR_WINDOW_ID next;int w=fe.w-24,h=fe.h-TOP-FOOT-16;
    midend_size(fe.me,&w,&h,true,1.0);
    next=GrNewPixmapEx(w,h,MWIF_RGB565,NULL);if(!next)fatal("cannot allocate puzzle canvas %dx%d",w,h);
    if(fe.canvas)GrDestroyWindow(fe.canvas);fe.canvas=next;fe.pw=w;fe.ph=h;
    fe.bx=(fe.w-w)/2;fe.by=TOP+(fe.h-TOP-FOOT-h)/2;
    box(fe.canvas,0,0,w,h,fe.colours[0]);midend_force_redraw(fe.me);fe.full=1;
    if(fe.trace)fprintf(stderr,"PUZZLES_LAYOUT %s %d %d %d %d %d\n",thegame.htmlhelp_topic,fe.bx,fe.by,w,h,midend_tilesize(fe.me));
}
static void title(void)
{
    GR_WM_PROPERTIES p;char text[200];game_params *params=midend_get_params(fe.me);char *s=thegame.encode_params(params,true);
    snprintf(text,sizeof(text),"%s / %s",thegame.name,s);sfree(s);thegame.free_params(params);
    memset(&p,0,sizeof(p));p.flags=GR_WM_FLAGS_TITLE;p.title=text;GrSetWMProperties(fe.win,&p);
}
static void new_game(void)
{
    fe.save_blocked=0;fe.held=0;message("Generating puzzle...");
    box(fe.win,0,fe.h-FOOT,fe.w,FOOT,PANEL);ui_text(12,fe.h-FOOT+6,fe.message,GOLD);GrFlush();
    fe.status[0]=0;midend_new_game(fe.me);colours();layout();title();fe.message[0]=0;fe.hud=1;
    modified();save();
}
static int list_rows(void) { return max(1,(fe.h-148)/27); }
static void close_mode(void)
{
    if(fe.cfg){free_cfg(fe.cfg);fe.cfg=NULL;sfree(fe.cfgtitle);fe.cfgtitle=NULL;}
    fe.mode=0;fe.full=1;fe.held=0;
}
static void config_open(int which)
{
    close_mode();fe.cfgwhich=which;fe.cfg=midend_get_config(fe.me,which,&fe.cfgtitle);fe.cfgcount=0;fe.cfgrow=0;fe.first=0;
    for(;fe.cfg[fe.cfgcount].type!=C_END;fe.cfgcount++){}
    fe.mode=3;fe.full=1;
}
static void choice_text(const char *s,int index,char *out,int cap)
{
    char sep=*s++;int i=0,n=0;while(i<index&&*s){if(*s++==sep)i++;}
    while(*s&&*s!=sep&&n<cap-1)out[n++]=*s++;out[n]=0;
}
static const char *hint(void)
{
    return fe.info->controls;
}
static int paragraph(int y,const char *s,GR_COLOR colour)
{
    int limit=(fe.w-40)/8,n;char buf[256];
    if(limit>250)limit=250;
    while(*s&&y<fe.h-FOOT-18){
        n=strlen(s);if(n>limit){n=limit;while(n>0&&s[n]!=' ')n--;if(!n)n=limit;}
        snprintf(buf,sizeof(buf),"%.*s",n,s);ui_text(20,y,buf,colour);y+=20;
        s+=n;while(*s==' ')s++;
    }
    return y;
}
static void modal(void)
{
    int i,n,y;char buf[256];box(fe.win,0,TOP,fe.w,fe.h-TOP-FOOT,BG);
    box(fe.win,fe.w-168,8,76,25,SELECTED);ui_text(fe.w-155,12,fe.mode==1?"Play":fe.mode==2?"Close":"Apply",GOLD);
    box(fe.win,fe.w-84,8,72,25,SELECTED);ui_text(fe.w-68,12,"Back",FG);
    if(fe.mode==1){
        if(fe.choice<fe.first)fe.first=fe.choice;if(fe.choice>=fe.first+list_rows())fe.first=fe.choice-list_rows()+1;
        ui_text(20,TOP+8,"Choose a game type",GOLD);
        for(i=0;i<list_rows()&&i+fe.first<fe.count+1;i++){
            n=i+fe.first;y=TOP+36+i*27;
            box(fe.win,14,y-3,fe.w-28,26,n==fe.choice?SELECTED:BG);
            snprintf(buf,sizeof(buf),"%s%s",n<fe.count?fe.presets[n]->title:"Custom...",n<fe.count&&fe.presets[n]->id==midend_which_preset(fe.me)?"  *":"");ui_text(22,y,buf,n==fe.choice?GOLD:FG);
        }
    }else if(fe.mode==2){
        y=paragraph(TOP+14,"Simon Tatham's Portable Puzzle Collection",GOLD);
        y=paragraph(y+12,fe.info->objective,FG);
        y=paragraph(y+12,hint(),FG);
        y=paragraph(y+12,"N: new. U / Ctrl-Z: undo. Y / Ctrl-Y: redo.",MUTED);
        y=paragraph(y,"F2: presets/custom. F3: game ID. F1: help.",MUTED);
        paragraph(y,"Progress and undo history save automatically.",MUTED);
    }else if(fe.mode==3){
        ui_text(20,TOP+8,fe.cfgtitle,GOLD);
        if(fe.cfgrow<fe.first)fe.first=fe.cfgrow;if(fe.cfgrow>=fe.first+list_rows())fe.first=fe.cfgrow-list_rows()+1;
        for(i=0;i<list_rows()&&i+fe.first<fe.cfgcount;i++){
            config_item *c=fe.cfg+(n=i+fe.first);y=TOP+38+i*27;
            box(fe.win,12,y-4,fe.w-24,26,n==fe.cfgrow?SELECTED:BG);
            ui_text(20,y,c->name,MUTED);
            if(c->type==C_STRING)snprintf(buf,sizeof(buf),"%s%s",c->u.string.sval,n==fe.cfgrow?"_":"");
            else if(c->type==C_BOOLEAN)strcpy(buf,c->u.boolean.bval?"Yes":"No");
            else choice_text(c->u.choices.choicenames,c->u.choices.selected,buf,sizeof(buf));
            box(fe.win,fe.w/2-8,y-3,fe.w/2-12,24,n==fe.cfgrow?SELECTED:BG);ui_text(fe.w/2,y,buf,n==fe.cfgrow?GOLD:FG);
        }
    }else if(fe.mode==4){
        ui_text(20,TOP+20,"Game ID / random seed",GOLD);
        ui_text(20,TOP+52,"Enter params:description or params#seed.",FG);
        ui_text(20,TOP+78,"The current ID is selected; typing replaces it.",MUTED);
        box(fe.win,16,TOP+114,fe.w-32,78,SELECTED);
        n=(fe.w-48)/8;for(i=0;i<3&&i*n<(int)strlen(fe.entry);i++){
            snprintf(buf,sizeof(buf),"%.*s",min(n,250),fe.entry+i*n);ui_text(24,TOP+122+i*20,buf,FG);
        }
    }else if(fe.mode==5||fe.mode==6){
        ui_text(24,TOP+35,fe.mode==5?"Restart this puzzle?":"Reveal the solution?",GOLD);
        ui_text(24,TOP+75,"Enter: confirm     Escape: cancel",FG);
        ui_text(24,TOP+110,"Undo can restore your previous position.",MUTED);
    }
    box(fe.win,0,fe.h-FOOT,fe.w,FOOT,PANEL);
    ui_text(12,fe.h-FOOT+5,fe.message[0]?fe.message:fe.mode==3?"Up/Down: field. Left/Right/Space: change. Type: edit.":"Arrows / mouse: select. Enter: accept. Esc: return.",GOLD);
    ui_text(12,fe.h-22,fe.mode==3||fe.mode==4?"Enter: apply     Esc: cancel":"F1: help     Progress is saved automatically.",MUTED);
}
static void paint(void)
{
    int i,bw=(fe.w-16)/8,changed=fe.full||fe.hud||fe.dirty;
    if(!changed)return;
    GrSetGCRegion(fe.gc,0);GrSetGCFont(fe.gc,0);
    if(fe.full){
        box(fe.win,0,0,fe.w,fe.h,BG);box(fe.win,0,0,fe.w,TOP,PANEL);
        ui_text(14,12,thegame.name,GOLD);ui_text(130,12,"PORTABLE PUZZLES",MUTED);
    }
    if(fe.full||fe.hud){
        for(i=0;i<8;i++){
            int enabled=!(i==2&&!midend_can_undo(fe.me)||i==3&&!midend_can_redo(fe.me)||i==5&&!thegame.can_solve);
            box(fe.win,8+i*bw,44,bw-3,27,SELECTED);ui_text(8+i*bw+(bw-3-(int)strlen(buttons[i])*8)/2,50,buttons[i],enabled?FG:MUTED);
        }
    }
    if(fe.mode)modal();
    else {
        if(fe.full){GrSetGCForeground(fe.gc,GOLD);GrRect(fe.win,fe.gc,fe.bx-1,fe.by-1,fe.pw+2,fe.ph+2);GrCopyArea(fe.win,fe.gc,fe.bx,fe.by,fe.pw,fe.ph,fe.canvas,0,0,MWROP_COPY);}
        else if(fe.dirty)GrCopyArea(fe.win,fe.gc,fe.bx+fe.x0,fe.by+fe.y0,fe.x1-fe.x0,fe.y1-fe.y0,fe.canvas,fe.x0,fe.y0,MWROP_COPY);
        if(fe.full||fe.hud){
            box(fe.win,0,fe.h-FOOT,fe.w,FOOT,PANEL);
            ui_text(12,fe.h-FOOT+5,fe.message[0]?fe.message:fe.status[0]?fe.status:midend_status(fe.me)>0?"Puzzle complete! New starts another game.":"Progress saves automatically.",GOLD);
            ui_text(12,fe.h-22,hint(),MUTED);
        }
    }
    fe.full=fe.hud=fe.dirty=0;GrFlush();fe.frames++;
    if(fe.trace)fprintf(stderr,"PUZZLES_PAINT %s %d %lu\n",thegame.htmlhelp_topic,fe.mode,fe.frames);
}
static void game_key(int x,int y,int key)
{
    int result=midend_process_key(fe.me,x,y,key);fe.hud=1;
    if(result==PKR_QUIT)stopping=1;
    else if(result==PKR_SOME_EFFECT)modified();
}
static void action(int n)
{
    int i;
    if(n==5&&!thegame.can_solve){message("This game has no automatic solver.");return;}
    if(fe.trace)fprintf(stderr,"PUZZLES_ACTION %s %d\n",thegame.htmlhelp_topic,n);
    if(n==7){close_mode();fe.mode=2;return;}
    if(n==1){close_mode();fe.mode=1;fe.choice=0;fe.first=0;for(i=0;i<fe.count;i++)if(fe.presets[i]->id==midend_which_preset(fe.me))fe.choice=i;return;}
    if(n==6){char *id=midend_get_game_id(fe.me);close_mode();snprintf(fe.entry,sizeof(fe.entry),"%s",id);sfree(id);fe.mode=4;fe.choice=1;return;}
    close_mode();fe.message[0]=0;
    if(n==0)new_game();
    else if(n==2||n==3){game_key(0,0,n==2?UI_UNDO:UI_REDO);title();}
    else if(n==4||n==5)fe.mode=n+1;
}
static void modal_accept(void)
{
    const char *err;
    if(fe.mode==1){
        if(fe.choice==fe.count){config_open(CFG_SETTINGS);return;}
        midend_set_params(fe.me,fe.presets[fe.choice]->params);close_mode();new_game();
    }else if(fe.mode==3){
        err=midend_set_config(fe.me,fe.cfgwhich,fe.cfg);if(err){error(err);return;}
        close_mode();new_game();
    }else if(fe.mode==4){
        err=midend_game_id(fe.me,fe.entry);if(err){error(err);return;}
        close_mode();new_game();
    }else if(fe.mode==5){close_mode();midend_restart_game(fe.me);modified();}
    else if(fe.mode==6){close_mode();err=midend_solve(fe.me);if(err)error(err);else {message("Solution revealed. Undo returns to your game.");modified();}}
    else close_mode();
}
static void key(int ch,unsigned mods)
{
    int k,n,keypad=0;config_item *c;
    if((unsigned)ch>=MWKEY_KP0&&(unsigned)ch<=MWKEY_KP9){keypad=1;ch='0'+((unsigned)ch-MWKEY_KP0);}
    if(ch==MWKEY_KP_ENTER)ch='\r';k=ch;
    if(ch==MWKEY_ESCAPE||ch==27){if(fe.mode)close_mode();return;}
    if(ch==MWKEY_F1){action(7);return;}
    if(fe.mode){
        if(ch=='\r'||ch=='\n'){modal_accept();return;}
        if(fe.mode==1){
            if(ch==MWKEY_HOME)fe.choice=0;if(ch==MWKEY_END)fe.choice=fe.count;
            if(ch==MWKEY_UP&&fe.choice)fe.choice--;
            if(ch==MWKEY_DOWN&&fe.choice<fe.count)fe.choice++;
            if(ch==MWKEY_PAGEUP)fe.choice=max(0,fe.choice-list_rows());
            if(ch==MWKEY_PAGEDOWN)fe.choice=min(fe.count,fe.choice+list_rows());
        }else if(fe.mode==3){
            if(ch==MWKEY_UP)fe.cfgrow=max(0,fe.cfgrow-1);
            else if(ch==MWKEY_DOWN||ch=='\t')fe.cfgrow=(fe.cfgrow+1)%fe.cfgcount;
            else {
                c=fe.cfg+fe.cfgrow;
                if(c->type==C_BOOLEAN&&(ch==' '||ch==MWKEY_LEFT||ch==MWKEY_RIGHT))c->u.boolean.bval=!c->u.boolean.bval;
                else if(c->type==C_CHOICES&&(ch==' '||ch==MWKEY_LEFT||ch==MWKEY_RIGHT)){
                    const char *s=c->u.choices.choicenames;n=0;for(s++;*s;s++)if(*s==*c->u.choices.choicenames)n++;
                    c->u.choices.selected=(c->u.choices.selected+n+1+(ch==MWKEY_LEFT?-1:1))%(n+1);
                }else if(c->type==C_STRING){
                    n=strlen(c->u.string.sval);
                    if((mods&MWKMOD_CTRL)&&(ch=='u'||ch=='U'||ch==21))c->u.string.sval[0]=0;
                    else if((ch==8||ch==127||ch==MWKEY_BACKSPACE)&&n)c->u.string.sval[n-1]=0;
                    else if(ch>=32&&ch<127&&n<128){c->u.string.sval=sresize(c->u.string.sval,n+2,char);c->u.string.sval[n]=ch;c->u.string.sval[n+1]=0;}
                }
            }
        }else if(fe.mode==4){
            n=strlen(fe.entry);
            if((ch==8||ch==127||ch==MWKEY_BACKSPACE)&&n){if(fe.choice)fe.entry[0]=0;else fe.entry[n-1]=0;fe.choice=0;}
            else if(ch>=32&&ch<127){if(fe.choice)fe.entry[n=0]=0;fe.choice=0;if(n<(int)sizeof(fe.entry)-1){fe.entry[n]=ch;fe.entry[n+1]=0;}}
        }
        fe.full=1;return;
    }
    if(ch==MWKEY_F2){action(1);return;}if(ch==MWKEY_F3){action(6);return;}
    if(ch=='n'||ch=='N'){action(0);return;}
    if(mods&MWKMOD_CTRL){
        if(ch=='z'||ch=='Z'||ch==26){action(2);return;}
        if(ch=='y'||ch=='Y'||ch==25){action(3);return;}
        if(ch=='s'||ch=='S'||ch==19){save();return;}
        if(ch=='q'||ch=='Q'||ch==17){stopping=1;return;}
    }
    if(ch==MWKEY_UP)k=CURSOR_UP;else if(ch==MWKEY_DOWN)k=CURSOR_DOWN;
    else if(ch==MWKEY_LEFT)k=CURSOR_LEFT;else if(ch==MWKEY_RIGHT)k=CURSOR_RIGHT;
    else if(ch=='\r'||ch=='\n')k=CURSOR_SELECT;else if(ch==' ')k=CURSOR_SELECT2;
    else if(ch==MWKEY_BACKSPACE||ch==MWKEY_DELETE||ch==127)k=8;
    else if(ch>127)return;
    if(keypad)k|=MOD_NUM_KEYPAD;
    if(k>=CURSOR_UP&&k<=CURSOR_RIGHT){if(mods&MWKMOD_SHIFT)k|=MOD_SHFT;if(mods&MWKMOD_CTRL)k|=MOD_CTRL;}
    game_key(0,0,k);
}
static void mouse_down(GR_EVENT_BUTTON *b)
{
    int x=b->x,y=b->y,bw=(fe.w-16)/8,k=0;
    if(b->changebuttons&(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)){
        if(fe.mode==1){fe.choice=max(0,min(fe.count,fe.choice+(b->changebuttons&GR_BUTTON_SCROLLUP?-1:1)));fe.full=1;}return;
    }
    if(b->changebuttons&GR_BUTTON_L){
        if(fe.mode&&y>=8&&y<33&&x>=fe.w-168){if(x<fe.w-92)modal_accept();else if(x>=fe.w-84)close_mode();return;}
        if(y>=44&&y<71&&x>=8&&x<8+bw*8){action((x-8)/bw);return;}
        if(fe.mode){
            if(fe.mode==1&&y>=TOP+33&&y<TOP+33+list_rows()*27){int n=fe.first+(y-TOP-33)/27;if(n<=fe.count){fe.choice=n;modal_accept();}}
            else if(fe.mode==3&&y>=TOP+34&&y<TOP+34+list_rows()*27){int n=fe.first+(y-TOP-34)/27;if(n<fe.cfgcount){fe.cfgrow=n;key(' ',0);}}
            else if(fe.mode==2)close_mode();
            return;
        }
    }
    if(fe.mode||x<fe.bx||y<fe.by||x>=fe.bx+fe.pw||y>=fe.by+fe.ph)return;
    if(b->changebuttons&GR_BUTTON_L)k=LEFT_BUTTON;
    if(b->changebuttons&GR_BUTTON_M)k=MIDDLE_BUTTON;
    if(b->changebuttons&GR_BUTTON_R)k=RIGHT_BUTTON;
    if(k==LEFT_BUTTON&&(b->modifiers&MWKMOD_SHIFT))k=MIDDLE_BUTTON;
    if(k==LEFT_BUTTON&&(b->modifiers&MWKMOD_CTRL))k=RIGHT_BUTTON;
    if(k){fe.held=k;game_key(x-fe.bx,y-fe.by,k);}
}
int main(int argc,char **argv)
{
    GR_SCREEN_INFO screen;GR_EVENT e;FILE *f;const char *err,*home=getenv("HOME"),*id=NULL;char dir[448];int i,fresh=0;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")){printf("usage: %s [--new] [params#seed | params:description]\nF2: presets/custom. F3: game ID. F1: controls. Ctrl-Q: quit.\nProgress and undo history: $HOME/.puzzles/%s.save\n",thegame.htmlhelp_topic,thegame.htmlhelp_topic);return 0;}
        if(!strcmp(argv[i],"--new"))fresh=1;
        else if(!id)id=argv[i];else return 2;
    }
    fe.trace=!!getenv("PUZZLES_TRACE");fe.w=720;fe.h=620;fe.full=fe.hud=1;
    for(i=0;i<(int)PUZZLE_COUNT;i++)if(!strcmp(puzzle_catalog[i].id,thegame.htmlhelp_topic))fe.info=puzzle_catalog+i;
    if(!fe.info)fatal("missing game help for %s",thegame.htmlhelp_topic);
    if(snprintf(dir,sizeof(dir),"%s/.puzzles",home?home:"/root")>=(int)sizeof(dir))fatal("save path too long");
    if(mkdir(dir,0700)<0&&errno!=EEXIST)fatal("cannot create save directory");
    snprintf(fe.path,sizeof(fe.path),"%s/%s.save",dir,thegame.htmlhelp_topic);
    fe.me=midend_new(&fe,&thegame,&api,&fe);flatten(midend_get_presets(fe.me,NULL));
    f=fresh||id?NULL:fopen(fe.path,"r");
    if(f){err=midend_deserialise(fe.me,file_read,f);fclose(f);if(err){fe.save_blocked=1;error(err);}else fresh=-1;}
    else if(!fresh&&!id&&errno!=ENOENT){fe.save_blocked=1;error("Cannot read saved progress; file kept. New starts fresh.");}
    if(id){err=midend_game_id(fe.me,id);if(err)fatal("%s",err);}
    if(fresh!=-1)midend_new_game(fe.me);
    if(GrOpen()<0){fputs("puzzles: start Nano-X with startx first\n",stderr);midend_free(fe.me);return 1;}
    GrGetScreenInfo(&screen);fe.w=min(fe.w,screen.cols-24);fe.h=min(fe.h,screen.rows-64);
    if(fe.w<480)fe.w=480;if(fe.h<400)fe.h=400;
    fe.win=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,thegame.name,GR_ROOT_WINDOW_ID,12,8,fe.w,fe.h,BG);fe.gc=GrNewGC();fe.copygc=GrNewGC();
    if(!fe.win||!fe.gc||!fe.copygc)fatal("cannot create puzzle window");GrSetGCUseBackground(fe.gc,GR_FALSE);
    GrSelectEvents(fe.win,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_FOCUS_OUT|GR_EVENT_MASK_TIMER);
    if(pipe(wakefd)<0||fcntl(wakefd[0],F_SETFL,O_NONBLOCK)<0||fcntl(wakefd[1],F_SETFL,O_NONBLOCK)<0)fatal("cannot create signal pipe");
    GrRegisterInput(wakefd[0]);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGINT,caught);
    font_upload();colours();layout();title();GrMapWindow(fe.win);GrSetFocus(fe.win);paint();timer_sync(&fe);
    if(fresh!=-1){modified();save();}
    if(fe.trace)fprintf(stderr,"PUZZLES_READY %s restored=%d\n",thegame.htmlhelp_topic,fresh==-1);
    while(!stopping){
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        else if(e.type==GR_EVENT_TYPE_FDINPUT){char bytes[16];read(wakefd[0],bytes,sizeof(bytes));}
        else if(e.type==GR_EVENT_TYPE_EXPOSURE)fe.full=1;
        else if(e.type==GR_EVENT_TYPE_KEY_DOWN)key(e.keystroke.ch,e.keystroke.modifiers);
        else if(e.type==GR_EVENT_TYPE_BUTTON_DOWN)mouse_down(&e.button);
        else if(e.type==GR_EVENT_TYPE_BUTTON_UP&&fe.held){game_key(e.button.x-fe.bx,e.button.y-fe.by,fe.held+(LEFT_RELEASE-LEFT_BUTTON));fe.held=0;}
        else if(e.type==GR_EVENT_TYPE_MOUSE_POSITION&&fe.held&&!fe.mode)game_key(e.mouse.x-fe.bx,e.mouse.y-fe.by,fe.held+(LEFT_DRAG-LEFT_BUTTON));
        else if(e.type==GR_EVENT_TYPE_FOCUS_OUT&&fe.held){game_key(-1,-1,fe.held+(LEFT_RELEASE-LEFT_BUTTON));fe.held=0;}
        else if(e.type==GR_EVENT_TYPE_UPDATE&&e.update.utype==GR_UPDATE_SIZE&&e.update.wid==fe.win){
            fe.w=max(480,e.update.width);fe.h=max(400,e.update.height);fe.held=0;
            if(fe.w!=e.update.width||fe.h!=e.update.height)GrResizeWindow(fe.win,fe.w,fe.h);
            layout();if(fe.trace)fprintf(stderr,"PUZZLES_RESIZE %s %d %d\n",thegame.htmlhelp_topic,fe.w,fe.h);
        }else if(e.type==GR_EVENT_TYPE_TIMER&&e.timer.tid==fe.timer){
            unsigned long t=now_ms(),elapsed=t-fe.lasttick;fe.lasttick=t;
            if(fe.active_timer)midend_timer(fe.me,elapsed/1000.0F);
            if(fe.savedirty&&(long)(t-fe.saveat)>=0){if(!save())fe.saveat=t+10000;}
        }
        if(!stopping){paint();timer_sync(&fe);}
    }
    fe.savedirty=1; /* Also retain a timed game's elapsed time at close. */
    if(!save()&&!fe.save_blocked)fe.exit_code=1;
    close_mode();if(fe.timer)GrDestroyTimer(fe.timer);
    GrUnregisterInput(wakefd[0]);close(wakefd[0]);close(wakefd[1]);wakefd[0]=wakefd[1]=-1;
    midend_free(fe.me);sfree(fe.colours);sfree(fe.presets);
    fde_image_destroy(fe.font);
    GrDestroyWindow(fe.canvas);GrDestroyWindow(fe.win);GrDestroyGC(fe.gc);GrDestroyGC(fe.copygc);GrClose();
    if(fe.trace)fprintf(stderr,"PUZZLES_EXIT %s %d\n",thegame.htmlhelp_topic,fe.exit_code);
    return fe.exit_code;
}
