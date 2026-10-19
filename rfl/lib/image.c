/* SPDX-License-Identifier: MIT */
/* Opaque ABI bridge; decoding and pixel storage live in the shared library. */
#include "../../libraries/image/decode.c"
RiImage *rfl_image_decode(const RiByte *bytes,unsigned long length,int *status) {
    RiImage *image=NULL;*status=ri_image_decode(bytes,length,NULL,&image);return image;
}
unsigned rfl_image_width(const RiImage *image) {return image->width;}
unsigned rfl_image_height(const RiImage *image) {return image->height;}
unsigned rfl_image_opaque(const RiImage *image) {return image->opaque;}
uint32_t rfl_image_rgba(const RiImage *image,unsigned x,unsigned y) {
    const RiByte *p;
    if(x>=image->width||y>=image->height)return 0;
    p=image->rgba+((size_t)y*image->width+x)*4;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
