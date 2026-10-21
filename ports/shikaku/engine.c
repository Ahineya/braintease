/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "shikaku.h"
#include "puzzles.inc"
const int s_sizes[6]={5,7,10,15,20,25};
SState *s_state(SGame *g) { return g->history+g->cursor; }
void s_destroy(SGame *g) {
    int i;for(i=0;i<g->states;i++)free(g->history[i].rects);
    memset(g,0,sizeof(*g));
}
SRect s_solution(int p,int i) {
    SRect r={0,0,0,0};const char *s;
    if(p<0||p>=S_PUZZLES||i<0||i>=s_puzzles[p].count)return r;
    s=s_puzzles[p].data+i*6;r.x=s[0]-33;r.y=s[1]-33;r.w=s[2]-33;r.h=s[3]-33;return r;
}
unsigned long s_signature(int p) {
    unsigned long h=5381;const char *s=s_puzzles[p].data;
    while(*s)h=(h*33UL+(unsigned)*s++)&0xffffffffUL;
    return h;
}
int s_begin(SGame *g,int p) {
    int i;const char *s;
    if(p<0||p>=S_PUZZLES)return 0;
    s_destroy(g);g->puzzle=p;g->size=s_puzzles[p].size;g->states=1;g->revision=1;
    s=s_puzzles[p].data;
    for(i=0;i<s_puzzles[p].count;i++,s+=6)g->clues[(s[5]-33)*g->size+s[4]-33]=(s[2]-33)*(s[3]-33);
    return 1;
}
int s_inside(const SGame *g,SRect r) {
    return r.x>=0&&r.y>=0&&r.w>0&&r.h>0&&r.x<g->size&&r.y<g->size&&r.w<=g->size-r.x&&r.h<=g->size-r.y;
}
int s_intersects(SRect a,SRect b) { return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h; }
int s_status(const SGame *g,SRect r) {
    int x,y,n=0,area=0;if(!s_inside(g,r))return -1;
    for(y=r.y;y<r.y+r.h;y++)for(x=r.x;x<r.x+r.w;x++)if(g->clues[y*g->size+x]){n++;area=g->clues[y*g->size+x];}
    return n==1?(area==r.w*r.h): -1;
}
static int publish(SGame *g,SRect *rects,int count) {
    int i;
    for(i=g->cursor+1;i<g->states;i++)free(g->history[i].rects);
    g->states=g->cursor+1;
    if(g->states==S_HISTORY){free(g->history[0].rects);memmove(g->history,g->history+1,(S_HISTORY-1)*sizeof(SState));g->cursor--;g->states--;}
    g->cursor++;g->states++;g->history[g->cursor].rects=rects;g->history[g->cursor].count=count;g->revision++;return 1;
}
int s_apply(SGame *g,SRect r,int erase) {
    SState *old=s_state(g);SRect *next;int i,count=0,removed=0;
    if(!s_inside(g,r))return 0;
    for(i=0;i<old->count;i++)if(s_intersects(old->rects[i],r))removed++;
    if(erase&&!removed)return 0;
    if(!erase&&removed==1){for(i=0;i<old->count;i++)if(!memcmp(old->rects+i,&r,sizeof(r)))return 0;}
    count=old->count-removed+!erase;
    next=count?malloc((unsigned long)count*sizeof(SRect)):NULL;if(count&&!next)return -1;
    count=0;for(i=0;i<old->count;i++)if(!s_intersects(old->rects[i],r))next[count++]=old->rects[i];
    if(!erase)next[count++]=r;
    return publish(g,next,count);
}
int s_clear(SGame *g) { return s_state(g)->count?publish(g,NULL,0):0; }
int s_undo(SGame *g) { if(!g->cursor)return 0;g->cursor--;g->revision++;return 1; }
int s_redo(SGame *g) { if(g->cursor+1==g->states)return 0;g->cursor++;g->revision++;return 1; }
int s_restore(SGame *g,const SRect *rects,int count) {
    SRect *copy;int i,j;
    if(count<0||count>g->size*g->size)return 0;
    for(i=0;i<count;i++){
        if(!s_inside(g,rects[i]))return 0;
        for(j=0;j<i;j++)if(s_intersects(rects[i],rects[j]))return 0;
    }
    copy=count?malloc((unsigned long)count*sizeof(SRect)):NULL;if(count&&!copy)return -1;
    if(count)memcpy(copy,rects,(unsigned long)count*sizeof(SRect));
    for(i=0;i<g->states;i++)free(g->history[i].rects);
    memset(g->history,0,sizeof(g->history));g->cursor=0;g->states=1;g->history[0].rects=copy;g->history[0].count=count;g->revision++;return 1;
}
int s_progress(SGame *g,int *errors) {
    int i,area=0,bad=0;SState *s=s_state(g);
    for(i=0;i<s->count;i++)if(s_status(g,s->rects[i])==1)area+=s->rects[i].w*s->rects[i].h;else bad++;
    if(errors)*errors=bad;return area;
}
int s_won(SGame *g) { int bad;return s_progress(g,&bad)==g->size*g->size&&!bad; }

unsigned long s_game_signature(const SGame *g) {
    unsigned long h=5381;int i;
    if(!g->generated)return s_signature(g->puzzle);
    for(i=0;i<g->size*g->size;i++)h=(h*33UL+(unsigned)g->clues[i])&0xffffffffUL;
    return h;
}
