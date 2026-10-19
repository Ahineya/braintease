/* SPDX-License-Identifier: MIT */
#include "tl.h"
static TgU32 last_constructor;
TgU32 tg_tl_error(void){return last_constructor;}
static int object(TgTl *,TgView *,unsigned,unsigned *);
static int value(TgTl *t,unsigned op,unsigned depth,unsigned *budget) {
    unsigned count,n;TgU32 length;
    if(op>=16){
        if(op<32&&tg_u32(t)!=0x1cb5c415UL)return 0;
        length=tg_u32(t);if(length>4096)return 0;count=length;
        for(n=0;n<count;n++)if(!value(t,op%16,depth+1,budget))return 0;
        return t->ok;
    }
    if(op==0)return 1;
    if(op==1)return tg_raw(t,4)!=NULL;
    if(op==2)return tg_raw(t,8)!=NULL;
    if(op==3)return tg_bytes(t,&n)!=NULL;
    if(op==4)return object(t,NULL,depth+1,budget);
    if(op==8)return tg_raw(t,16)!=NULL;
    if(op==9)return tg_raw(t,32)!=NULL;
    return 0;
}
static int object(TgTl *t,TgView *out,unsigned depth,unsigned *budget) {
    TgU32 id,flags[3]={0,0,0};unsigned lo=0,hi=tg_constructor_count,mid,i,at,condition;const TgConstructor *c;const TgRule *r;
    if(depth>24||!*budget)return 0;--*budget;id=tg_u32(t);last_constructor=id;if(!t->ok)return 0;
    while(lo<hi){mid=lo+(hi-lo)/2;if(tg_constructors[mid].id<id)lo=mid+1;else hi=mid;}
    if(lo>=tg_constructor_count||tg_constructors[lo].id!=id)return 0;c=tg_constructors+lo;
    if(out){memset(out,0,sizeof(*out));out->id=id;}
    for(i=0;i<c->count;i++){
        if(!*budget)return 0;--*budget;r=tg_rules+c->start+i;condition=r->condition;
        if(condition&&!(flags[(condition-1)/32]&(1UL<<((condition-1)%32))))continue;
        at=t->at;
        if(r->op>=5&&r->op<=7)flags[r->op-5]=tg_u32(t);
        else if(!value(t,r->op,depth,budget))return 0;
        if(out&&r->key){out->fields[r->key-1].p=t->p+at;out->fields[r->key-1].n=t->at-at;}
    }
    if(out)memcpy(out->flags,flags,sizeof(flags));return t->ok;
}
int tg_view(TgTl *t,TgView *out) {unsigned budget=32000;int ok=object(t,out,0,&budget);if(!ok)t->ok=0;return ok;}
int tg_slice_view(TgSlice s,TgView *v) {TgTl t;if(!s.p)return 0;tg_tl_init(&t,s.p,s.n);return tg_view(&t,v)&&t.at==t.n;}
TgU32 tg_v32(const TgView *v,unsigned field){TgSlice s=v->fields[field];return s.p&&s.n==4?tg_le32(s.p):0;}
TgU64 tg_v64(const TgView *v,unsigned field){TgSlice s=v->fields[field];return s.p&&s.n==8?tg_le64(s.p):0;}
TgSlice tg_vtext(const TgView *v,unsigned field){TgSlice s=v->fields[field];TgTl t;unsigned n;const TgByte *p;if(!s.p)return s;
    tg_tl_init(&t,s.p,s.n);p=tg_bytes(&t,&n);s.p=(TgByte *)p;s.n=p?n:0;return s;}
int tg_vector(TgSlice s,TgTl *t,unsigned *count){TgU32 n;if(!s.p)return 0;tg_tl_init(t,s.p,s.n);
    if(tg_u32(t)!=0x1cb5c415UL)return 0;n=tg_u32(t);if(!t->ok||n>4096)return 0;*count=n;return 1;}
