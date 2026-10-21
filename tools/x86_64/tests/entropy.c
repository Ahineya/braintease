/* Deterministic failure-injection oracle, independent of CPU entropy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/entropy.h"
static unsigned calls,fail_at;
static int word(void *p,uint64_t *out){(void)p;if(calls++==fail_at)return 0;*out=0x0706050403020100ULL;return 1;}
int main(void){
 uint8_t out[258];
 for(unsigned n=1;n<=256;n++){
  memset(out,0xa5,sizeof(out));calls=0;fail_at=1000;
  assert(entropy_generate(out+1,n,word,0));assert(calls==(n+7)/8);
  assert(out[0]==0xa5&&out[n+1]==0xa5);
  for(unsigned i=0;i<n;i++)assert(out[1+i]==i%8);
 }
 for(unsigned f=0;f<32;f++){
  memset(out,0xa5,sizeof(out));calls=0;fail_at=f;assert(!entropy_generate(out+1,256,word,0));
  for(unsigned i=0;i<sizeof(out);i++)assert(out[i]==0xa5);
 }
 calls=0;assert(!entropy_generate(out,0,word,0));assert(!entropy_generate(out,257,word,0));assert(!entropy_generate(0,1,word,0));assert(!calls);
 puts("PASS: entropy sizes/byte order/guards and every partial-fill failure leaves output unchanged");
}
