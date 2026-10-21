/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include "pixel.h"
const RiLimits ri_defaults={512,512,65536UL,262144UL,786432UL};
uint32_t ri_be32(const RiByte *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
uint32_t ri_le32(const RiByte *p) { return ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0]; }
unsigned ri_le16(const RiByte *p) { return p[0]|(unsigned)p[1]<<8; }
int ri_size(unsigned w,unsigned h,const RiLimits *limits) {
    if(!limits)limits=&ri_defaults;
    if(!w||!h)return RI_CORRUPT;
    if(w>limits->width||h>limits->height||w>4096||h>4096||
       (uint32_t)w*h>limits->pixels||(uint32_t)w*h>16777216UL)return RI_LIMIT;
    return RI_OK;
}
RiImage *ri_image_new(unsigned w,unsigned h,const RiLimits *limits) {
    RiImage *im;
    if(ri_size(w,h,limits))return NULL;
    im=calloc(1,sizeof(*im));if(!im)return NULL;
    im->rgba=calloc((size_t)w*h,4*sizeof(RiByte));
    if(!im->rgba){free(im);return NULL;}
    im->source_width=im->width=w;im->source_height=im->height=h;return im;
}
void ri_image_free(RiImage *im) { if(im){free(im->rgba);free(im);} }
const char *ri_image_error(int status) {
    switch(status) {
    case RI_OK:return "OK";case RI_FORMAT:return "Unrecognized image format";
    case RI_CORRUPT:return "Corrupt or truncated image";case RI_LIMIT:return "Image exceeds resource limits";
    case RI_MEMORY:return "Not enough memory for image";case RI_UNSUPPORTED:return "Unsupported image encoding";
    default:return "Invalid image status";
    }
}
int ri_image_decode_fit(const RiByte *p,size_t n,const RiLimits *source,const RiLimits *limits,RiImage **out) {
    size_t i;int result;uint32_t count;
    if(!out)return RI_CORRUPT;*out=NULL;
    if(!p||n<2)return RI_CORRUPT;
    if(!limits)limits=&ri_defaults;
    if(!source)source=limits;
    if(n>source->input||n>4194304UL)return RI_LIMIT;
#if CHAR_BIT > 8
    for(i=0;i<n;i++)if(p[i]>255)return RI_CORRUPT;
#endif
    if(n>=8&&p[0]==137&&!memcmp(p+1,"PNG\r\n\032\n",7))result=ri_png(p,n,limits,out);
    else if(n>=6&&(!memcmp(p,"GIF87a",6)||!memcmp(p,"GIF89a",6)))result=ri_gif(p,n,limits,out);
    else if(p[0]=='B'&&p[1]=='M')result=ri_bmp(p,n,limits,out);
    else if(p[0]==255&&p[1]==216)result=ri_jpeg_fit(p,n,source,limits,out);
    else return RI_FORMAT;
    if(result){ri_image_free(*out);*out=NULL;return result;}
    (*out)->opaque=1;count=(uint32_t)(*out)->width*(*out)->height;
    for(i=0;i<count;i++)if((*out)->rgba[i*4+3]!=255){(*out)->opaque=0;break;}
    return RI_OK;
}
uint16_t ri_image_rgb565(const RiByte *p) { return ((uint16_t)(p[0]>>3)<<11)|((uint16_t)(p[1]>>2)<<5)|(p[2]>>3); }
uint16_t ri_image_over565(uint16_t fg,unsigned a,uint16_t bg) { return ri_pixel_over565(fg,a,bg); }

int ri_image_decode(const RiByte *p,size_t n,const RiLimits *limits,RiImage **out) {
    return ri_image_decode_fit(p,n,limits,limits,out);
}
