/* Exploratory Tremor transform benchmark, not an Ogg player. */
#include "mdct.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static ogg_int32_t data[2048];
static unsigned long micros(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (unsigned long)t.tv_sec*1000000UL+t.tv_nsec/1000UL;
}
static void fill(unsigned round) {
    unsigned i;for(i=0;i<2048;i++)data[i]=(long)(((i*13U+round*17U)&255U))-128L;
}
int main(int argc,char **argv) {
    unsigned round,rounds=argc>1?(unsigned)atoi(argv[1]):32;
    unsigned long start,elapsed,hash=2166136261UL;
    if(!rounds||rounds>500)return 1;
    fill(0);mdct_backward(2048,data); /* Warm the transform before measuring. */
    start=micros();
    for(round=1;round<=rounds;round++) {
        fill(round);mdct_backward(2048,data);
        hash=((hash^(unsigned long)(uint32_t)data[(round*31U)&1023U])*16777619UL)&0xffffffffUL;
    }
    elapsed=micros()-start;
    printf("OGG_MDCT %u %lu %lu\n",rounds,elapsed,hash);
    return 0;
}
