/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include "../deflate/deflate.h"
static uint32_t crc32(const RiByte *p,size_t n) {
    static const uint32_t table[16]={0,0x1db71064UL,0x3b6e20c8UL,0x26d930acUL,
        0x76dc4190UL,0x6b6b51f4UL,0x4db26158UL,0x5005713cUL,0xedb88320UL,
        0xf00f9344UL,0xd6d6a3e8UL,0xcb61b38cUL,0x9b64c2b0UL,0x86d3d2d4UL,0xa00ae278UL,0xbdbdf21cUL};
    uint32_t c=0xffffffffUL;
    while(n--){c^=*p++;c=(c>>4)^table[c&15];c=(c>>4)^table[c&15];}
    return c^0xffffffffUL;
}
static uint32_t adler(const RiByte *p,uint32_t n) {
    uint32_t a=1,b=0,take,i;
    while(n){take=n>5552?5552:n;n-=take;for(i=0;i<take;i++){a+=*p++;b+=a;}a%=65521UL;b%=65521UL;}
    return (b<<16)|a;
}
static unsigned sample(const RiByte *p,unsigned index,unsigned depth) {
    uint32_t bit=(uint32_t)index*depth;
    if(depth==16)return (unsigned)p[index*2]<<8|p[index*2+1];
    if(depth==8)return p[index];
    return (p[bit/8]>>(8-depth-bit%8))&((1U<<depth)-1);
}
static unsigned channel(unsigned value,unsigned depth) { return depth==8?value:depth==16?value>>8:value*255/((1U<<depth)-1); }
static unsigned paeth(unsigned a,unsigned b,unsigned c) {
    long p=(long)a+b-c,pa=labs(p-a),pb=labs(p-b),pc=labs(p-c);
    return pa<=pb&&pa<=pc?a:pb<=pc?b:c;
}
/* Choose the predictor once per scanline. In particular, filter zero must not
 * read every byte of the previous row only to discard it. Keep reconstruction
 * in place: later bytes in Sub/Average/Paeth depend on already restored bytes. */
static void unfilter(RiByte *row,const RiByte *previous,unsigned size,unsigned bpp,unsigned filter) {
    unsigned i,a,b,c;
    if(filter==1) {
        for(i=bpp;i<size;i++)row[i]=(row[i]+row[i-bpp])&255;
    } else if(filter==2) {
        if(previous)for(i=0;i<size;i++)row[i]=(row[i]+previous[i])&255;
    } else if(filter==3) {
        for(i=0;i<size;i++) {
            a=i>=bpp?row[i-bpp]:0;b=previous?previous[i]:0;
            row[i]=(row[i]+(a+b)/2)&255;
        }
    } else if(filter==4) {
        for(i=0;i<size;i++) {
            a=i>=bpp?row[i-bpp]:0;b=previous?previous[i]:0;c=previous&&i>=bpp?previous[i-bpp]:0;
            row[i]=(row[i]+paeth(a,b,c))&255;
        }
    }
}
int ri_png(const RiByte *p,size_t n,const RiLimits *limits,RiImage **out) {
    size_t at=8,offset=0;uint32_t len,w=0,h=0,packed=0,total=0,rowsize,passw,passh,used;
    unsigned depth=0,type=0,channels=0,interlace=0,palette=0,transparency=0,idat=0,ended=0,finish=0;
    unsigned i,j,pass,count,bpp,x,y,sx,sy,r,g,b,a,gray,filter,plte_seen=0,trns_seen=0;
    unsigned tr[3]={0,0,0};RiByte pal[256*4],*compressed=NULL,*raw=NULL,*row,*previous,*pixel;
    int status=RI_CORRUPT;RiImage *im=NULL;
    static const unsigned xs[7]={0,4,0,2,0,1,0},ys[7]={0,0,4,0,2,0,1};
    static const unsigned xd[7]={8,8,4,4,2,2,1},yd[7]={8,8,8,4,4,2,2};
    for(i=0;i<256;i++)pal[i*4+3]=255;
    while(at<n) {
        if(n-at<12)goto done;
        len=ri_be32(p+at);if(len>n-at-12)goto done;
        for(i=0;i<4;i++)if(!((p[at+4+i]>='A'&&p[at+4+i]<='Z')||(p[at+4+i]>='a'&&p[at+4+i]<='z')))goto done;
        if(p[at+6]&32)goto done;
        if(crc32(p+at+4,len+4)!=ri_be32(p+at+8+len))goto done;
        if(!memcmp(p+at+4,"IHDR",4)) {
            if(at!=8||len!=13)goto done;
            w=ri_be32(p+at+8);h=ri_be32(p+at+12);
            if(w>4096||h>4096){status=RI_LIMIT;goto done;}
            status=ri_size(w,h,limits);if(status)goto done;status=RI_CORRUPT;
            depth=p[at+16];type=p[at+17];interlace=p[at+20];
            if(p[at+18]||p[at+19]||interlace>1){status=RI_UNSUPPORTED;goto done;}
            if(type==0)channels=1;else if(type==2)channels=3;else if(type==3)channels=1;
            else if(type==4)channels=2;else if(type==6)channels=4;else goto done;
            if(!(depth==1||depth==2||depth==4||depth==8||depth==16)||
               (type==3&&depth==16)||(type!=0&&type!=3&&depth<8))goto done;
        } else {
            if(!w)goto done;
            if(!memcmp(p+at+4,"PLTE",4)) {
                if(idat||plte_seen||trns_seen||type==0||type==4||!len||len%3||len>768)goto done;
                palette=len/3;plte_seen=1;
                if(type==3&&palette>(1U<<depth))goto done;
                for(i=0;i<palette;i++)for(j=0;j<3;j++)pal[i*4+j]=p[at+8+i*3+j];
            } else if(!memcmp(p+at+4,"tRNS",4)) {
                if(idat||trns_seen)goto done;trns_seen=1;transparency=1;
                if(type==3) {
                    if(!palette||!len||len>palette)goto done;
                    for(i=0;i<len;i++)pal[i*4+3]=p[at+8+i];
                } else if(type==0||type==2) {
                    if(len!=(type==0?2:6))goto done;
                    for(i=0;i<len/2;i++){tr[i]=(unsigned)p[at+8+i*2]<<8|p[at+9+i*2];if(depth<16&&tr[i]>((1U<<depth)-1))goto done;}
                } else goto done;
            } else if(!memcmp(p+at+4,"IDAT",4)) {
                if(ended||(type==3&&!palette))goto done;
                idat=1;packed+=len;
            } else if(!memcmp(p+at+4,"IEND",4)) {
                if(len||!idat||at+12!=n)goto done;
                finish=1;break;
            } else if(!(p[at+4]&32)){status=RI_UNSUPPORTED;goto done;}
            if(idat&&memcmp(p+at+4,"IDAT",4))ended=1;
        }
        at+=len+12;
    }
    if(!finish||packed<6)goto done;
    count=interlace?7:1;bpp=(channels*depth+7)/8;
    for(pass=0;pass<count;pass++) {
        sx=interlace?xs[pass]:0;sy=interlace?ys[pass]:0;
        passw=w>sx?(w-sx+(interlace?xd[pass]:1)-1)/(interlace?xd[pass]:1):0;
        passh=h>sy?(h-sy+(interlace?yd[pass]:1)-1)/(interlace?yd[pass]:1):0;
        if(passw&&passh)total+=((passw*channels*depth+7)/8+1)*passh;
    }
    if(packed>limits->work||total>limits->work-packed){status=RI_LIMIT;goto done;}
    compressed=malloc(packed);raw=malloc(total);
    if(!compressed||!raw){status=RI_MEMORY;goto done;}
    for(at=8;at<n;at+=len+12) {
        len=ri_be32(p+at);
        if(!memcmp(p+at+4,"IDAT",4)){memcpy(compressed+offset,p+at+8,len);offset+=len;}
    }
    if((compressed[0]&15)!=8||(compressed[0]>>4)>7||compressed[1]&32||
       (((unsigned)compressed[0]<<8)|compressed[1])%31)goto done;
    used=total;
    if(ri_deflate(raw,&used,compressed+2,packed-6)!=TINF_OK||used!=total||adler(raw,total)!=ri_be32(compressed+packed-4))goto done;
    free(compressed);compressed=NULL;
    im=ri_image_new(w,h,limits);if(!im){status=RI_MEMORY;goto done;}
    offset=0;
    for(pass=0;pass<count;pass++) {
        sx=interlace?xs[pass]:0;sy=interlace?ys[pass]:0;
        passw=w>sx?(w-sx+(interlace?xd[pass]:1)-1)/(interlace?xd[pass]:1):0;
        passh=h>sy?(h-sy+(interlace?yd[pass]:1)-1)/(interlace?yd[pass]:1):0;
        if(!passw||!passh)continue;
        rowsize=(passw*channels*depth+7)/8;previous=NULL;
        for(y=0;y<passh;y++) {
            filter=raw[offset++];row=raw+offset;offset+=rowsize;
            if(filter>4)goto done;
            unfilter(row,previous,(unsigned)rowsize,bpp,filter);
            /* Most web PNGs have contiguous 8-bit RGB/RGBA rows. Avoid
             * per-channel sample/depth conversions and per-pixel wide address
             * multiplication; Adam7 and colour-key transparency still use the
             * general path below. memcpy counts C cells on both C8 and C16. */
            if(!interlace&&depth==8&&(type==6||(type==2&&!transparency))) {
                pixel=im->rgba+(size_t)y*w*4;
                if(type==6)memcpy(pixel,row,(size_t)w*4);
                else {
                    const RiByte *rgb=row;
                    for(x=0;x<w;x++) {
                        pixel[0]=rgb[0];pixel[1]=rgb[1];pixel[2]=rgb[2];pixel[3]=255;
                        pixel+=4;rgb+=3;
                    }
                }
                previous=row;
                continue;
            }
            for(x=0;x<passw;x++) {
                a=255;
                if(type==3) {
                    gray=sample(row,x,depth);if(gray>=palette)goto done;
                    r=pal[gray*4];g=pal[gray*4+1];b=pal[gray*4+2];a=pal[gray*4+3];
                } else if(type==0||type==4) {
                    gray=sample(row,x*channels,depth);r=g=b=channel(gray,depth);
                    if(type==4)a=channel(sample(row,x*channels+1,depth),depth);
                    else if(transparency&&gray==tr[0])a=0;
                } else {
                    r=sample(row,x*channels,depth);g=sample(row,x*channels+1,depth);b=sample(row,x*channels+2,depth);
                    if(type==6)a=channel(sample(row,x*channels+3,depth),depth);
                    else if(transparency&&r==tr[0]&&g==tr[1]&&b==tr[2])a=0;
                    r=channel(r,depth);g=channel(g,depth);b=channel(b,depth);
                }
                pixel=im->rgba+((size_t)(sy+y*(interlace?yd[pass]:1))*w+sx+x*(interlace?xd[pass]:1))*4;
                pixel[0]=r;pixel[1]=g;pixel[2]=b;pixel[3]=a;
            }
            previous=row;
        }
    }
    *out=im;im=NULL;status=RI_OK;
 done:free(compressed);free(raw);ri_image_free(im);return status;
}
