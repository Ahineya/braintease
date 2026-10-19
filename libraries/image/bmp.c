/* SPDX-License-Identifier: MIT */
#include "internal.h"
int ri_bmp(const RiByte *p,size_t n,const RiLimits *limits,RiImage **out) {
    uint32_t header,w,h,offset,stride,total,colour_count=0,start,y,x;unsigned bits,top=0,index,entries=0;
    int status;RiImage *im;const RiByte *row,*c;RiByte *dest;
    if(n<26)return RI_CORRUPT;
    header=ri_le32(p+14);offset=ri_le32(p+10);
    if(header==12){w=ri_le16(p+18);h=ri_le16(p+20);bits=ri_le16(p+24);if(ri_le16(p+22)!=1)return RI_CORRUPT;}
    else if(header>=40&&header<=124) {
        if(n<14+header)return RI_CORRUPT;
        w=ri_le32(p+18);h=ri_le32(p+22);bits=ri_le16(p+28);
        if(w&0x80000000UL)return RI_CORRUPT;
        if(h&0x80000000UL){top=1;h=(~h+1)&0xffffffffUL;}
        if(ri_le16(p+26)!=1)return RI_CORRUPT;
        if(ri_le32(p+30))return RI_UNSUPPORTED;
        colour_count=ri_le32(p+46);
    } else return RI_UNSUPPORTED;
    if(w>4096||h>4096)return RI_LIMIT;
    status=ri_size(w,h,limits);if(status)return status;
    if(bits!=1&&bits!=4&&bits!=8&&bits!=24&&bits!=32)return RI_UNSUPPORTED;
    if(bits<=8){if(colour_count>(1U<<bits))return RI_CORRUPT;entries=colour_count?colour_count:1U<<bits;}
    start=14+header+(uint32_t)entries*(header==12?3:4);
    stride=((w*bits+31)/32)*4;total=stride*h;
    if(start>n||offset<start||offset>n||total>n-offset)return RI_CORRUPT;
    im=ri_image_new(w,h,limits);if(!im)return RI_MEMORY;
    for(y=0;y<h;y++) {
        row=p+offset+(top?y:h-1-y)*stride;
        for(x=0;x<w;x++) {
            dest=im->rgba+((size_t)y*w+x)*4;
            if(bits<=8) {
                index=(row[x*bits/8]>>(8-bits-(x*bits)%8))&((1U<<bits)-1);
                if(index>=entries){ri_image_free(im);return RI_CORRUPT;}
                c=p+14+header+index*(header==12?3:4);
            } else c=row+x*(bits/8);
            dest[0]=c[2];dest[1]=c[1];dest[2]=c[0];dest[3]=255;
        }
    }
    *out=im;return RI_OK;
}
