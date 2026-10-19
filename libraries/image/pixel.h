/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_IMAGE_PIXEL_H
#define RIPPLE_IMAGE_PIXEL_H
#include <stdint.h>
/* RGB565 source-over, rounded in its native channel precision. All
 * numerators are <=16192, so these operations are safe on C16 unsigned int. */
static inline uint16_t ri_pixel_over565(uint16_t fg,unsigned a,uint16_t bg) {
    unsigned inv,r,g,b;
    if(!a)return bg;if(a>=255)return fg;inv=255-a;
    r=((fg>>11)&31)*a+((bg>>11)&31)*inv+127;
    g=((fg>>5)&63)*a+((bg>>5)&63)*inv+127;
    b=(fg&31)*a+(bg&31)*inv+127;
    /* Exact /255 over the bounded numerator range, without wide divides. */
    r=(r+1+(r>>8))>>8;g=(g+1+(g>>8))>>8;b=(b+1+(b>>8))>>8;
    return (uint16_t)((r<<11)|(g<<5)|b);
}
#endif
