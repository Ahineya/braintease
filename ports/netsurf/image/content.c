/* SPDX-License-Identifier: GPL-2.0-only */
/* NetSurf owns fetching/layout; the shared image library owns RGBA decoding. */
#include <netsurf_profile.h>
#include "content.h"
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
    bool probed;
} ImageContent;
/* Across the complete cache, not per URL. A web page cannot allocate one
 * full-size bitmap per image without bound. Four RGBA cells per pixel. */
#define PIXEL_BUDGET 65536UL
static uint32_t used_pixels;
static void (*dispose_image)(const RiImage *);
void netsurf_images_dispose(void (*callback)(const RiImage *)) { dispose_image=callback; }
static nserror create(const struct content_handler *handler,lwc_string *mime,
 const struct http_parameter *params,struct llcache_handle *cache,
 const char *charset,bool quirks,struct content **out) {
    ImageContent *im=calloc(1,sizeof(*im));nserror error;
    if(!im)return NSERROR_NOMEM;
    error=content__init(&im->base,handler,mime,params,cache,charset,quirks);
    if(error!=NSERROR_OK){free(im);return error;}*out=&im->base;return NSERROR_OK;
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
    uint32_t w=0,h=0;unsigned marker,length;
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
                if(marker!=192) {error=RI_UNSUPPORTED;break;}
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
    if(!error&&(w>512||h>512||w*h>PIXEL_BUDGET-used_pixels))error=RI_LIMIT;
    if(error) {
        if(getenv("NETSURF_TRACE_IMAGES"))fprintf(stderr,"NETSURF_IMAGE_REJECT %s prefix=%lu status=%d\n",
            nsurl_access(content_get_url(c)),(unsigned long)n,error);
        content_broadcast_error(c,NSERROR_INVALID,ri_image_error(error));return false;
    }
    im->probed=true;
    return true;
}
static bool complete(struct content *c) {
    ImageContent *im=(ImageContent *)c;
    RiLimits limits={512,512,0,262144UL,393216UL};size_t length;const ns_octet_t *data;int result;
    limits.pixels=PIXEL_BUDGET-used_pixels;
    data=content__get_source_data(c,&length);
    result=ri_image_decode(data,length,&limits,&im->image);
    if(getenv("NETSURF_TRACE_IMAGES"))
        fprintf(stderr,"NETSURF_IMAGE %s octets=%lu status=%d pixels_used=%lu\n",
            nsurl_access(content_get_url(c)),(unsigned long)length,result,(unsigned long)used_pixels);
    if(result){content_broadcast_error(c,result==RI_MEMORY?NSERROR_NOMEM:NSERROR_INVALID,ri_image_error(result));return false;}
    im->pixels=(uint32_t)im->image->width*im->image->height;used_pixels+=im->pixels;
    c->width=im->image->width;c->height=im->image->height;
    /* NetSurf's legacy size field is a C16 unsigned, unlike source/cache
     * lengths. Saturate it; the separate exact pixel budget is authoritative. */
    c->size=im->pixels>16383?65535:(unsigned)(im->pixels*4);
    content_set_ready(c);content_set_done(c);content_set_status(c,"");return true;
}
static void destroy(struct content *c) {
    ImageContent *im=(ImageContent *)c;used_pixels-=im->pixels;if(dispose_image&&im->image)dispose_image(im->image);ri_image_free(im->image);
}
static bool redraw(struct content *c,struct content_redraw_data *data,
 const struct rect *clip,const struct redraw_context *ctx) {
    ImageContent *im=(ImageContent *)c;
    return im->image&&image_bitmap_plot((struct bitmap *)im->image,data,clip,ctx);
}
static void *internal(const struct content *c,void *context) { return ((const ImageContent *)c)->image; }
static content_type image_type(void) { return CONTENT_IMAGE; }
static bool opaque(struct content *c) { RiImage *im=((ImageContent *)c)->image;return im&&im->opaque; }
static nserror clone(const struct content *old,struct content **out) {
    ImageContent *im=calloc(1,sizeof(*im));nserror error;
    if(!im)return NSERROR_NOMEM;
    error=content__clone(old,&im->base);
    if(error!=NSERROR_OK){content_destroy(&im->base);return error;}
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
    for(i=0;i<sizeof(mimes)/sizeof(mimes[0]);i++) {
        error=content_factory_register_handler(mimes[i],&handler);if(error!=NSERROR_OK)return error;
    }
    return NSERROR_OK;
}
