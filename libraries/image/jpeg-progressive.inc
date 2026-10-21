/* SPDX-License-Identifier: MIT */
/* Progressive JPEG uses the pinned portable stb decoder. Keep the small MCU
 * baseline path for streaming/scaled decode without a full coefficient plane. */
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ASSERT(x) ((void)0)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb-codec.inc"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* Validate the complete marker/entropy envelope before stb is allowed to
 * synthesize any end-of-buffer reads. Bound repeated progressive scans too. */
static int jpeg_envelope(const RiByte *p,size_t n,unsigned *w,unsigned *h,unsigned *progressive) {
    size_t at=2,end;unsigned marker,length,scans=0,frame=0,components=0;
    while(at<n) {
        if(p[at++]!=255)return RI_CORRUPT;
        while(at<n&&p[at]==255)++at;
        if(at>=n)return RI_CORRUPT;
        marker=p[at++];
        if(marker==217)return frame&&scans&&at==n?RI_OK:RI_CORRUPT;
        if(!marker||marker==216||marker==1||(marker>=208&&marker<=215))return RI_CORRUPT;
        if(n-at<2)return RI_CORRUPT;
        length=(unsigned)p[at]*256U+p[at+1];
        if(length<2||length>n-at)return RI_CORRUPT;
        end=at+length;
        if(marker==192||marker==194) {
            if(frame||length<8||p[at+2]!=8)return RI_UNSUPPORTED;
            *h=(unsigned)p[at+3]*256U+p[at+4];*w=(unsigned)p[at+5]*256U+p[at+6];
            components=p[at+7];
            if((components!=1&&components!=3)||length!=8+3*components)return RI_UNSUPPORTED;
            *progressive=marker==194;frame=1;
        } else if(marker>=192&&marker<=207&&marker!=196&&marker!=204)return RI_UNSUPPORTED;
        if(marker==218) {
            if(!frame||length<6||!p[at+2]||p[at+2]>components||length!=6+2*p[at+2])return RI_CORRUPT;
            if(++scans>64)return RI_LIMIT;
            at=end;
            while(at<n) {
                if(p[at++]!=255)continue;
                while(at<n&&p[at]==255)++at;
                if(at>=n)return RI_CORRUPT;
                marker=p[at];
                if(!marker||(marker>=208&&marker<=215)){++at;continue;}
                --at;break;
            }
        } else at=end;
    }
    return RI_CORRUPT;
}
static int ri_jpeg_progressive(const RiByte *p,size_t n,unsigned w,unsigned h,
    unsigned scale,const RiLimits *limits,RiImage **out) {
    uint32_t padded=((uint32_t)w+31)/32*32*((h+31U)/32U*32U);
    uint32_t estimate=padded*3*(sizeof(RiByte)+sizeof(short))+
        (uint32_t)w*h*4*sizeof(RiByte)+(uint32_t)w*12+sizeof(stbi__jpeg)+256;
    int32_t x=0,y=0,components=0;RiByte *pixels;RiImage *im;
    stbi__context context;stbi__jpeg *decoder;
    unsigned ow=(w+scale-1)/scale,oh=(h+scale-1)/scale,xx,yy;
    /* Conservative peak, including padded component/coefficient buffers,
     * full RGBA output and the final reduced bitmap. No process-global budget. */
    if(estimate+(uint32_t)ow*oh*4>limits->work)return RI_LIMIT;
    decoder=calloc(1,sizeof(*decoder));if(!decoder)return RI_MEMORY;
    stbi__start_mem(&context,p,n);decoder->s=&context;stbi__setup_jpeg(decoder);
    pixels=load_jpeg_image(decoder,&x,&y,&components,4);
    free(decoder);
    if(!pixels)return RI_CORRUPT;
    if(x!=(int32_t)w||y!=(int32_t)h){free(pixels);return RI_CORRUPT;}
    im=ri_image_new(ow,oh,limits);if(!im){free(pixels);return RI_MEMORY;}
    for(yy=0;yy<oh;yy++)for(xx=0;xx<ow;xx++)
        memcpy(im->rgba+((size_t)yy*ow+xx)*4,pixels+((size_t)yy*scale*w+xx*scale)*4,4);
    free(pixels);im->source_width=w;im->source_height=h;*out=im;return RI_OK;
}
