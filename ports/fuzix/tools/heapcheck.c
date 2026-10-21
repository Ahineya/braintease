/* Shared guest allocator correctness and measured scaling workload. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#define C(x) do { if(!(x)) {printf("HEAP_FAIL %d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
#ifdef FUSE_X_USER
#define LARGE_CHUNK 500000UL
#else
#define LARGE_CHUNK 50000UL
#endif
static char *slots[1024];
static unsigned sizes[1024];
static unsigned long elapsed(struct timespec *a,struct timespec *b) {
    return (b->tv_sec-a->tv_sec)*1000000UL+(b->tv_nsec-a->tv_nsec)/1000L;
}
static int batch(unsigned count) {
    unsigned i,round; struct timespec a,b;
    C(clock_gettime(CLOCK_MONOTONIC,&a)==0);
    for(round=0;round<2;round++) {
        for(i=0;i<count;i++) {
            unsigned size=16+(i%49); sizes[i]=size;
            slots[i]=malloc(size);C(slots[i]!=NULL);
            slots[i][0]=i;slots[i][size-1]=i^0xa55a;
        }
        for(i=0;i<count;i++) C((unsigned char)slots[i][0]==i && (unsigned char)slots[i][sizes[i]-1]==(i^0xa55a));
        for(i=0;i<count;i+=2) {free(slots[i]);slots[i]=NULL;}
        for(i=0;i<count;i+=2) {slots[i]=malloc(16);C(slots[i]!=NULL);slots[i][0]=0xbeef;}
        for(i=0;i<count;i++) C((unsigned char)slots[i][0]==(i%2?i:0xbeef));
        for(i=count;i;i--) free(slots[i-1]);
    }
    C(clock_gettime(CLOCK_MONOTONIC,&b)==0);
    printf("HEAP_BATCH %u %lu\n",count,elapsed(&a,&b));return 0;
}
int main(void) {
    unsigned i,n; char *p,*q,*large[40];
    C(batch(128)==0);C(batch(512)==0);C(batch(1024)==0);
    p=malloc(0);C(p!=NULL);free(p);free(NULL);
    C(calloc(~0UL,2)==NULL);
    p=calloc(5000,2);C(p!=NULL);for(i=0;i<10000;i++)C(p[i]==0);free(p);
    p=malloc(70003UL);C(p!=NULL);p[0]=0xabcd;p[64000UL]=0xfeed;p[70002UL]=0x1234;
    q=realloc(p,140007UL);C(q!=NULL);C((unsigned char)q[0]==0xabcd && (unsigned char)q[64000UL]==0xfeed && (unsigned char)q[70002UL]==0x1234);
    p=realloc(q,32);C(p!=NULL && (unsigned char)p[0]==0xabcd);
    q=realloc(p,~0UL);C(q==NULL && (unsigned char)p[0]==0xabcd);free(p);
    for(n=0;n<40;n++) {large[n]=malloc(LARGE_CHUNK);if(!large[n])break;large[n][LARGE_CHUNK-1]=n;}
#if defined(FUSE_X_USER) && !defined(FUSE_X_BOUNDED_HEAP)
    C(n==40); /* The hosted heap is no longer exhausted by 40 MiB. */
#else
    C(n>0 && n<40);
#endif
    for(i=0;i<n;i++)C((unsigned char)large[i][LARGE_CHUNK-1]==i);
    for(i=0;i<n;i++)free(large[i]);
    p=malloc(140000UL);C(p!=NULL);free(p);
    puts("HEAP_OK");return 0;
}
