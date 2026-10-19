/* Portable guest benchmark: identical input streams before/after Unicode. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef VT_BASELINE
#include VT_BASELINE
#else
#include "../terminal.c"
#endif
static struct vt terminal;
static unsigned long micros(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)t.tv_sec*1000000UL+t.tv_nsec/1000;}
int main(void){
    static const char ascii[]="abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\n";
    static const char mixed[]={0xd0,0x9f,0xd1,0x80,0xd0,0xb8,0xd0,0xb2,0xd0,0xb5,0xd1,0x82,' ',0xce,0x95,0xce,0xbb,0xce,0xbb,0xce,0xb7,0xce,0xbd,0xce,0xb9,0xce,0xba,0xce,0xac,' ','e',0xcc,0x81,' ',0xe4,0xb8,0xad,' ',0xf0,0x9f,0x99,0x82,'\r','\n',0};
    unsigned i;unsigned long start,ascii_time,mixed_time;
    if(vt_init_size(&terminal,80,30))return 1;
#ifndef VT_BASELINE
    {static const char check[]={27,'[','3','1','m','A',0xd0,0x96,'e',0xcc,0x81,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82,'Z'};
    vt_feed(&terminal,check,sizeof(check));}
    if(vt_cells(&terminal)[1].ch!=0x416||vt_cells(&terminal)[1].colour!=1||
        vt_cells(&terminal)[4].ch!=RU_CONTINUATION||vt_cells(&terminal)[5].ch!=0x1f642UL){printf("UNICODE_VT_FAILED %lu %lu %lu %lu\n",(unsigned long)vt_cells(&terminal)[1].ch,(unsigned long)vt_cells(&terminal)[1].colour,(unsigned long)vt_cells(&terminal)[4].ch,(unsigned long)vt_cells(&terminal)[5].ch);return 1;}
#endif
    for(i=0;i<100;i++)vt_feed(&terminal,ascii,sizeof(ascii)-1);
    start=micros();for(i=0;i<2000;i++)vt_feed(&terminal,ascii,sizeof(ascii)-1);ascii_time=micros()-start;
    start=micros();for(i=0;i<2000;i++)vt_feed(&terminal,mixed,sizeof(mixed)-1);mixed_time=micros()-start;
    printf("UNICODE_BENCH %lu %lu %u\n",ascii_time,mixed_time,(unsigned)sizeof(struct vt_cell));
    vt_destroy(&terminal);return 0;
}
