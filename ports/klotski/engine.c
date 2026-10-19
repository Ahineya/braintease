/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adapted from GNOME Klotski 3.38.2 puzzle.vala and puzzle-view.vala,
 * Copyright (C) 2010-2013 Robert Ancell. See upstream/ and COPYING.
 * Bounds, journal history and persistent-state validation are port additions. */
#include "klotski.h"
#include <stdlib.h>
#include <string.h>
#include "levels.inc"

int k_static(char id) { return !id||id=='#'||id=='.'||id==' '||id=='-'; }
void k_free(KGame *g) { free(g->steps);memset(g,0,sizeof(*g)); }
int k_begin(KGame *g,const KLevel *l)
{
    int n;
    if(!l||l->width<1||l->width>19||l->height<1||l->height>22)return 0;
    n=l->width*l->height;
    if(n>K_CELLS||strlen(l->map)!=(unsigned)n||!strchr(l->map,'*')||!strchr(l->map,'.'))return 0;
    k_free(g);g->level=l;memcpy(g->board,l->map,n);return 1;
}
char k_at(const KGame *g,int x,int y)
{
    if(!g->level||x<0||y<0||x>=g->level->width||y>=g->level->height)return '#';
    return g->board[y*g->level->width+x];
}
int k_can(const KGame *g,char id,int dx,int dy)
{
    int x,y,found=0;char c;
    if(!g->level||k_static(id)||!((dx==0&&(dy==1||dy==-1))||(dy==0&&(dx==1||dx==-1))))return 0;
    for(y=0;y<g->level->height;y++)for(x=0;x<g->level->width;x++)if(k_at(g,x,y)==id){
        found=1;c=k_at(g,x+dx,y+dy);
        if(!(c==' '||c=='.'||c==id||(id=='*'&&c=='-')))return 0;
    }
    return found;
}
static void translate(KGame *g,char id,int dx,int dy)
{
    char tmp[K_CELLS];int x,y,i,n=g->level->width*g->level->height;
    memcpy(tmp,g->board,n);
    for(i=0;i<n;i++)if(tmp[i]==id)tmp[i]=' ';
    for(y=0;y<g->level->height;y++)for(x=0;x<g->level->width;x++)
        if(k_at(g,x,y)==id)tmp[(y+dy)*g->level->width+x+dx]=id;
    for(i=0;i<n;i++)if(tmp[i]==' '&&(g->level->map[i]=='.'||g->level->map[i]=='-'))tmp[i]=g->level->map[i];
    memcpy(g->board,tmp,n);
}
static void score(KGame *g,char id,int dx,int dy)
{
    /* A rigid piece returning to its group origin restores the previous move
     * count, exactly as the upstream view's undomove_map comparison does. */
    if(g->score.last!=id){g->score.moves++;g->score.last=id;g->score.dx=g->score.dy=0;}
    g->score.dx+=dx;g->score.dy+=dy;
    if(!g->score.dx&&!g->score.dy){g->score.moves--;g->score.last=0;}
}
int k_move(KGame *g,char id,int dx,int dy)
{
    unsigned cap;KStep *p,*s;
    if(!k_can(g,id,dx,dy)||k_won(g))return 0;
    if(g->cursor==K_HISTORY)return -1;
    if(g->cursor==g->capacity){
        cap=g->capacity?g->capacity*2:128;if(cap>K_HISTORY)cap=K_HISTORY;
        p=realloc(g->steps,(unsigned long)cap*sizeof(*p));if(!p)return -1;
        g->steps=p;g->capacity=cap;
    }
    s=g->steps+g->cursor;s->id=id;s->dx=dx;s->dy=dy;s->before=g->score;
    translate(g,id,dx,dy);score(g,id,dx,dy);g->cursor++;g->count=g->cursor;return 1;
}
int k_undo(KGame *g)
{
    KStep *s;if(!g->cursor)return 0;s=g->steps+g->cursor-1;
    translate(g,s->id,-s->dx,-s->dy);g->score=s->before;g->cursor--;return 1;
}
int k_redo(KGame *g)
{
    KStep *s;if(g->cursor==g->count)return 0;s=g->steps+g->cursor;
    translate(g,s->id,s->dx,s->dy);score(g,s->id,s->dx,s->dy);g->cursor++;return 1;
}
int k_won(const KGame *g)
{
    int i,n;if(!g->level)return 0;n=g->level->width*g->level->height;
    for(i=0;i<n;i++)if(g->board[i]=='*'&&g->level->map[i]!='.')return 0;
    return 1;
}
int k_tile(const KGame *g,int x,int y)
{
    /* Original eight-neighbour tile lookup, including the nonrectangular joins. */
    static const unsigned pairs[]={0,0,64,1,66,2,2,3,16,4,24,5,8,6,208,7,248,8,104,9,214,10,255,11,107,12,22,13,31,14,11,15,18,16,10,17,80,18,72,19,25,5,28,5,56,5,152,5,184,5,29,5,67,2,70,2,98,2,194,2,99,2,198,2,9,6,40,6,20,4,144,4,3,3,6,3,96,1,192,1,224,1,14,17,42,17,46,17,19,16,146,16,147,16,73,19,200,19,201,19,84,18,112,18,116,18,26,25};
    int xx,yy,i=0;unsigned mask=0,bit=1;char c=k_at(g,x,y);
    if(c=='-')return 23;if(c==' ')return 21;if(c=='.')return 20;
    for(yy=-1;yy<=1;yy++)for(xx=-1;xx<=1;xx++)if(xx||yy){
        if(x+xx>=0&&y+yy>=0&&x+xx<g->level->width&&y+yy<g->level->height&&k_at(g,x+xx,y+yy)==c)mask|=bit;bit<<=1;
    }
    for(i=0;i<108;i+=2)if(pairs[i]==mask)return pairs[i+1];
    return -1; /* Explicit renderer failure for an unsupported topology. */
}
