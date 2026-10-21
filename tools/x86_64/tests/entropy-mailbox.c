#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BANK 64000U
#define CELLS (256U*BANK)
typedef uint64_t U;
typedef struct {uint16_t cells[CELLS],desc_bank,desc_offset,device_status;} Context;
static Context context;static unsigned calls,dirties,fail;
static void dirty(Context *c,size_t at,size_t count){(void)c;(void)at;(void)count;dirties++;}
#include "../boot/entropy.h"
int entropy_fill(uint8_t *out,unsigned n){calls++;if(fail)return 0;for(unsigned i=0;i<n;i++)out[i]=i;return 1;}
#include "../boot/entropy.inc"
static uint16_t *reset(void){
 memset(&context,0,sizeof(context));calls=dirties=fail=0;
 context.desc_bank=1;context.desc_offset=20;
 uint16_t *d=context.cells+BANK+20;d[0]=1;d[1]=6;d[2]=2;d[3]=63990;d[4]=256;return d;
}
int main(void){
 uint16_t *d=reset();entropy_device(&context);assert(!context.device_status&&calls==1&&dirties==1);
 for(unsigned i=0;i<256;i++)assert(context.cells[3*BANK-10+i]==i);
 for(unsigned n=0;n<10;n++){
  d=reset();switch(n){case 0:d[0]=2;break;case 1:d[1]=5;break;case 2:d[4]=0;break;case 3:d[4]=257;break;
  case 4:d[5]=1;break;case 5:context.desc_bank=256;break;case 6:context.desc_bank=255;context.desc_offset=63999;context.cells[CELLS-1]=1;break;
  case 7:d[2]=0;d[3]=29;break;case 8:d[2]=1;d[3]=22;break;case 9:d[2]=255;d[3]=63999;break;}
  entropy_device(&context);assert(context.device_status&&calls==0&&dirties==0);
 }
 d=reset();fail=1;context.cells[3*BANK]=0xaaaa;entropy_device(&context);
 assert(context.device_status==10&&calls==1&&dirties==0&&context.cells[3*BANK]==0xaaaa);
 puts("PASS: random mailbox validates ranges/reserved/overlap, crosses banks and leaves failures unchanged");
}
