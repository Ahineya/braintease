#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef FAST
#define FAST 0
#endif
#define BANK 64000ULL
#define LIMIT (65536ULL*BANK)
typedef uint64_t (*Read)(void*,uint64_t,uint64_t);
#define DECL(n) extern uint64_t rcc_user_##n(void*,uint64_t,uint64_t)
DECL(pair1); DECL(pair2); DECL(pair4); DECL(single); DECL(volatile);
DECL(storebarrier); DECL(geppair); DECL(many);
static uint64_t polls, loads, acquires, generation, geps, record[5];
static uint16_t pages[2][512];
static int deny, bad;
static uint64_t logical(uint64_t dense) { return ((dense/BANK)<<16)|(dense%BANK); }
static uint64_t dense(uint64_t p) {
    if ((p>>16)>=65536 || (p&65535)>=BANK) { fprintf(stderr,"checked-load invalid pointer\n");exit(86); }
    return (p>>16)*BANK+(p&65535);
}
static uint16_t cell(uint64_t d) { return (uint16_t)((d*193+17)^generation); }
static uint64_t reference(uint64_t p, unsigned w) {
    uint64_t d=dense(p), v=0;
    if (w>LIMIT-d) { fprintf(stderr,"checked-load span\n");exit(86); }
    for(unsigned i=0;i<w;i++) v|=(uint64_t)cell(d+i)<<(16*i);
    return v;
}
uint64_t rcc_native_poll(void *ctx) { (void)ctx;polls++;return 0; }
uint64_t rcc_native_load(void *ctx,uint64_t p,uint64_t w) { rcc_native_poll(ctx);loads++;return reference(p,(unsigned)w); }
uint64_t rcc_native_store(void *ctx,uint64_t p,uint64_t v,uint64_t w) {
    (void)p;(void)v;(void)w;rcc_native_poll(ctx);generation++;
    /* Poison old backing; a new grant must name a different native page. */
    memset(pages[(generation-1)%2],0xee,sizeof(pages[0]));return 0;
}
uint64_t rcc_native_gep(void *ctx,uint64_t p,uint64_t delta) { (void)ctx;geps++;return logical(dense(p)+delta); }
uint64_t rcc_native_register(void *ctx,uint64_t t,uint64_t p) { (void)ctx;(void)t;(void)p;return 0; }
#if FAST
uint64_t rcc_native_memview_v1(void *ctx,uint64_t p,uint64_t n,uint64_t flags) {
    (void)ctx;acquires++;
    uint64_t bank=p>>16, offset=p&65535;
    if (deny || flags!=1 || !n || !bank || bank>=65536 || offset>=BANK || n>BANK-offset
        || (bank>=240 && bank<256) || bank>=32768) return 0;
    uint64_t d=bank*BANK+offset, start=d/512*512;
    if(n>512-(d-start)) return 0;
    for(unsigned i=0;i<512;i++) pages[generation%2][i]=cell(start+i);
    record[0]=1;record[1]=1;record[2]=start;record[3]=512;record[4]=(uint64_t)pages[generation%2];
    if(bad==1)record[0]=2;
    if(bad==2)record[1]=3;
    if(bad==3)record[3]=0;
    if(bad==4)record[2]=UINT64_MAX;
    if(bad==5)record[3]=UINT64_MAX;
    if(bad==6)record[4]=0;
    if(bad==7)record[4]=UINT64_MAX-3;
    if(bad==8)record[2]=d+1;
    if(bad==9)record[3]=n-1;
    if(bad==10)record[4]|=1;
    return (uint64_t)record;
}
#endif
static void check(Read f,uint64_t p,uint64_t q,unsigned w,int expected_loads,int expected_acquires) {
    uint64_t expected=reference(p,w)+reference(q,w), bp=polls,bl=loads,ba=acquires;
    uint64_t actual=f(NULL,p,q);
    if(actual!=expected || polls-bp!=2 || loads-bl!=(uint64_t)(FAST?expected_loads:2) || acquires-ba!=(uint64_t)(FAST?expected_acquires:0)) {
        fprintf(stderr,"read mismatch p=%llx q=%llx w=%u result=%llx expected=%llx loads=%llu polls=%llu acquisitions=%llu\n",
          (unsigned long long)p,(unsigned long long)q,w,(unsigned long long)actual,(unsigned long long)expected,
          (unsigned long long)(loads-bl),(unsigned long long)(polls-bp),(unsigned long long)(acquires-ba));exit(1);
    }
}
uint64_t rcc_native_start(uint64_t entry) {
    (void)entry;(void)record;
    const char *fault=getenv("VIEW_FAULT");
    if(fault) {
        uint64_t p=logical(BANK+8);
        if(!strcmp(fault,"offset"))p=(1<<16)|64000;
        if(!strcmp(fault,"bank"))p=1ULL<<32;
        if(!strcmp(fault,"carrier"))p|=1ULL<<48;
        if(!strcmp(fault,"last-cell"))p=logical(LIMIT-1);
        rcc_user_pair4(NULL,p,p);return 1;
    }
    Read functions[]={rcc_user_pair1,rcc_user_pair2,rcc_user_pair4};
    unsigned widths[]={1,2,4};
    uint64_t banks[]={1,2,239,240,255,256,32767,32768,65535};
    unsigned offsets[]={0,1,31,508,509,510,511,512,63996};
    for(unsigned f=0;f<3;f++) for(unsigned b=0;b<9;b++) for(unsigned i=0;i<9;i++) {
        uint64_t p=logical(banks[b]*BANK+offsets[i]);
        int private=banks[b]<240||(banks[b]>=256&&banks[b]<32768);
        int grant=private&&offsets[i]%512+widths[f]<=512;
        check(functions[f],p,p,widths[f],grant?0:2,1);
    }
    uint64_t p=logical(BANK+8),q=logical(BANK+10);
    for(bad=0;bad<=10;bad++)check(rcc_user_pair2,p,q,2,bad?2:0,1);
    bad=0;deny=1;check(rcc_user_pair4,p,q,4,2,1);deny=0;
    check(rcc_user_pair1,p,logical(BANK+512),1,1,1);
    check(rcc_user_pair1,logical(32),logical(33),1,2,1);
    check(rcc_user_volatile,p,q,1,2,0);
    uint64_t bp=polls,ba=acquires,bl=loads;
    if(rcc_user_single(NULL,p,q)!=reference(p,1)||polls-bp!=1||loads-bl!=1||acquires!=ba)return 2;
    bp=polls;ba=acquires;bl=loads;
    uint64_t expected=reference(p,1)+reference(q,1);generation++;expected+=reference(p,1)+reference(q,1);generation--;
    if(rcc_user_storebarrier(NULL,p,q)!=expected||polls-bp!=5||loads-bl!=(FAST?0:4)||acquires-ba!=(FAST?2:0))return 3;
    bp=polls;ba=acquires;bl=loads;
    if(rcc_user_geppair(NULL,p,q)!=reference(p,1)+reference(logical(dense(p)+1),1)||polls-bp!=2||loads-bl!=(FAST?0:2)||acquires-ba!=(FAST?1:0))return 4;
    // Hosted read-v1 also keeps valid high-bank same-bank arithmetic inline.
    p=logical(256*BANK+8);bp=polls;ba=acquires;bl=loads;
    if(rcc_user_geppair(NULL,p,q)!=reference(p,1)+reference(logical(dense(p)+1),1)||polls-bp!=2||loads-bl!=(FAST?0:2)||acquires-ba!=(FAST?1:0))return 5;
    if(getenv("VIEW_BENCH")) {
        p=logical(BANK+32);uint64_t sum=0;clock_t start=clock();
        for(unsigned i=0;i<1000000;i++)sum+=rcc_user_many(NULL,p,0);
        printf("bench %.6f checksum %llu\n",(double)(clock()-start)/CLOCKS_PER_SEC,(unsigned long long)sum);
    }
    puts("read-view differential passed");return 0;
}
