/* SPDX-License-Identifier: GPL-2.0-only */
/* NetSurf owns fetching/layout; the shared image library owns RGBA decoding. */
#include <netsurf_profile.h>
#include "content.h"
#include "image-limits.h"
#include <stdlib.h>
#include "utils/utils.h"
#include "utils/nsurl.h"
#include "netsurf/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "content/handlers/image/image.h"
#include "netsurf/plotters.h"
#include "../../../libraries/image/image.h"
typedef struct ImageContent {
    struct content base;RiImage *image;uint32_t pixels;
    size_t probe_at;
    bool probed,listed;
    struct ImageContent *next,*prev;
    unsigned long used;
    uint32_t requested_pixels;
} ImageContent;
/* Across the complete cache, not per URL. A web page cannot allocate one
 * full-size bitmap per image without bound. Four RGBA cells per pixel. */
static uint32_t pixel_budget, image_pixels, source_limit, work_limit;
static uint32_t used_pixels;
static ImageContent *images;
static unsigned long image_clock;
static void (*dispose_image)(const RiImage *);
void netsurf_images_dispose(void (*callback)(const RiImage *)) { dispose_image=callback; }
static void track(ImageContent *im) {
    im->next=images;if(images)images->prev=im;images=im;im->listed=true;
}
static void discard(ImageContent *im) {
    if(!im->image)return;
    if(dispose_image)dispose_image(im->image);
    used_pixels-=im->pixels;im->pixels=0;
    ri_image_free(im->image);im->image=NULL;
}
static void make_room(ImageContent *current,uint32_t need) {
    while(need>pixel_budget-used_pixels) {
        ImageContent *im,*old=NULL;
        for(im=images;im;im=im->next)
            if(im!=current&&im->image&&(!old||im->used<old->used))old=im;
        if(!old)break;
        if(getenv("NETSURF_TRACE_IMAGES"))fprintf(stderr,"NETSURF_IMAGE_EVICT pixels=%lu\n",(unsigned long)old->pixels);
        discard(old);
    }
}
static nserror create(const struct content_handler *handler,lwc_string *mime,
 const struct http_parameter *params,struct llcache_handle *cache,
 const char *charset,bool quirks,struct content **out) {
    ImageContent *im=calloc(1,sizeof(*im));nserror error;
    if(!im)return NSERROR_NOMEM;
    error=content__init(&im->base,handler,mime,params,cache,charset,quirks);
    if(error!=NSERROR_OK){free(im);return error;}track(im);*out=&im->base;return NSERROR_OK;
}
static uint32_t dimension(const ns_octet_t *p,bool little) {
    if(little)return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    return (uint32_t)p[3]|((uint32_t)p[2]<<8)|((uint32_t)p[1]<<16)|((uint32_t)p[0]<<24);
}
/* Refuse impossible images from their prefix, rather than fetching their
 * complete compressed source before discovering the dimensions/format. This
 * is a resource probe; the decoder still validates the complete container. */
static bool process(struct content *c,const char *chunk,unsigned int size) {
    ImageContent *im=(ImageContent *)c;
    const ns_octet_t *p;size_t n,at,end;
    uint32_t w=0,h=0;unsigned marker,length;bool jpeg=false;
    int error=RI_OK;
    if(im->probed)return true;
    p=content__get_source_data(c,&n);
    if(n<12)return true;
    if(p[0]==137&&!memcmp(p+1,"PNG\r\n\032\n",7)) {
        if(n<24)return true;
        w=dimension(p+16,false);h=dimension(p+20,false);
    } else if(!memcmp(p,"GIF87a",6)||!memcmp(p,"GIF89a",6)) {
        w=p[6]|(uint32_t)p[7]<<8;h=p[8]|(uint32_t)p[9]<<8;
    } else if(p[0]=='B'&&p[1]=='M') {
        if(n<26)return true;
        if(dimension(p+14,true)==12) {
            w=p[18]|(uint32_t)p[19]<<8;h=p[20]|(uint32_t)p[21]<<8;
        } else {
            w=dimension(p+18,true);h=dimension(p+22,true);
            if(h&0x80000000UL)h=0UL-h;
        }
    } else if(p[0]==255&&p[1]==216) {
        jpeg=true;
        at=im->probe_at?im->probe_at:2;
        for(;;) {
            im->probe_at=at;
            if(at>65536UL) {error=RI_LIMIT;break;}
            if(n-at<4)return true;
            if(p[at]!=255) {error=RI_CORRUPT;break;}
            while(at<n&&p[at]==255)++at;
            if(n-at<3)return true;
            marker=p[at++];length=(unsigned)p[at]<<8|p[at+1];
            if(length<2) {error=RI_CORRUPT;break;}
            if(marker>=192&&marker<=207&&marker!=196&&marker!=200&&marker!=204) {
                if(marker!=192&&marker!=194) {error=RI_UNSUPPORTED;break;}
                if(length<8) {error=RI_CORRUPT;break;}
                if(n-at<8)return true;
                h=(unsigned)p[at+3]<<8|p[at+4];w=(unsigned)p[at+5]<<8|p[at+6];
                break;
            }
            if(marker==218||marker==217) {error=RI_CORRUPT;break;}
            end=at+(size_t)length;
            if(end>n)return true;
            at=end;
        }
    } else error=RI_UNSUPPORTED; /* Includes WebP behind .png/.jpg URLs. */
    if(!error&&(!w||!h))error=RI_CORRUPT;
    if(!error) {
        if(jpeg) {
            if(w>4096||h>4096)error=RI_LIMIT;
            /* JPEG fits output separately; intrinsic layout size is retained. */
        } else if(w>NS_IMAGE_SIDE||h>NS_IMAGE_SIDE||w*h>image_pixels||w*h>pixel_budget)error=RI_LIMIT;
    }
    if(error) {
        if(getenv("NETSURF_TRACE_IMAGES"))fprintf(stderr,"NETSURF_IMAGE_REJECT %s prefix=%lu status=%d\n",
            nsurl_access(content_get_url(c)),(unsigned long)n,error);
        content_broadcast_error(c,NSERROR_INVALID,ri_image_error(error));return false;
    }
    im->requested_pixels=w*h;
    im->probed=true;
    return true;
}
static bool decode(struct content *c,bool announce) {
    ImageContent *im=(ImageContent *)c;
    RiLimits limits={NS_IMAGE_SIDE,NS_IMAGE_SIDE,0,0,0},source={4096,4096,16777216UL,0,0};
    size_t length;const ns_octet_t *data;int result;
    /* Retain encoded source through NetSurf; evict only decoded RGBA. Rendered
     * page tiles remain valid, while an exposed image can be decoded lazily. */
    uint32_t need=image_pixels<pixel_budget?image_pixels:pixel_budget;
    if(im->requested_pixels&&im->requested_pixels<need)need=im->requested_pixels;
    make_room(im,need);
    limits.pixels=pixel_budget-used_pixels;
    if(limits.pixels>image_pixels)limits.pixels=image_pixels;
    limits.input=source.input=source_limit;limits.work=source.work=work_limit;
    data=content__get_source_data(c,&length);
    result=ri_image_decode_fit(data,length,&source,&limits,&im->image);
    if(getenv("NETSURF_TRACE_IMAGES"))
        fprintf(stderr,"NETSURF_IMAGE %s octets=%lu status=%d pixels_used=%lu\n",
            nsurl_access(content_get_url(c)),(unsigned long)length,result,(unsigned long)used_pixels);
    if(result){if(announce)content_broadcast_error(c,result==RI_MEMORY?NSERROR_NOMEM:NSERROR_INVALID,ri_image_error(result));return false;}
    im->pixels=(uint32_t)im->image->width*im->image->height;used_pixels+=im->pixels;
    im->used=++image_clock;
    c->width=im->image->source_width;c->height=im->image->source_height;
    /* NetSurf's legacy size field is a C16 unsigned, unlike source/cache
     * lengths. Saturate it; the separate exact pixel budget is authoritative. */
    c->size=im->pixels>16383?65535:(unsigned)(im->pixels*4);
    if(announce){content_set_ready(c);content_set_done(c);content_set_status(c,"");}return true;
}
static bool complete(struct content *c) { return decode(c,true); }
static void destroy(struct content *c) {
    ImageContent *im=(ImageContent *)c;discard(im);
    if(im->listed){if(im->prev)im->prev->next=im->next;else images=im->next;if(im->next)im->next->prev=im->prev;}
}
static bool redraw(struct content *c,struct content_redraw_data *data,
 const struct rect *clip,const struct redraw_context *ctx) {
    ImageContent *im=(ImageContent *)c;
    if(!im->image&&!decode(c,false))return false;
    im->used=++image_clock;
    return image_bitmap_plot((struct bitmap *)im->image,data,clip,ctx);
}
static void *internal(const struct content *c,void *context) { return ((const ImageContent *)c)->image; }
static content_type image_type(void) { return CONTENT_IMAGE; }
static bool opaque(struct content *c) { RiImage *im=((ImageContent *)c)->image;return im&&im->opaque; }
static nserror clone(const struct content *old,struct content **out) {
    ImageContent *im=calloc(1,sizeof(*im));nserror error;
    if(!im)return NSERROR_NOMEM;
    error=content__clone(old,&im->base);
    if(error!=NSERROR_OK){content_destroy(&im->base);return error;}
    track(im);
    im->requested_pixels=((const ImageContent *)old)->requested_pixels;
    if((old->status==CONTENT_STATUS_READY||old->status==CONTENT_STATUS_DONE)&&!complete(&im->base)) {
        content_destroy(&im->base);return NSERROR_CLONE_FAILED;
    }
    *out=&im->base;return NSERROR_OK;
}
static const struct content_handler handler={.create=create,.process_data=process,.data_complete=complete,.destroy=destroy,
 .redraw=redraw,.clone=clone,.get_internal=internal,.type=image_type,.is_opaque=opaque,.no_share=false};
nserror netsurf_images_register(void) {
    static const char *mimes[]={"image/png","image/x-png","image/gif","image/jpeg","image/jpg","image/pjpeg",
     "image/bmp","image/x-bmp","image/x-ms-bmp","image/x-win-bitmap","image/ms-bmp","application/bmp"};
    unsigned i;nserror error;
    image_pixels=ns_image_limit("NETSURF_IMAGE_PIXELS",NS_IMAGE_PIXELS_DEFAULT,4194304UL);
    pixel_budget=ns_image_limit("NETSURF_IMAGE_CACHE_PIXELS",NS_IMAGE_CACHE_DEFAULT,67108864UL);
    source_limit=ns_image_limit("NETSURF_IMAGE_INPUT",NS_IMAGE_INPUT_DEFAULT,33554432UL);
    work_limit=ns_image_limit("NETSURF_IMAGE_WORK",NS_IMAGE_WORK_DEFAULT,67108864UL);
    if(!image_pixels||!pixel_budget||!source_limit||!work_limit)return NSERROR_BAD_PARAMETER;
    for(i=0;i<sizeof(mimes)/sizeof(mimes[0]);i++) {
        error=content_factory_register_handler(mimes[i],&handler);if(error!=NSERROR_OK)return error;
    }
    return NSERROR_OK;
}
