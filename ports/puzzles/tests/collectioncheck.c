/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "puzzles.h"
#include "puzzle_engines.h"
/* midend's combined-build discovery uses this ordinary upstream interface. */
#define GAME(name) &name,
const game *gamelist[] = {
#include "generated-games.h"
};
#undef GAME
const int gamecount=sizeof(gamelist)/sizeof(gamelist[0]);

static SHA_State trace;
static int failures,recording;
struct buffer { char *data; int len,pos; };
struct blitter { int w,h; };
void activate_timer(frontend *f) { (void)f; }
void deactivate_timer(frontend *f) { (void)f; }
static void check(int yes,const char *what)
{ if(!yes){printf("FAIL %s\n",what);failures++;} }
static void record(const char *s) { if(recording)SHA_Bytes(&trace,s,strlen(s)); }
static void numbers(const char *op,int a,int b,int c,int d,int e)
{
    char buf[160];snprintf(buf,sizeof(buf),"%s %d %d %d %d %d\n",op,a,b,c,d,e);record(buf);
}
static void p_noop(drawing *d) { (void)d; }
static void p_rect(drawing *d,int x,int y,int w,int h,int c) { (void)d;numbers("rect",x,y,w,h,c); }
static void p_line(drawing *d,int x,int y,int xx,int yy,int c) { (void)d;numbers("line",x,y,xx,yy,c); }
static void p_text(drawing *d,int x,int y,int t,int s,int a,int c,const char *v)
{ (void)d;numbers("text",x,y,t,s,a);numbers("colour",c,0,0,0,0);record(v);record("\n"); }
static void p_circle(drawing *d,int x,int y,int r,int f,int o) { (void)d;numbers("circle",x,y,r,f,o); }
static void p_region(drawing *d,int x,int y,int w,int h) { (void)d;numbers("clip",x,y,w,h,0); }
static blitter *p_bl_new(drawing *d,int w,int h) { blitter *b=snew(blitter);(void)d;b->w=w;b->h=h;return b; }
static void p_bl_free(drawing *d,blitter *b) { (void)d;sfree(b); }
static void p_bl_save(drawing *d,blitter *b,int x,int y) { (void)d;numbers("save",x,y,b->w,b->h,0); }
static void p_bl_load(drawing *d,blitter *b,int x,int y) { (void)d;numbers("load",x,y,b->w,b->h,0); }
static const drawing_api api={.version=1,.draw_text=p_text,.draw_rect=p_rect,.draw_line=p_line,
    .draw_polygon=draw_polygon_fallback,.draw_circle=p_circle,.clip=p_region,.unclip=p_noop,
    .start_draw=p_noop,.end_draw=p_noop,.blitter_new=p_bl_new,.blitter_free=p_bl_free,
    .blitter_save=p_bl_save,.blitter_load=p_bl_load};
static void write_buf(void *ctx,const void *data,int len)
{
    struct buffer *b=ctx;b->data=sresize(b->data,b->len+len,char);
    memcpy(b->data+b->len,data,len);b->len+=len;
}
static bool read_buf(void *ctx,void *data,int len)
{
    struct buffer *b=ctx;if(len>b->len-b->pos)return false;
    memcpy(data,b->data+b->pos,len);b->pos+=len;return true;
}
static void hash(const char *tag,SHA_State *sha)
{
    unsigned char sum[20];int i;SHA_Final(sha,sum);printf("%s ",tag);
    for(i=0;i<20;i++)printf("%02x",(unsigned)sum[i]);putchar('\n');
}
int main(int argc,char **argv)
{
    const game *g;const char *err;midend *m,*copy,*generator;char *id,*id2,*encoded;game_params *params;char requested[512];
    struct buffer b={NULL,0,0};int w=600,h=420,i,x,y,undo,status;
    SHA_State sha;
    if(argc<2||atoi(argv[1])<0||atoi(argv[1])>=(int)(sizeof(puzzle_engines)/sizeof(puzzle_engines[0])))return 2;
    g=puzzle_engines[atoi(argv[1])];generator=midend_new(NULL,g,NULL,NULL);
    params=midend_get_params(generator);encoded=g->encode_params(params,true);g->free_params(params);
    if(argc<3||argv[2][0]=='#')snprintf(requested,sizeof(requested),"%s%s",encoded,argc>2?argv[2]:"#123456789");
    else snprintf(requested,sizeof(requested),"%s",argv[2]);
    sfree(encoded);err=midend_game_id(generator,requested);if(err)fatal("%s",err);
    midend_new_game(generator);id=midend_get_game_id(generator);
    SHA_Init(&sha);SHA_Bytes(&sha,id,strlen(id));hash("BOARD",&sha);midend_free(generator);
    m=midend_new(NULL,g,&api,NULL);copy=midend_new(NULL,g,&api,NULL);
    err=midend_game_id(m,id);if(err)fatal("%s",err);midend_new_game(m);
    midend_size(m,&w,&h,false,1.0);SHA_Init(&trace);recording=1;
    midend_redraw(m);recording=0;hash("DRAW",&trace);
    /* Real midend input paths, not only generation and solving. */
    SHA_Init(&trace);recording=1;
    for(i=1;i<=3;i++){
        x=w*i/4;y=h*i/4;
        midend_process_key(m,x,y,LEFT_BUTTON);midend_process_key(m,x+4,y+4,LEFT_DRAG);midend_process_key(m,x+4,y+4,LEFT_RELEASE);
        midend_process_key(m,x,y,'1'+i);midend_timer(m,1.0F);
        midend_process_key(m,x,y,RIGHT_BUTTON);midend_process_key(m,x,y,RIGHT_RELEASE);midend_timer(m,1.0F);
    }
    midend_process_key(m,0,0,CURSOR_RIGHT);midend_process_key(m,0,0,CURSOR_DOWN);
    midend_process_key(m,0,0,CURSOR_SELECT2);midend_timer(m,1.0F);
    recording=0;hash("MOVES",&trace);
    if(g->can_solve){err=midend_solve(m);printf("SOLVE %s\n",err?err:"OK");}
    undo=midend_can_undo(m);if(undo)midend_process_key(m,0,0,UI_UNDO);
    status=midend_status(m);midend_serialise(m,write_buf,&b);
    err=midend_deserialise(copy,read_buf,&b);check(!err,"load saved history");
    /* Loading creates a fresh drawstate; initialise its size before moves
     * can redraw it (Inertia allocates its player blitter in set_size). */
    {int cw=600,ch=420;midend_size(copy,&cw,&ch,false,1.0);}
    check(midend_status(copy)==status,"status restored");
    check(midend_can_redo(copy)==midend_can_redo(m),"redo availability restored");
    id2=midend_get_game_id(copy);check(!strcmp(id,id2),"identity restored");sfree(id2);
    if(undo){midend_process_key(copy,0,0,UI_REDO);check(midend_can_undo(copy),"redo after reload");midend_process_key(copy,0,0,UI_UNDO);}
    midend_restart_game(copy);
    b.pos=0;b.len=12;err=midend_deserialise(copy,read_buf,&b);check(err!=NULL,"reject truncated save");
    id2=midend_get_game_id(copy);check(!strcmp(id,id2),"failed load is atomic");sfree(id2);
    sfree(id);sfree(b.data);midend_free(m);midend_free(copy);
    printf("PUZZLES_COLLECTION %s failures=%d\n",g->htmlhelp_topic,failures);return failures!=0;
}
