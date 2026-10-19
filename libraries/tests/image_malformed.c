/* SPDX-License-Identifier: MIT */
/* Host-only sanitizer/property checks, with failures injected at each allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "../image/image.h"
struct vector { const char *name;const unsigned char *data;size_t size;unsigned w,h;uint32_t hash;const unsigned char *expected;unsigned opaque; };
#include <fixtures.h>
static long fail_after=-1,live;
static void *checked_malloc(size_t n) {
 unsigned char *p;if(fail_after==0)return NULL;if(fail_after>0)fail_after--;
 p=malloc(n+64);if(!p)return NULL;*(size_t *)p=n;
 memset(p+sizeof(size_t),0xa5,32-sizeof(size_t));memset(p+32+n,0xa5,32);live++;return p+32;
}
static void *checked_calloc(size_t n,size_t s) {void *p=checked_malloc(n*s);if(p)memset(p,0,n*s);return p;}
static void checked_free(void *p) {
 unsigned char *base;size_t n,i;if(!p)return;base=(unsigned char *)p-32;n=*(size_t *)base;
 for(i=sizeof(size_t);i<32;i++)assert(base[i]==0xa5);
 for(i=0;i<32;i++)assert(base[32+n+i]==0xa5);
 live--;free(base);
}
#define malloc checked_malloc
#define calloc checked_calloc
#define free checked_free
#include "../image/decode.c"
#undef malloc
#undef calloc
#undef free
static uint32_t rng=123456789;
static unsigned random32(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void) {
    size_t v,n,i,at;unsigned j,passes=0;int status;RiImage *im;unsigned char *copy;
    for(v=0;v<sizeof(vectors)/sizeof(vectors[0]);v++) {
        const struct vector *item=vectors+v;

        for(i=0;i<16;i++) {
            fail_after=i;im=(void *)1;status=ri_image_decode(item->data,item->size,NULL,&im);
            if(status==RI_OK){ri_image_free(im);assert(live==0);break;}
            assert(status<0&&im==NULL&&live==0);passes++;
        }
        assert(i<16);fail_after=-1;
        copy=malloc(item->size);assert(copy);
        for(i=0;i<300;i++) {
            memcpy(copy,item->data,item->size);n=item->size;
            for(j=0;j<1+i%4;j++){at=random32()%n;copy[at]^=1U<<(random32()%8);}
            if(i%3==0)n=random32()%n;
            /* Recompute valid PNG chunk CRCs on some mutated containers, so
             * mutation reaches zlib, filtering and palette validation too. */
            if(i%2 && n>20 && copy[0]==137) {
                size_t pos=8;
                while(n-pos>=12) {
                    uint32_t length=ri_be32(copy+pos),c;
                    if(length>n-pos-12)break;
                    c=crc32(copy+pos+4,length+4);
                    copy[pos+8+length]=c>>24;copy[pos+9+length]=(c>>16)&255;
                    copy[pos+10+length]=(c>>8)&255;copy[pos+11+length]=c&255;
                    pos+=length+12;
                }
            }
            im=(void *)1;status=ri_image_decode(copy,n,NULL,&im);
            if(status==RI_OK){assert(im&&im->rgba&&im->width&&im->height);ri_image_free(im);}
            else assert(status<0&&im==NULL);
            assert(live==0);passes++;
        }
        free(copy);
    }
    /* Exhaust every 6-bit channel pair/alpha against an independent division
     * reference; red/blue are subranges of the same bounded arithmetic. */
    for(i=0;i<64;i++)for(at=0;at<64;at++)for(j=0;j<256;j++) {
        unsigned actual=ri_image_over565((uint16_t)(i<<5),j,(uint16_t)(at<<5));
        unsigned expected=(i*j+at*(255-j)+127)/255;
        assert(actual==(expected<<5));
    }
    printf("IMAGE_MALFORMED passes=%u allocations=%ld\n",passes,live);return 0;
}
