/* CPU hardware random source. ISA capability + carry are checked before use.
 * Same RDSEED/RDRAND contract as the physically accepted WPA nonce source;
 * this exposes independent bounded requests, never the WPA state or keys. */
#include "entropy.h"
static void wipe(void *p, unsigned n) { volatile uint8_t *v=p;while(n--)*v++=0; }
int entropy_generate(uint8_t *out,unsigned n,EntropyWord word,void *opaque) {
    uint8_t staging[256];uint64_t value=0;
    if(!out||!n||n>sizeof(staging)||!word)return 0;
    for(unsigned i=0;i<n;) {
        if(!word(opaque,&value)){wipe(staging,sizeof(staging));wipe(&value,sizeof(value));return 0;}
        for(unsigned j=0;j<8&&i<n;j++,i++)staging[i]=value>>(j*8);
    }
    for(unsigned i=0;i<n;i++)out[i]=staging[i];
    wipe(staging,sizeof(staging));wipe(&value,sizeof(value));return 1;
}
#ifndef ENTROPY_HOST_TEST
static unsigned capabilities(void) {
    unsigned a,b,c,d,max;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));max=a;
    if(max<1)return 0;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    unsigned result=(c&(1U<<30))?1:0;
    if(max>=7){__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(7),"c"(0));if(b&(1U<<18))result|=2;}
    return result;
}
int entropy_available(void){return capabilities()!=0;}
static int hardware_word(void *opaque,uint64_t *out) {
    unsigned caps=*(unsigned *)opaque;unsigned char ready;
    for(unsigned n=0;n<64;n++) {
        if(caps&2)__asm__ volatile("rdseed %0; setc %1":"=r"(*out),"=qm"(ready)::"cc");
        else __asm__ volatile("rdrand %0; setc %1":"=r"(*out),"=qm"(ready)::"cc");
        if(ready)return 1;
        __asm__ volatile("pause");
    }
    return 0;
}
int entropy_fill(uint8_t *out,unsigned n) {
    unsigned caps=capabilities();if(!caps)return 0;
    return entropy_generate(out,n,hardware_word,&caps);
}
#endif
