#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/bootinfo.h"
#define RIPLIX_X86_KERNEL_H
BootInfo boot;
typedef struct {uint32_t cfg[64],mask[6];unsigned probing,probes,writes,command_writes,refuse_decode,bad_restore,bad_command_restore;} Fake;
static Fake f;
uint32_t pci_read(unsigned bdf,unsigned offset){
    assert(bdf==0x10);
    if(offset>=0x10 && offset<0x28){unsigned bar=(offset-0x10)/4;
        return (f.probing&(1U<<bar))?f.mask[bar]:f.cfg[offset/4];}
    return f.cfg[offset/4];
}
void pci_write(unsigned bdf,unsigned offset,uint32_t value){
    assert(bdf==0x10 && offset>=0x10 && offset<0x28 && !(f.cfg[1]&3));
    assert(f.cfg[1]&4); /* Bus master is preserved, including during sizing. */
    unsigned bar=(offset-0x10)/4;f.writes++;
    if(value==UINT32_MAX){assert(!f.probing);f.probing|=1U<<bar;f.probes++;return;}
    if(f.bad_restore==bar+1)return;
    f.probing&=~(1U<<bar);f.cfg[offset/4]=value;
}
void pci_write16(unsigned bdf,unsigned offset,uint16_t value){
    assert(bdf==0x10 && offset==4);f.command_writes++;
    if((!(value&3) && f.refuse_decode) || ((value&3) && f.bad_command_restore))return;
    f.cfg[1]=(f.cfg[1]&0xffff0000U)|value;
}
#include "../boot/pci-refine.c"
static PciBootSpan span(unsigned bar,uint64_t base,uint64_t length,unsigned kind){
    return (PciBootSpan){.base=base,.length=length,.bdf=0x10,.bar=(uint8_t)bar,.kind=(uint8_t)kind};
}
static void setup(void){
    memset(&boot,0,sizeof(boot));memset(&f,0,sizeof(f));attempted=0;outcome=0;
    boot.pci.version=1;boot.pci.status=PCI_BOOT_COMPLETE;
    f.cfg[0]=0x12348086;f.cfg[1]=0xa5a50007;f.cfg[2]=0x03000000;
    f.cfg[4]=0x80000004;f.cfg[5]=0x40;
    /* 64-bit BAR with upper address bits unimplemented: 16MiB extent. */
    f.mask[0]=0xff000004;f.mask[1]=0x7f;
    f.cfg[6]=0x90000000;f.mask[2]=0xfffff000;
    boot.pci.span_count=3;
    boot.pci.occupied[0]=span(0,UINT64_C(0x4080000000),UINT64_C(0x4000000000),PCI_BOOT_UNKNOWN);
    boot.pci.occupied[1]=span(1,UINT64_C(0x4080000000),0x1000000,PCI_BOOT_BAR);
    boot.pci.occupied[2]=span(0xfe,0x80000000,0x100000,PCI_BOOT_BRIDGE);
}
static void restored(void){
    assert(f.cfg[1]==0xa5a50007 && f.cfg[4]==0x80000004 && f.cfg[5]==0x40 && f.cfg[6]==0x90000000 && !f.probing);
}
static unsigned matching(unsigned bar,uint64_t base,uint64_t bytes){
    unsigned count=0;for(unsigned i=0;i<boot.pci.span_count;i++){
        PciBootSpan *s=&boot.pci.occupied[i];
        if(s->bdf==0x10 && s->bar==bar && s->base==base && s->length==bytes && s->kind==PCI_BOOT_BAR)count++;
    }return count;
}
int main(void){
    setup();assert(!pci_resources_refine());restored();
    assert(boot.pci.span_count==3 && matching(0,UINT64_C(0x4080000000),0x1000000)==1 && matching(2,0x90000000,0x1000)==1);
    assert(boot.pci.occupied[0].kind==PCI_BOOT_BRIDGE && f.probes==3 && f.command_writes==2);
    unsigned writes=f.writes;assert(!pci_resources_refine() && f.writes==writes); /* Once only. */
    setup();f.cfg[2]=0x06000000;assert(pci_resources_refine() && !f.writes && !f.command_writes && boot.pci.span_count==3);
    setup();f.refuse_decode=1;assert(pci_resources_refine() && !f.writes && boot.pci.status==PCI_BOOT_INCOMPLETE);restored();
    setup();f.mask[0]=0xff0ff004;assert(pci_resources_refine() && boot.pci.span_count==3);restored();
    setup();f.mask[0]=f.cfg[4];f.mask[1]=f.cfg[5];
    assert(pci_resources_refine() && boot.pci.span_count==3);restored(); /* Read-only/D3 BAR is not a size. */
    setup();f.mask[0]=UINT32_MAX;assert(pci_resources_refine() && boot.pci.span_count==3);restored();
    setup();f.cfg[4]+=0x1000;assert(pci_resources_refine() && boot.pci.span_count==3);assert(!f.probing && f.cfg[1]==0xa5a50007);
    setup();f.bad_restore=1;assert(pci_resources_refine() && !(f.cfg[1]&3) && (f.cfg[1]&4));
    assert(boot.pci.status==PCI_BOOT_INCOMPLETE && strstr(boot.pci.reason,"rollback unverified"));
    setup();f.bad_command_restore=1;assert(pci_resources_refine() && boot.pci.span_count==3 && strstr(boot.pci.reason,"command restore"));
    setup();boot.pci.occupied[boot.pci.span_count++]=span(4,0xabc00000,4096,PCI_BOOT_BAR);
    boot.pci.occupied[boot.pci.span_count++]=span(6,0xdef00000,4096,PCI_BOOT_ROM32);
    assert(!pci_resources_refine() && boot.pci.span_count==5 && matching(4,0xabc00000,4096)==1);restored();
    /* Wrong firmware index 3 points at assigned live BAR4, not a D3 cache. */
    setup();f.cfg[8]=f.cfg[6];f.cfg[6]=0;f.mask[4]=f.mask[2];
    boot.pci.occupied[boot.pci.span_count++]=span(3,0x90000000,4096,PCI_BOOT_BAR);
    assert(!pci_resources_refine() && boot.pci.span_count==3 && matching(4,0x90000000,4096)==1 && !matching(3,0x90000000,4096));
    setup();boot.pci.span_count=PCI_BOOT_SPANS;
    for(unsigned i=1;i<PCI_BOOT_SPANS;i++)boot.pci.occupied[i]=span(0xfe,0x100000+i*4096,4096,PCI_BOOT_BRIDGE);
    assert(pci_resources_refine() && boot.pci.span_count==PCI_BOOT_SPANS && strstr(boot.pci.reason,"capacity"));restored();
    setup();boot.pci.span_count=PCI_BOOT_SPANS+1;assert(pci_resources_refine() && !f.writes);
    setup();boot.pci.status=PCI_BOOT_INCOMPLETE;strcpy(boot.pci.reason,"original capture failure");
    assert(pci_resources_refine() && !f.writes && !strcmp(boot.pci.reason,"original capture failure"));
    setup();f.cfg[4]=4;f.cfg[5]=f.cfg[6]=0;
    assert(pci_resources_refine() && boot.pci.span_count==3 && strstr(boot.pci.reason,"no assigned"));
    assert(f.cfg[4]==4 && !f.cfg[5] && !f.cfg[6] && f.cfg[1]==0xa5a50007 && !f.probing);
    puts("PCI refinement passed: independent DWORD probes/restoration, BME/status preservation, limited address masks, exact inventory/alias replacement, D3/bridge/ROM retention, once-only/host exclusion and conservative failures/capacity");
}
