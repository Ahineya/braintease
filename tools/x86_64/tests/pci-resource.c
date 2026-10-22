#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../boot/pci-resource.h"
static int overlap(uint64_t a,uint64_t n,PciResourceRange r){return a<r.end && r.start<a+n;}
int main(void){
    PciResourceRange windows[]={
        {0x80000000,0xc0000000},
        {UINT64_C(0x4000000000),UINT64_C(0x5000000000)}};
    uint64_t address=0;
    PciResourceRange used[]={
        {UINT64_C(0x4fffff8000),UINT64_C(0x5000000000)},
        {UINT64_C(0x4fffff6000),UINT64_C(0x4fffff7500)}};
    assert(!pci_resource_select(windows,2,used,2,4096,4096,UINT64_C(1)<<47,&address));
    assert(address==UINT64_C(0x4fffff5000));
    assert(!pci_resource_select(windows,2,used,2,4096,4096,UINT64_C(1)<<32,&address));
    assert(address==0xbffff000);
    assert(pci_resource_select(windows,2,used,2,4096,2048,UINT64_C(1)<<47,&address));
    PciResourceRange all[]={windows[0],windows[1]};
    assert(pci_resource_select(windows,2,all,2,4096,4096,UINT64_C(1)<<47,&address));
    PciResourceRange tiny={0x100001,0x101001};
    assert(pci_resource_select(&tiny,1,0,0,4096,4096,UINT64_C(1)<<47,&address));
    PciResourceRange malformed={2,1};
    assert(pci_resource_select(windows,2,&malformed,1,4096,4096,UINT64_C(1)<<47,&address));
    PciResourceRange zero={0,4096};
    assert(!pci_resource_select(&zero,1,0,0,4096,4096,UINT64_MAX,&address) && !address);
    PciResourceRange high={UINT64_MAX-8191,UINT64_MAX};
    assert(!pci_resource_select(&high,1,0,0,4096,4096,UINT64_MAX,&address));
    assert(address==UINT64_MAX-8191);
    assert(pci_resource_select(windows,2,used,PCI_RESOURCE_MAX_OCCUPIED+1,4096,4096,UINT64_MAX,&address));
    /* Compare the bounded jumping algorithm to exhaustive search over many
     * small overlapping/unaligned occupied layouts and varying limits. */
    uint32_t random=1;
    PciResourceRange w={16,256},blocked[8];
    for(unsigned trial=0;trial<2000;trial++){
        for(unsigned i=0;i<8;i++){
            random=random*1664525+1013904223;uint64_t lo=random%255;
            random=random*1664525+1013904223;uint64_t hi=lo+1+random%(256-lo);
            blocked[i]=(PciResourceRange){lo,hi};
        }
        uint64_t limit=32+trial%225,best=0;unsigned found=0;
        for(uint64_t at=16;at+16<=256 && at+16<=limit;at+=16){
            unsigned clear=1;for(unsigned i=0;i<8;i++)if(overlap(at,16,blocked[i]))clear=0;
            if(clear){found=1;best=at;}
        }
        const char *error=pci_resource_select(&w,1,blocked,8,16,16,limit,&address);
        assert((!error)==found);if(found)assert(address==best);
    }
    puts("PCI aligned root-window collision/limit/overflow checks passed");
}
