/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable, bounded bitmap strikes. No host font calls or guest font parser. */
#include "../../../runtime/include/ripple_unicode.h"
#include "../fonts/compose.inc"
static unsigned char *nf_data;
static unsigned nf_count;
static unsigned long nf_records;
static unsigned char *nf_checked;
static unsigned nf_u16(const unsigned char *p) { return p[0]+p[1]*256U; }
static unsigned long nf_u32(const unsigned char *p) { return nf_u16(p)+(unsigned long)nf_u16(p+2)*65536UL; }
static int nf_signed(unsigned v) { return v<128?(int)v:(int)v-256; }
static bool nf_load(const char *path)
{
    FILE *file=fopen(path,"rb");long bytes;unsigned long i,at,length;unsigned char *data;
    if(!file)return false;
    if(fseek(file,0,SEEK_END)|| (bytes=ftell(file))<20 || bytes>4194304L || fseek(file,0,SEEK_SET)) { fclose(file);return false; }
    data=malloc(bytes);if(!data){fclose(file);return false;}
    if(fread(data,1,bytes,file)!=(size_t)bytes){free(data);fclose(file);return false;}fclose(file);
    for(i=0;i<20;i++)if(data[i]>255) { free(data);return false; }
    if(memcmp(data,"RBFONT01",8)||nf_u16(data+8)!=12||nf_u16(data+10)!=2||nf_u16(data+14)||nf_u32(data+16)!=(unsigned long)bytes)goto bad;
    nf_count=nf_u16(data+12);nf_records=20UL+nf_count*2UL;
    if(!nf_count||nf_count>2048||nf_records+nf_count*288UL>(unsigned long)bytes)goto bad;
    for(i=20;i<nf_records;i++)if(data[i]>255)goto bad;
    for(i=1;i<nf_count;i++)if(nf_u16(data+20+i*2)<=nf_u16(data+18+i*2))goto bad;
    for(i=0;i<nf_count*24UL;i++) {
        const unsigned char *r=data+nf_records+i*12;unsigned j;
        for(j=0;j<12;j++)if(r[j]>255)goto bad;
        at=nf_u32(r);length=nf_u16(r+4);
        if(r[11])goto bad;
        if(at==0xffffffffUL)continue;
        if(at<nf_records+nf_count*288UL||at>(unsigned long)bytes||length>(unsigned long)bytes-at||r[8]>64||r[9]>64||r[10]>64)goto bad;
        /* RLE contents are checked once when this glyph is first used. The
         * entire directory is still validated before any record is accessed. */
    }
    free(nf_checked);nf_checked=calloc((nf_count*24UL+7)/8,1);
    if(!nf_checked)goto bad;
    nf_data=data;return true;
bad:
    free(data);nf_count=0;return false;
}
static bool nf_validate(const unsigned char *r) {
    unsigned long record=(r-(nf_data+nf_records))/12,at,length,pixels=0,i;
    unsigned bit=1U<<(record&7);
    if(nf_checked[record/8]&bit)return true;
    at=nf_u32(r);length=nf_u16(r+4);
    for(i=0;i<length;i++) {
        unsigned value=nf_data[at+i];
        if(value>255)return false;
        pixels+=(value>>4)+1;
        if(pixels>(unsigned long)r[8]*r[9])return false;
    }
    if(pixels!=(unsigned long)r[8]*r[9])return false;
    nf_checked[record/8]|=bit;return true;
}
struct nf_style { unsigned face, size; };
static struct nf_style nf_style(const plot_font_style_t *style)
{
    struct nf_style f={0,16};long px;
    if(!style)return f;
    if(style->family==PLOT_FONT_FAMILY_SERIF)f.face=4;
    if(style->family==PLOT_FONT_FAMILY_MONOSPACE)f.face=8;
    if(style->weight>=600)f.face++;
    if(style->flags&(FONTF_ITALIC|FONTF_OBLIQUE))f.face+=2;
    /* NetSurf supplies 22:10 points; 96 CSS pixels / 72 points. */
    px=style->size>49152L?64:(style->size+384)/768;
    if(style->size>0)f.size=px<8?8:px>64?64:(unsigned)px;
    return f;
}
static int nf_index(uint32_t cp)
{
    unsigned lo=0,hi=nf_count,mid,v;
    while(lo<hi){mid=lo+(hi-lo)/2;v=nf_u16(nf_data+20+mid*2UL);if(cp<v)hi=mid;else if(cp>v)lo=mid+1;else return mid;}
    return -1;
}
static uint32_t nf_compose(uint32_t base,uint32_t mark)
{
    unsigned lo=0,hi=sizeof(nf_pairs)/sizeof(nf_pairs[0]),mid;
    while(lo<hi) {
        mid=lo+(hi-lo)/2;
        if(base<nf_pairs[mid][0]||(base==nf_pairs[mid][0]&&mark<nf_pairs[mid][1]))hi=mid;
        else if(base>nf_pairs[mid][0]||mark>nf_pairs[mid][1])lo=mid+1;
        else return nf_pairs[mid][2];
    }
    return 0;
}
static uint32_t nf_cluster(const char *s,size_t n,size_t at,size_t *next,unsigned *cells)
{
    uint32_t cp,mark,composed;size_t pos=at;
    *next=ru_next_grapheme((const unsigned char *)s,n,at,cells);
    cp=ru_next((const unsigned char *)s,*next,&pos);
    while(pos<*next) {
        mark=ru_next((const unsigned char *)s,*next,&pos);composed=nf_compose(cp,mark);
        cp=composed?composed:ru_append(cp,mark);
    }
    return cp;
}
struct nf_glyph { int left,top;unsigned width,height,advance;const unsigned char *record; };
static int nf_scale(int value,unsigned size,unsigned base)
{ return value<0?-((-(long)value*size+base/2)/base):((long)value*size+base/2)/base; }
static struct nf_glyph nf_glyph(struct nf_style f,uint32_t cp,unsigned cells)
{
    struct nf_glyph g;int index=nf_index(cp);unsigned strike=f.size<=20?0:1,base=strike?32:16;
    memset(&g,0,sizeof(g));
    if(index>=0) {
        const unsigned char *r=nf_data+nf_records+((f.face*2UL+strike)*nf_count+index)*12;
        if(nf_u32(r)!=0xffffffffUL&&nf_validate(r)) {
            g.record=r;g.left=nf_scale(nf_signed(r[6]),f.size,base);g.top=nf_scale(nf_signed(r[7]),f.size,base);
            g.width=nf_scale(r[8],f.size,base);g.height=nf_scale(r[9],f.size,base);g.advance=nf_scale(r[10],f.size,base);
            return g;
        }
    }
    /* ru_width returns -1 for controls (including Unicode line separators).
     * Atlas preparation visits them too; never reinterpret that as a width. */
    if(cells>2)cells=0;
    g.width=g.advance=(cells*f.size+1)/2;g.height=cells?f.size:0;g.top=-(f.size*3/4);return g;
}
static unsigned nf_advance(const plot_font_style_t *style,const char *s,size_t n,size_t at,size_t *next)
{
    unsigned cells;uint32_t cp=nf_cluster(s,n,at,next,&cells);
    return nf_glyph(nf_style(style),cp,cells).advance;
}
#define TEXT_ADVANCE nf_advance

static unsigned nf_page_size(unsigned size)
{ return size<=18?96:size<=20?48:size<=32?24:size<=40?12:4; }
