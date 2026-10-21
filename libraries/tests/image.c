/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../image/image.h"
struct vector { const char *name;const unsigned char *data;size_t size;unsigned w,h;uint32_t hash;const unsigned char *expected;unsigned opaque; };
#include <fixtures.h>
static unsigned checks,failures;
static void check(int value,const char *name) { checks++;if(!value){printf("FAIL %s\n",name);failures++;} }
static uint32_t hash(const RiByte *p,size_t n) { uint32_t h=2166136261UL;while(n--){h^=*p++;h*=16777619UL;}return h; }
int main(void) {
    unsigned v,i,max,error;uint32_t sum;size_t n,cut;RiImage *im=NULL,*again=NULL;int status;
    RiLimits small={16,16,256,262144UL,786432UL};
    for(v=0;v<sizeof(vectors)/sizeof(vectors[0]);v++) {
        const struct vector *item=vectors+v;
        status=ri_image_decode(item->data,item->size,NULL,&im);
        if(status){printf("DECODE %s status=%d\n",item->name,status);check(0,item->name);continue;}
        check(im->width==item->w&&im->height==item->h,item->name);
        n=(size_t)im->width*im->height*4;
        if(item->expected) {
            max=0;sum=0;
            for(cut=0;cut<n;cut++){error=abs((int)im->rgba[cut]-(int)item->expected[cut]);if(error>max)max=error;sum+=error;}
            check(max<=4&&sum<=n,item->name);
        } else check(hash(im->rgba,n)==item->hash,item->name);
        check(im->opaque==item->opaque,"opacity");
        ri_image_free(im);im=NULL;
        for(i=0;i<5;i++) {
            cut=i==0?0:i==1?1:i==2?7:i==3?item->size/2:item->size-1;
            again=(RiImage *)1;
            status=ri_image_decode(item->data,cut,NULL,&again);
            check(status<0&&again==NULL,"truncated image must fail transactionally");
            if(again&&(again!=(RiImage *)1))ri_image_free(again);
        }
    }
    /* Independent source/output budgets: JPEG layout keeps original geometry,
     * decoded buffers never exceed the requested target. Other formats remain
     * strict until they have a row/tile decoder as well. */
    for(v=0;v<sizeof(vectors)/sizeof(vectors[0]);v++)if(vectors[v].expected) {
        const struct vector *item=vectors+v;
        RiLimits output={8,8,64,262144UL,786432UL};
        status=ri_image_decode_fit(item->data,item->size,NULL,&output,&im);
        /* NULL source defaults to output, deliberately still a strict input. */
        check(status==RI_LIMIT&&!im,"default source bound");
        { RiLimits source={512,512,65536UL,262144UL,786432UL};
          status=ri_image_decode_fit(item->data,item->size,&source,&output,&im);
          check(status==RI_OK&&im&&im->width<=8&&im->height<=8,"bounded scaled JPEG");
          if(!status) {
              check(im->source_width==item->w&&im->source_height==item->h,"intrinsic JPEG dimensions");
              ri_image_free(im);im=NULL;
          }
          output.work=1;
          check(ri_image_decode_fit(item->data,item->size,&source,&output,&im)==RI_LIMIT&&!im,"decoder work bound");
        }
    }
    { RiLimits source={4096,4096,16777216UL,262144UL,786432UL};
      RiLimits output={128,128,16384UL,262144UL,786432UL};
      check(ri_image_decode(image_large_baseline,sizeof(image_large_baseline),NULL,&im)==RI_LIMIT&&!im,"strict large JPEG refused");
      status=ri_image_decode_fit(image_large_baseline,sizeof(image_large_baseline),&source,&output,&im);
      check(status==RI_OK&&im&&im->width==65&&im->height==49&&im->source_width==1025&&im->source_height==769,"direct reduced MCU decode");
      if(!status) {
          max=0;
          for(cut=0;cut<65UL*49*4;cut++) {error=abs((int)im->rgba[cut]-image_large_expected[cut]);if(error>max)max=error;}
          if(max>8)printf("REDUCED max=%u first=%u,%u,%u expected=%u,%u,%u\n",max,im->rgba[0],im->rgba[1],im->rgba[2],image_large_expected[0],image_large_expected[1],image_large_expected[2]);
          check(max<=8,"reduced MCU reference pixels");ri_image_free(im);im=NULL;
      }
    }
    check(ri_image_decode(vectors[0].data,vectors[0].size,&small,&im)==RI_LIMIT&&!im,"custom dimension bound");
    check(ri_image_decode(NULL,10,NULL,&im)==RI_CORRUPT&&!im,"null input");
    check(ri_image_decode(vectors[0].data,vectors[0].size,NULL,NULL)==RI_CORRUPT,"null output");
    check(ri_image_rgb565((const RiByte[]){255,0,0,255})==0xf800U,"RGB565 red");
    check(ri_image_over565(0xf800U,128,0x001fU)==0x800fU,"partial alpha source over");
    check(ri_image_over565(0xf800U,0,0x001fU)==0x001fU,"transparent pixel");
    check(ri_image_over565(0xf800U,255,0x001fU)==0xf800U,"opaque pixel");
#if CHAR_BIT>8
    { RiByte invalid[8]={137,80,78,71,13,10,26,300};check(ri_image_decode(invalid,8,NULL,&im)==RI_CORRUPT&&!im,"reject non-octet C16 input"); }
#endif
    printf("IMAGE_CHECK checks=%u failures=%u vectors=%u\n",checks,failures,(unsigned)(sizeof(vectors)/sizeof(vectors[0])));
    return failures?1:0;
}
