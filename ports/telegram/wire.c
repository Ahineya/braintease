/* SPDX-License-Identifier: MIT */
#include "core.h"
void tg_erase(void *p,unsigned long n) { volatile TgByte *b=p;while(n--)*b++=0; }
int tg_equal(const TgByte *a,const TgByte *b,unsigned n) { unsigned diff=0;while(n--)diff|=*a++^*b++;return !diff; }
TgU32 tg_le32(const TgByte *p) {return (TgU32)p[0]|(TgU32)p[1]<<8|(TgU32)p[2]<<16|(TgU32)p[3]<<24;}
TgU64 tg_le64(const TgByte *p) {return (TgU64)tg_le32(p)|((TgU64)tg_le32(p+4)<<32);}
void tg_put32(TgByte *p,TgU32 v) {unsigned i;for(i=0;i<4;i++){p[i]=v&255;v>>=8;}}
void tg_put64(TgByte *p,TgU64 v) {unsigned i;for(i=0;i<8;i++){p[i]=v&255;v>>=8;}}
void tg_tl_init(TgTl *t,TgByte *p,unsigned n) {t->p=p;t->n=n;t->at=0;t->ok=1;}
const TgByte *tg_raw(TgTl *t,unsigned n) {
    const TgByte *p;if(!t->ok||n>t->n-t->at){t->ok=0;return NULL;}
    p=t->p+t->at;t->at+=n;return p;
}
TgU32 tg_u32(TgTl *t) {const TgByte *p=tg_raw(t,4);return p?tg_le32(p):0;}
TgU64 tg_u64(TgTl *t) {const TgByte *p=tg_raw(t,8);return p?tg_le64(p):0;}
const TgByte *tg_bytes(TgTl *t,unsigned *size) {
    const TgByte *p,*padding;unsigned n,head=1,pad,i;*size=0;
    p=tg_raw(t,1);if(!p)return NULL;n=*p;
    if(n==254){p=tg_raw(t,3);if(!p)return NULL;
        if(p[2]){t->ok=0;return NULL;}n=p[0]|p[1]<<8;head=4;if(n<254){t->ok=0;return NULL;}}
    else if(n==255){t->ok=0;return NULL;}
    if(n>TG_MAX){t->ok=0;return NULL;}p=tg_raw(t,n);pad=(4-((n+head)&3))&3;
    padding=tg_raw(t,pad);if(!t->ok)return NULL;
    for(i=0;i<pad;i++)if(padding[i]){t->ok=0;return NULL;}
    *size=n;return p;
}
void tg_wraw(TgTl *t,const TgByte *p,unsigned n) {TgByte *out=(TgByte *)tg_raw(t,n);if(out&&n)memcpy(out,p,n);}
void tg_w32(TgTl *t,TgU32 n) {TgByte b[4];tg_put32(b,n);tg_wraw(t,b,4);}
void tg_w64(TgTl *t,TgU64 n) {TgByte b[8];tg_put64(b,n);tg_wraw(t,b,8);}
void tg_wbytes(TgTl *t,const TgByte *p,unsigned n) {
    TgByte h[4]={0,0,0,0};unsigned head=1,pad;if(n>TG_MAX){t->ok=0;return;}
    if(n<254)h[0]=n;else{h[0]=254;h[1]=n&255;h[2]=n>>8;head=4;}
    tg_wraw(t,h,head);tg_wraw(t,p,n);pad=(4-((n+head)&3))&3;
    memset(h,0,4);tg_wraw(t,h,pad);
}
void tg_wtext(TgTl *t,const char *s) {tg_wbytes(t,(const TgByte *)s,strlen(s));}
