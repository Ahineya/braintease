/* SPDX-License-Identifier: MIT */
#include "internal.h"
struct lzw { uint16_t prefix[4096];RiByte suffix[4096],stack[4097]; };
static int subblocks(const RiByte *p,size_t n,size_t *at,RiByte *output,size_t *used) {
    unsigned count;*used=0;
    for(;;) {
        if(*at>=n)return RI_CORRUPT;
        count=p[(*at)++];if(!count)return RI_OK;
        if(count>n-*at)return RI_CORRUPT;
        if(output)memcpy(output+*used,p+*at,count);
        *used+=count;*at+=count;
    }
}
static int gif_pixels(const RiByte *data,size_t n,unsigned min,struct lzw *dict,
                     RiImage *im,unsigned left,unsigned top,unsigned w,unsigned h,
                     const RiByte *palette,unsigned entries,int transparent,unsigned interlace) {
    uint32_t tag=0,produced=0,total=(uint32_t)w*h;
    size_t at=0;unsigned have=0,bits=min+1,clear=1U<<min,end=clear+1,next=end+1;
    unsigned old=4096,code,in,first=0,length,i,x=0,y=0,pass=0,started=0;
    RiByte *dest;
    static const unsigned starts[4]={0,4,2,1},steps[4]={8,8,4,2};
    for(;;) {
        while(have<bits){if(at==n)return RI_CORRUPT;tag|=(uint32_t)data[at++]<<have;have+=8;}
        code=tag&((1U<<bits)-1);tag>>=bits;have-=bits;
        if(!started&&code!=clear)return RI_CORRUPT;started=1;
        if(code==clear){bits=min+1;next=end+1;old=4096;continue;}
        if(code==end)return produced==total?RI_OK:RI_CORRUPT;
        in=code;length=0;
        if(old==4096){if(code>=clear)return RI_CORRUPT;}
        else if(code==next){dict->stack[length++]=first;code=old;}
        else if(code>next)return RI_CORRUPT;
        while(code>=clear) {
            if(code>=next||length>=4096)return RI_CORRUPT;
            dict->stack[length++]=dict->suffix[code];code=dict->prefix[code];
        }
        first=code;dict->stack[length++]=first;
        if(length>total-produced)return RI_CORRUPT;
        while(length) {
            i=dict->stack[--length];if(i>=entries||y>=h)return RI_CORRUPT;
            dest=im->rgba+((size_t)(top+y)*im->width+left+x)*4;
            if((int)i!=transparent){dest[0]=palette[i*3];dest[1]=palette[i*3+1];dest[2]=palette[i*3+2];dest[3]=255;}
            produced++;if(++x==w) {
                x=0;y+=interlace?steps[pass]:1;
                if(interlace)while(y>=h&&pass<3)y=starts[++pass];
            }
        }
        if(old!=4096&&next<4096) {
            dict->prefix[next]=old;dict->suffix[next]=first;next++;
            if(next==(1U<<bits)&&bits<12)bits++;
        }
        old=in;
    }
}
int ri_gif(const RiByte *p,size_t n,const RiLimits *limits,RiImage **out) {
    size_t at=13,used=0;uint32_t pixel_index;unsigned w,h,global=0,entries,packed,left,top,fw,fh,min,frames=0,tag,bg;
    int transparent=-1,status=RI_CORRUPT;RiByte global_pal[768],local_pal[768],*compressed=NULL;
    const RiByte *palette;RiImage *im=NULL;struct lzw *dict=NULL;
    if(n<13)return RI_CORRUPT;
    w=ri_le16(p+6);h=ri_le16(p+8);status=ri_size(w,h,limits);if(status)return status;status=RI_CORRUPT;
    packed=p[10];bg=p[11];
    if(packed&128) {
        global=1U<<((packed&7)+1);
        if((size_t)global*3>n-at||bg>=global)return RI_CORRUPT;
        memcpy(global_pal,p+at,global*3);at+=global*3;
    }
    if(n>limits->work||sizeof(*dict)>limits->work-n)return RI_LIMIT;
    im=ri_image_new(w,h,limits);dict=malloc(sizeof(*dict));compressed=malloc(n);
    if(!im||!dict||!compressed){status=RI_MEMORY;goto done;}
    while(at<n) {
        tag=p[at++];
        if(tag==0x3b){if(!frames||at!=n)goto done;*out=im;im=NULL;status=RI_OK;goto done;}
        if(tag==0x21) {
            if(at>=n)goto done;tag=p[at++];
            if(tag==0xf9) {
                if(n-at<6||p[at]!=4||p[at+5]||(p[at+1]&0xe0))goto done;
                transparent=p[at+1]&1?p[at+4]:-1;at+=6;
            } else {
                if(tag==1){status=RI_UNSUPPORTED;goto done;}
                if(subblocks(p,n,&at,NULL,&used))goto done;
            }
        } else if(tag==0x2c) {
            if(n-at<9)goto done;
            left=ri_le16(p+at);top=ri_le16(p+at+2);fw=ri_le16(p+at+4);fh=ri_le16(p+at+6);packed=p[at+8];at+=9;
            if(!fw||!fh||(uint32_t)left+fw>w||(uint32_t)top+fh>h||(packed&0x18))goto done;
            entries=global;palette=global_pal;
            if(packed&128) {
                entries=1U<<((packed&7)+1);if((size_t)entries*3>n-at)goto done;
                memcpy(local_pal,p+at,entries*3);palette=local_pal;at+=entries*3;
            }
            if(!entries||transparent>=(int)entries||at>=n)goto done;
            min=p[at++];if(min<2||min>8)goto done;
            if(subblocks(p,n,&at,frames?NULL:compressed,&used))goto done;
            if(!frames) {
                if(transparent<0&&global)for(pixel_index=0;pixel_index<(uint32_t)w*h;pixel_index++) {
                    RiByte *d=im->rgba+(size_t)pixel_index*4;
                    d[0]=global_pal[bg*3];d[1]=global_pal[bg*3+1];d[2]=global_pal[bg*3+2];d[3]=255;
                }
                status=gif_pixels(compressed,used,min,dict,im,left,top,fw,fh,palette,entries,transparent,packed&64);
                if(status)goto done;status=RI_CORRUPT;
            }
            frames++;transparent=-1;
        } else goto done;
    }
 done:ri_image_free(im);free(dict);free(compressed);return status;
}
