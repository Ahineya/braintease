/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "puzzles.h"

static int failures;
struct buffer { char *data; int len,pos; };
void activate_timer(frontend *f) { (void)f; }
void deactivate_timer(frontend *f) { (void)f; }
static void noop(drawing *d) { (void)d; }
static void rect(drawing *d,int x,int y,int w,int h,int c) { (void)d;(void)x;(void)y;(void)w;(void)h;(void)c; }
static void line(drawing *d,int x,int y,int xx,int yy,int c) { rect(d,x,y,xx,yy,c); }
static void text(drawing *d,int x,int y,int t,int s,int a,int c,const char *v) { rect(d,x,y,t,s,c);(void)a;(void)v; }
static void circle(drawing *d,int x,int y,int r,int f,int o) { rect(d,x,y,r,f,o); }
static void region(drawing *d,int x,int y,int w,int h) { rect(d,x,y,w,h,0); }
struct blitter { int dummy; };
static blitter *bl_new(drawing *d,int w,int h) { (void)d;(void)w;(void)h;return snew(blitter); }
static void bl_free(drawing *d,blitter *b) { (void)d;sfree(b); }
static void bl_copy(drawing *d,blitter *b,int x,int y) { (void)d;(void)b;(void)x;(void)y; }
static const drawing_api api={.version=1,.draw_text=text,.draw_rect=rect,.draw_line=line,.draw_polygon=draw_polygon_fallback,.draw_circle=circle,.clip=region,.unclip=noop,.start_draw=noop,.end_draw=noop,.blitter_new=bl_new,.blitter_free=bl_free,.blitter_save=bl_copy,.blitter_load=bl_copy};
static void check(int yes,const char *what)
{ if(!yes){printf("FAIL %s\n",what);failures++;} }
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
static void digest(const char *tag,const char *data,int len)
{
    unsigned char sum[20];int i;SHA_Simple(data,len,sum);printf("%s ",tag);
    for(i=0;i<20;i++)printf("%02x",(unsigned)sum[i]);putchar('\n');
}
int main(int argc,char **argv)
{
    midend *m,*copy,*generator;const char *err;char *id,*id2;struct buffer b={NULL,0,0};int w=700,h=500;
    unsigned char hash[20];int i;random_state *r;
    static const unsigned char abc[]={0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    SHA_Simple("abc",3,hash);check(!memcmp(hash,abc,sizeof(hash)),"SHA-1 octets");
    r=random_new("Riplix",6);printf("RANDOM");for(i=0;i<12;i++)printf(" %lu",random_upto(r,100000));putchar('\n');random_free(r);
    generator=midend_new(NULL,&thegame,NULL,NULL);
    err=midend_game_id(generator,argc>1?argv[1]:"#123456789");if(err)fatal("%s",err);
    midend_new_game(generator);id=midend_get_game_id(generator);digest("BOARD",id,strlen(id));midend_free(generator);
    m=midend_new(NULL,&thegame,&api,NULL);copy=midend_new(NULL,&thegame,&api,NULL);
    err=midend_game_id(m,id);if(err)fatal("%s",err);midend_new_game(m);midend_size(m,&w,&h,false,1.0);
    check(midend_status(m)==0,"new game in progress");
    err=midend_solve(m);check(!err,"solver success");check(midend_status(m)==(!strcmp(thegame.htmlhelp_topic,"mines")?0:1),"solved/revealed");
    check(midend_can_undo(m),"undo available");midend_process_key(m,0,0,UI_UNDO);check(midend_status(m)==0,"undo solution");
    check(midend_can_redo(m),"redo available");
    midend_serialise(m,write_buf,&b);digest("SAVE",b.data,b.len);
    err=midend_deserialise(copy,read_buf,&b);check(!err,"load saved history");
    check(midend_status(copy)==0&&midend_can_redo(copy),"history restored");
    id2=midend_get_game_id(copy);check(!strcmp(id,id2),"identity restored");sfree(id2);
    midend_process_key(copy,0,0,UI_REDO);check(midend_status(copy)==(!strcmp(thegame.htmlhelp_topic,"mines")?0:1),"redo after load");
    midend_process_key(copy,0,0,UI_UNDO);midend_restart_game(copy);check(midend_status(copy)==0,"restart");
    /* A truncated save must not damage the existing state. */
    b.pos=0;b.len=12;err=midend_deserialise(copy,read_buf,&b);check(err!=NULL,"reject truncated save");
    id2=midend_get_game_id(copy);check(!strcmp(id,id2),"failed load is atomic");sfree(id2);
    sfree(id);sfree(b.data);midend_free(m);midend_free(copy);
    printf("PUZZLES_ENGINE %s failures=%d\n",thegame.htmlhelp_topic,failures);return !!failures;
}
