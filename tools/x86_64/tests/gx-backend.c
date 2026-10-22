#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/gx.c"
static uint16_t cells[40000];
static unsigned presents,restores,allocations,frees,denied;
static uint32_t frame[1920*1080];
static unsigned fw,fh;
static void *getmem(size_t bytes){++allocations;return malloc(bytes);}
static void putmem(void *p,size_t bytes){(void)bytes;++frees;free(p);}
static void present(const uint32_t *p,unsigned w,unsigned h){++presents;fw=w;fh=h;memcpy(frame,p,(size_t)w*h*4);}
static void restore(void){++restores;}
static uint16_t *getspan(void *opaque,uint32_t p,size_t n,int write){(void)opaque;(void)write;if(p<32 || p>40000 || n>40000-p || p==denied)return 0;return cells+p;}
static GxGuest guest={0,getspan};
static int call(uint64_t owner,unsigned op,uint32_t a[12]){return gx_call(owner,op,a,&guest);}
static uint32_t create(uint64_t owner,unsigned format){uint32_t a[12]={8,4,32,0,format};assert(!call(owner,1,a));return a[0];}
int main(void){
    GxPlatform p={1280,720,getmem,putmem,present,restore};gx_bind(&p);
    uint32_t handle=create(1,2),a[12]={0};a[0]=handle;
    assert(!gx_active() && !presents);
    a[1]=1;a[2]=1;a[3]=2;a[4]=2;a[5]=0x123456;
    assert(call(2,27,a)==EPERM_);assert(!call(1,27,a));assert(!presents);
    assert(!call(1,4,a));assert(gx_active() && presents==1 && fw==8 && fh==4 && frame[9]==0x123456);
    /* Entire source and rectangle are checked before upload mutates RAM. */
    uint32_t before=surfaces[0].pixels[9];
    a[1]=1;a[2]=1;a[3]=2;a[4]=2;a[5]=6;a[6]=39996;
    assert(call(1,23,a)==EFAULT_ && surfaces[0].pixels[9]==before && presents==1);
    a[6]=100;a[1]=7;assert(call(1,23,a)==EINVAL_ && surfaces[0].pixels[9]==before);
    a[1]=1;cells[100]=0x12aa;cells[101]=0x34bb;cells[102]=0x56cc;
    cells[103]=0x1122;cells[104]=0x3344;cells[105]=0x5566;
    assert(!call(1,23,a));assert(surfaces[0].pixels[9]==0xccbbaa && surfaces[0].pixels[10]==0x664422);
    assert(presents==1);assert(!call(1,4,a) && frame[9]==0xccbbaa);
    /* Copy overlaps in either direction preserve ordered memmove semantics. */
    for(unsigned i=0;i<8;i++)surfaces[0].pixels[i]=i;
    a[1]=0;a[2]=0;a[3]=7;a[4]=1;a[5]=1;a[6]=0;assert(!call(1,6,a));
    for(unsigned i=1;i<8;i++)assert(surfaces[0].pixels[i]==i-1);
    a[1]=1;a[5]=0;assert(!call(1,6,a));for(unsigned i=0;i<7;i++)assert(surfaces[0].pixels[i]==i);
    /* Invalid event output leaves the queued input available for a good call. */
    gx_key(79,1,0);gx_key(79,0,0);assert(call(2,10,a)==1);a[1]=39999;assert(call(1,8,a)==EFAULT_);
    a[1]=200;assert(!call(1,8,a) && cells[205]==1 && cells[206]==1 && cells[207]==79);
    assert(!call(1,8,a) && cells[205]==1 && cells[206]==0 && cells[207]==79);
    assert(!call(1,8,a) && !cells[205]);
    gx_pointer(-3,5,1,65536);assert(!call(1,8,a) && cells[205]==2);
    assert(!call(1,8,a) && cells[205]==3 && cells[206]==1 && cells[207]==1);
    assert(!call(1,8,a) && cells[205]==4 && cells[220]==1);
    gx_pointer(10,20,0,0);assert(!call(1,8,a) && cells[205]==2);
    assert(!call(1,8,a) && cells[205]==3 && cells[206]==0);
    assert(!call(1,9,a) && !cells[204] && cells[221]==10 && cells[223]==20);
    gx_key(226,1,0);gx_key(61,1,0);
    assert(!call(1,8,a) && cells[205]==1 && cells[207]==226);
    assert(!call(1,8,a) && cells[205]==8);
    gx_key(61,0,0);gx_key(226,0,0);
    assert(!call(1,8,a) && cells[205]==1);assert(!call(1,8,a) && cells[205]==1);
    for(unsigned i=0;i<100;i++)gx_key(4,1,0);
    assert(!call(1,8,a) && cells[205]==9);assert(!call(1,9,a) && (cells[205]&16));
    gx_key(4,0,0);assert(!call(1,9,a) && !(cells[205]&16));
    /* Unsupported optional accelerators reliably select existing fallback. */
    assert(call(1,30,a)==EOPNOTSUPP_ && call(1,29,a)==EOPNOTSUPP_);
    gx_release(2);assert(gx_active());gx_release(1);assert(!gx_active() && restores==1 && allocations==frees);
    assert(call(1,4,a)==EINVAL_);
    uint32_t newer=create(1,0);assert(newer!=handle && surfaces[0].format==1);a[0]=newer;
    a[1]=a[2]=0;a[3]=a[4]=1;a[5]=0xf800;assert(!call(1,5,a));assert(!call(1,4,a));assert(frame[0]==0xff0000);
    a[3]=a[4]=2;a[5]=2;a[6]=100;a[7]=500;cells[100]=0;cells[101]=1;cells[102]=2;cells[103]=256;
    uint32_t snapshot=surfaces[0].pixels[0];denied=500;assert(call(1,18,a)==EFAULT_ && surfaces[0].pixels[0]==snapshot);denied=0;
    cells[103]=3;cells[500]=0xf800;cells[501]=0x07e0;cells[502]=0x001f;cells[503]=0xffff;
    assert(!call(1,18,a));assert(!call(1,4,a));assert(frame[0]==0 && frame[2]==0xff0000 && frame[4]==0x00ff00);
    /* Caller RAM copy rejects overlap; fill preserves row padding. */
    uint32_t r[12]={1000,1001,2,2,3,3,0x55aa};assert(call(1,16,r)==EINVAL_);
    r[1]=2000;cells[1002]=0x7777;assert(!call(1,15,r));assert(cells[1000]==0x55aa && cells[1002]==0x7777);
    /* Match existing hosted BGR blend: mask low octet, opacity thresholds,
     * full validation before transparent early return, padding untouched. */
    uint32_t b[12]={3000,4000,2,2,8,3,30,60,90,255};
    for(unsigned i=0;i<16;i++)cells[3000+i]=173;
    cells[4000]=0xff00;cells[4001]=0xaaff;cells[4003]=0x55fe;cells[4004]=127;
    assert(!call(1,25,b));assert(cells[3000]==173 && cells[3003]==30 && cells[3006]==173);
    assert(cells[3008]==30 && cells[3011]==((30*127+173*128)>>8));
    b[9]=0;b[1]=3000;assert(call(1,25,b)==EINVAL_);b[1]=4000;assert(!call(1,25,b));
    gx_release(1);assert(allocations==frees);
    puts("GX checks passed: ownership, stale handles, complete upload validation, RGB/BGR and scaled indexed sources, explicit present, overlapping surface copy, event make/break/overflow, fallback, cleanup");
}
