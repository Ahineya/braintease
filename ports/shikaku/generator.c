/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Seeded partition generation followed by a resumable exact-cover proof. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "shikaku.h"
typedef struct { SRect r; uint32_t mask; int clue; } Candidate;
typedef struct { int clue,placed;unsigned long next,end; } Search;
struct SGenerator {
    int size,count,refinements,depth,solutions,done,failed,attempts;
    uint32_t random,seed,rows[S_MAX];
    SRect rects[S_CELLS],answer[2][S_CELLS];
    int x[S_CELLS],y[S_CELLS],clues[S_CELLS],prefix[(S_MAX+1)*(S_MAX+1)];
    unsigned long cell_start[S_CELLS+1];
    int *cell_candidates;
    Search stack[S_CELLS+1];
    Candidate *candidates;unsigned capacity,length;
    unsigned long nodes;
};
static uint32_t random_next(SGenerator *g) {
    uint32_t x=g->random;x^=x<<13;x^=x>>17;x^=x<<5;g->random=x;return x;
}
static int pick(SGenerator *g,int n) { return random_next(g)%(unsigned)n; }
static void clue_at(SGenerator *g,int i) { SRect r=g->rects[i];g->x[i]=r.x+pick(g,r.w);g->y[i]=r.y+pick(g,r.h); }
static int equal(SRect a,SRect b) { return a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h; }
static int available(SGenerator *g,const Candidate *c) {
    int y;for(y=c->r.y;y<c->r.y+c->r.h;y++)if(g->rows[y]&c->mask)return 0;return 1;
}
static void mark(SGenerator *g,const Candidate *c,int set) {
    int y;for(y=c->r.y;y<c->r.y+c->r.h;y++)if(set)g->rows[y]|=c->mask;else g->rows[y]&=~c->mask;

}
static int add(SGenerator *g,SRect r,int clue) {
    Candidate *p;unsigned capacity;
    if(g->length==g->capacity){
        capacity=g->capacity?g->capacity*2:256;
        if(capacity>16384)return 0;
        p=realloc(g->candidates,(unsigned long)capacity*sizeof(*p));if(!p)return 0;
        g->candidates=p;g->capacity=capacity;
    }
    p=g->candidates+g->length++;p->r=r;p->clue=clue;p->mask=((1UL<<r.w)-1)<<r.x;return 1;
}
static int prepare(SGenerator *g) {
    int i,x,y,w,h,area,n=g->size,stride=n+1,sum;unsigned long offsets[S_CELLS],length;SRect r;Candidate *c;
    memset(g->clues,0,sizeof(g->clues));memset(g->prefix,0,sizeof(g->prefix));
    for(i=0;i<g->count;i++)g->clues[g->y[i]*n+g->x[i]]=g->rects[i].w*g->rects[i].h;
    for(y=0;y<n;y++)for(x=0;x<n;x++)g->prefix[(y+1)*stride+x+1]=!!g->clues[y*n+x]+g->prefix[y*stride+x+1]+g->prefix[(y+1)*stride+x]-g->prefix[y*stride+x];
    g->length=0;
    for(i=0;i<g->count;i++){
        area=g->rects[i].w*g->rects[i].h;
        for(w=1;w<=n;w++)if(!(area%w)){
            h=area/w;if(h>n)continue;
            for(y=g->y[i]-h+1;y<=g->y[i];y++)if(y>=0&&y+h<=n)
                for(x=g->x[i]-w+1;x<=g->x[i];x++)if(x>=0&&x+w<=n){
                    sum=g->prefix[(y+h)*stride+x+w]-g->prefix[y*stride+x+w]-g->prefix[(y+h)*stride+x]+g->prefix[y*stride+x];
                    if(sum==1){r.x=x;r.y=y;r.w=w;r.h=h;if(!add(g,r,i))return 0;}
                }
        }
    }
    memset(g->cell_start,0,sizeof(g->cell_start));
    for(i=0;i<(int)g->length;i++){
        c=g->candidates+i;
        for(y=c->r.y;y<c->r.y+c->r.h;y++)for(x=c->r.x;x<c->r.x+c->r.w;x++)g->cell_start[y*n+x+1]++;
    }
    for(i=0;i<n*n;i++){g->cell_start[i+1]+=g->cell_start[i];offsets[i]=g->cell_start[i];}
    length=g->cell_start[n*n];free(g->cell_candidates);
    g->cell_candidates=malloc(length*sizeof(int));if(!g->cell_candidates)return 0;
    for(i=0;i<(int)g->length;i++){
        c=g->candidates+i;
        for(y=c->r.y;y<c->r.y+c->r.h;y++)for(x=c->r.x;x<c->r.x+c->r.w;x++)g->cell_candidates[offsets[y*n+x]++]=i;
    }
    g->depth=g->solutions=0;g->nodes=0;
    memset(g->rows,0,sizeof(g->rows));g->stack[0].clue=-1;return 1;
}
static int refine(SGenerator *g) {
    int i,best=-1,area=0,cut;SRect a,b;
    for(i=0;i<g->count;i++)if((g->solutions<2||!equal(g->answer[0][i],g->answer[1][i]))&&g->rects[i].w*g->rects[i].h>1&&
        (best<0||g->rects[i].w*g->rects[i].h<area)){best=i;area=g->rects[i].w*g->rects[i].h;}
    if(best<0||g->count==S_CELLS)return 0;
    a=b=g->rects[best];
    if(a.w>1&&(a.h==1||a.w>=a.h)){cut=1+pick(g,a.w-1);a.w=cut;b.x+=cut;b.w-=cut;}
    else {cut=1+pick(g,a.h-1);a.h=cut;b.y+=cut;b.h-=cut;}
    g->rects[best]=a;g->rects[g->count]=b;clue_at(g,best);clue_at(g,g->count++);g->refinements++;return prepare(g);
}
static int partition(SGenerator *g) {
    SRect pending[S_CELLS],r,a,b;int count=1,cut,i,limit,n=g->size;
    if(++g->attempts>32)return 0;
    g->count=0;
    pending[0].x=pending[0].y=0;pending[0].w=pending[0].h=n;
    while(count){
        r=pending[--count];
        limit=pick(g,4)?5+pick(g,10):15+pick(g,26);
        if(r.w*r.h<=limit&&r.w<=(n*2/3)&&r.h<=(n*2/3)){g->rects[g->count++]=r;continue;}
        a=b=r;
        if(r.w>1&&(r.h==1||pick(g,r.w+r.h)<r.w)){cut=1+pick(g,r.w-1);a.w=cut;b.x+=cut;b.w-=cut;}
        else {cut=1+pick(g,r.h-1);a.h=cut;b.y+=cut;b.h-=cut;}
        pending[count++]=a;pending[count++]=b;
    }
    for(i=0;i<g->count;i++)clue_at(g,i);
    return prepare(g);
}
SGenerator *s_generate_start(int n,unsigned long seed) {
    SGenerator *g;
    if(n<2||n>S_MAX||!seed||seed>0xffffffffUL)return NULL;
    g=calloc(1,sizeof(*g));if(!g)return NULL;g->size=n;g->seed=g->random=seed;
    if(!partition(g)){s_generate_free(g);return NULL;}return g;
}
static int varied(SGenerator *g) {
    int i,big=0,maximum=0,ones=0,area;
    for(i=0;i<g->count;i++){
        area=g->rects[i].w*g->rects[i].h;
        if(area>=12)big++;if(area>maximum)maximum=area;if(area==1)ones++;
    }
    if(g->size>=15&&(maximum<18||big<2))return 0;
    return g->size<10||ones<=g->count/8;
}
void s_generate_free(SGenerator *g) { if(g){free(g->candidates);free(g->cell_candidates);free(g);} }
/* Work is bounded by search steps; callers pump input between slices. */
int s_generate_step(SGenerator *g,unsigned budget) {
    unsigned work;unsigned long j;int i,best,choices,min,found;Search *f;Candidate *c;
    if(g->failed)return -1;if(g->done)return 1;
    for(work=0;work<budget;work++){
        if(++g->nodes>10000UL){if(!refine(g)){g->failed=1;return -1;}continue;}
        f=g->stack+g->depth;
        if(f->clue<0){
            best=-1;min=32767;
            for(i=0;i<g->size*g->size;i++)if(!(g->rows[i/g->size]&(1UL<<(i%g->size)))){
                choices=0;for(j=g->cell_start[i];j<g->cell_start[i+1];j++)if(available(g,g->candidates+g->cell_candidates[j]))choices++;
                if(choices<min){min=choices;best=i;if(choices<2)break;}
            }
            if(best<0){
                for(i=0;i<g->depth;i++){c=g->candidates+g->stack[i].placed;g->answer[g->solutions][c->clue]=c->r;}
                if(++g->solutions==2){if(!refine(g)){g->failed=1;return -1;}continue;}
                goto back;
            }
            if(!min)goto back;
            f->clue=best;f->next=g->cell_start[best];f->end=g->cell_start[best+1];
        }
        found=0;
        while(f->next<f->end){i=g->cell_candidates[f->next++];if(available(g,g->candidates+i)){found=1;break;}}
        if(found){f->placed=i;mark(g,g->candidates+i,1);g->depth++;g->stack[g->depth].clue=-1;continue;}
back:
        f->clue=-1;
        if(!g->depth){
            if(g->solutions==1){
                if(!varied(g)){if(!partition(g)){g->failed=1;return -1;}continue;}
                g->done=1;return 1;
            }
            g->failed=1;return -1;
        }
        g->depth--;mark(g,g->candidates+g->stack[g->depth].placed,0);
    }
    return 0;
}
int s_generate_finish(SGenerator *generator,SGame *g) {
    int i;if(!generator->done)return 0;
    s_destroy(g);g->size=generator->size;g->seed=generator->seed;g->generated=1;g->states=1;g->revision=1;
    for(i=0;i<6;i++)if(s_sizes[i]==g->size)break;g->puzzle=i*S_PER_SIZE;
    memcpy(g->clues,generator->clues,sizeof(g->clues));return 1;
}
unsigned long s_generate_nodes(SGenerator *g) { return g->nodes; }
int s_generate_refinements(SGenerator *g) { return g->refinements; }
