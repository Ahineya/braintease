/* SPDX-License-Identifier: MIT */
/* Unicode algorithms use octets held in C16 cells; no wchar_t/locale dependency. */
#include "../../runtime/include/ripple_unicode.h"
#include <stdlib.h>
struct ru_range { uint32_t first, last; unsigned value; };
#include "tables.inc"
static unsigned lookup(uint32_t cp, const struct ru_range *table, unsigned n) {
    unsigned low=0, mid;
    while(low<n) { mid=low+(n-low)/2;
        if(cp<table[mid].first)n=mid;
        else if(cp>table[mid].last)low=mid+1;
        else return table[mid].value;
    }
    return 0;
}
#define LOOKUP(name,cp) lookup(cp,ru_##name##_table,sizeof(ru_##name##_table)/sizeof(ru_##name##_table[0]))
int ru_decode(struct ru_decoder *s,unsigned b,uint32_t *out) {
    if(!s->need) {
        if(b<128){*out=b;return 1;}
        if(b>=0xc2&&b<=0xdf){s->value=b&31;s->minimum=0x80;s->need=1;return 0;}
        if(b>=0xe0&&b<=0xef){s->value=b&15;s->minimum=0x800;s->need=2;return 0;}
        if(b>=0xf0&&b<=0xf4){s->value=b&7;s->minimum=0x10000UL;s->need=3;return 0;}
        *out=RU_REPLACEMENT;return 1;
    }
    if(b<0x80||b>0xbf){s->need=0;*out=RU_REPLACEMENT;return -1;}
    if((s->need==2&&s->minimum==0x800&&((s->value==0&&b<0xa0)||(s->value==13&&b>=0xa0)))||
       (s->need==3&&((s->value==0&&b<0x90)||(s->value==4&&b>=0x90)))) {
        s->need=0;*out=RU_REPLACEMENT;return -1;
    }
    s->value=(s->value<<6)|(b&63);
    if(--s->need)return 0;
    *out=s->value;
    if(*out<s->minimum||*out>0x10ffffUL||(*out>=0xd800&&*out<=0xdfff))*out=RU_REPLACEMENT;
    return 1;
}
unsigned ru_encode(uint32_t cp,unsigned char *out) {
    if(cp>0x10ffffUL||(cp>=0xd800&&cp<=0xdfff))cp=RU_REPLACEMENT;
    if(cp<0x80){out[0]=(unsigned)cp;return 1;}
    if(cp<0x800){out[0]=0xc0|(unsigned)(cp>>6);out[1]=0x80|((unsigned)cp&63);return 2;}
    if(cp<0x10000UL){out[0]=0xe0|(unsigned)(cp>>12);out[1]=0x80|((unsigned)(cp>>6)&63);out[2]=0x80|((unsigned)cp&63);return 3;}
    out[0]=0xf0|(unsigned)(cp>>18);out[1]=0x80|((unsigned)(cp>>12)&63);out[2]=0x80|((unsigned)(cp>>6)&63);out[3]=0x80|((unsigned)cp&63);return 4;
}
uint32_t ru_next(const unsigned char *p,size_t n,size_t *at) {
    struct ru_decoder decoder={0,0,0};uint32_t cp=RU_REPLACEMENT;int status;
    while(*at<n){status=ru_decode(&decoder,p[*at],&cp);if(status<0)return cp;++*at;if(status)return cp;}
    return RU_REPLACEMENT;
}
unsigned ru_gcb(uint32_t cp) {
    if(cp<0xa0){if(cp==13)return RU_CR;if(cp==10)return RU_LF;if(cp<32||(cp>=127&&cp<160))return RU_CONTROL;return RU_OTHER;}
    if(cp>=0xac00&&cp<=0xd7a3)return (cp-0xac00)%28?RU_LVT:RU_LV;
    return LOOKUP(gcb,cp);
}
unsigned ru_pictographic(uint32_t cp){return cp>=0xa9?LOOKUP(pictographic,cp):0;}
unsigned ru_incb(uint32_t cp){return cp>=0x300?LOOKUP(incb,cp):0;}
int ru_width(uint32_t cp) {
    unsigned kind;
    if(cp>=32&&cp<127)return 1;
    if(cp>0x10ffffUL||(cp>=0xd800&&cp<=0xdfff))return 1;
    kind=ru_gcb(cp);
    if(kind==RU_CR||kind==RU_LF||kind==RU_CONTROL)return -1;
    if(kind==RU_EXTEND||kind==RU_ZWJ||cp==0x200b||cp==0x200e||cp==0x200f||
       (cp>=0x202a&&cp<=0x202e)||(cp>=0x2060&&cp<=0x206f)||cp==0xfeff)return 0;
    return LOOKUP(wide,cp)?2:1;
}
int ru_join(struct ru_break *s,uint32_t cp) {
    unsigned k=ru_gcb(cp),p=s->previous,ep=ru_pictographic(cp),incb=ru_incb(cp);int join=0;
    if(s->started){
        if(p==RU_CR&&k==RU_LF)join=1;
        else if(p==RU_CR||p==RU_LF||p==RU_CONTROL||k==RU_CR||k==RU_LF||k==RU_CONTROL)join=0;
        else if(p==RU_L&&(k==RU_L||k==RU_V||k==RU_LV||k==RU_LVT))join=1;
        else if((p==RU_LV||p==RU_V)&&(k==RU_V||k==RU_T))join=1;
        else if((p==RU_LVT||p==RU_T)&&k==RU_T)join=1;
        else if(k==RU_EXTEND||k==RU_ZWJ||k==RU_SPACING||p==RU_PREPEND)join=1;
        else if(incb==1&&s->conjunct==2)join=1;
        else if(p==RU_ZWJ&&s->zwj&&ep)join=1;
        else if(p==RU_RI&&k==RU_RI&&(s->regional&1))join=1;
    }
    if(!join){s->regional=s->pictographic=s->zwj=s->conjunct=0;}
    s->regional=k==RU_RI?s->regional+1:0;
    if(k==RU_ZWJ){s->zwj=s->pictographic;s->pictographic=0;}
    else if(k!=RU_EXTEND){s->pictographic=ep;s->zwj=0;}
    if(incb==1)s->conjunct=1;
    else if(incb==3&&s->conjunct)s->conjunct=2;
    else if(incb!=2)s->conjunct=0;
    s->started=1;s->previous=k;return join;
}
size_t ru_next_grapheme(const unsigned char *p,size_t n,size_t start,unsigned *width) {
    struct ru_break state={0,0,0,0,0,0};size_t at=start,saved;uint32_t cp;int w;unsigned total=0;
    if(start<n&&p[start]>=32&&p[start]<127&&(start+1==n||p[start+1]<128)){if(width)*width=1;return start+1;}
    while(at<n){saved=at;cp=ru_next(p,n,&at);if(!ru_join(&state,cp)&&saved!=start){at=saved;break;}
        w=ru_width(cp);if(w>0&&(unsigned)w>total)total=w;
        if(cp==0xfe0f||cp==0x20e3||state.regional>=2)total=2;
    }
    if(width)*width=total;return at;
}
size_t ru_previous_grapheme(const unsigned char *p,size_t n,size_t at) {
    size_t previous=0,next=0;if(at>n)at=n;
    if(at&&p[at-1]>=32&&p[at-1]<127&&(at==1||(p[at-2]>=32&&p[at-2]<127)))return at-1;
    while(next<at){previous=next;next=ru_next_grapheme(p,n,next,NULL);}return previous;
}
size_t ru_prefix(const unsigned char *p,size_t n,size_t columns) {
    size_t at=0,next,used=0;unsigned width;
    while(at<n){
        if(p[at]>=32&&p[at]<127&&(at+1==n||(p[at+1]>=32&&p[at+1]<127))){if(used==columns)break;++used;++at;continue;}
        next=ru_next_grapheme(p,n,at,&width);if(width>columns-used)break;used+=width;at=next;
    }
    return at;
}
size_t ru_columns(const unsigned char *p,size_t n) {
    size_t at=0,total=0;unsigned width;
    while(at<n){if(p[at]>=32&&p[at]<127&&(at+1==n||(p[at+1]>=32&&p[at+1]<127))){++at;++total;}
        else{at=ru_next_grapheme(p,n,at,&width);total+=width;}}
    return total;
}
struct ru_cluster { uint32_t codes[RU_CLUSTER_SCALARS]; unsigned count, width; };
/* Lazily reserve bounded display storage. ASCII, scalar text and width-only
 * clients pay no grapheme-pool startup/zero-fill cost. Never evict live IDs. */
static struct ru_cluster *clusters;
static unsigned cluster_count;
unsigned ru_scalar_count(uint32_t glyph){return glyph>=RU_CLUSTER_BASE&&glyph<RU_CLUSTER_BASE+cluster_count?clusters[(unsigned)(glyph-RU_CLUSTER_BASE)].count:1;}
uint32_t ru_scalar(uint32_t glyph,unsigned index){
    if(glyph>=RU_CLUSTER_BASE&&glyph<RU_CLUSTER_BASE+cluster_count)return index<clusters[(unsigned)(glyph-RU_CLUSTER_BASE)].count?clusters[(unsigned)(glyph-RU_CLUSTER_BASE)].codes[index]:0;
    return index?0:glyph;
}
int ru_glyph_width(uint32_t glyph){return glyph>=RU_CLUSTER_BASE&&glyph<RU_CLUSTER_BASE+cluster_count?clusters[(unsigned)(glyph-RU_CLUSTER_BASE)].width:ru_width(glyph);}
uint32_t ru_append(uint32_t glyph,uint32_t cp) {
    uint32_t codes[RU_CLUSTER_SCALARS];unsigned n=ru_scalar_count(glyph),i,j,width=0,regional=0;int w;
    if(n>=RU_CLUSTER_SCALARS)return RU_REPLACEMENT;
    if(!clusters){clusters=malloc(sizeof(*clusters)*RU_CLUSTER_LIMIT);if(!clusters)return RU_REPLACEMENT;}
    for(i=0;i<n;i++)codes[i]=ru_scalar(glyph,i);codes[n++]=cp;
    for(i=0;i<cluster_count;i++)if(clusters[i].count==n){for(j=0;j<n&&clusters[i].codes[j]==codes[j];j++){}if(j==n)return RU_CLUSTER_BASE+i;}
    if(cluster_count==RU_CLUSTER_LIMIT)return RU_REPLACEMENT;
    i=cluster_count++;
    for(j=0;j<n;j++){clusters[i].codes[j]=codes[j];w=ru_width(codes[j]);if(w>0&&(unsigned)w>width)width=w;if(ru_gcb(codes[j])==RU_RI)regional++;if(codes[j]==0xfe0f||codes[j]==0x20e3||regional>=2)width=2;}
    clusters[i].count=n;clusters[i].width=width;return RU_CLUSTER_BASE+i;
}
