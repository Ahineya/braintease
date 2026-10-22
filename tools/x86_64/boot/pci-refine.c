/* Early endpoint BAR refinement. Linux drivers/pci/probe.c pci_read_bases /
 * __pci_size_bars: suppress I/O+MEM once, independently size/restore DWORDs,
 * preserve bus mastering, restore decode before any caller can log. */
#include "kernel.h"
#include "pci.h"
#include "pci-refine.h"
static unsigned attempted;
static const char *outcome;
static PciBootSpan refined[PCI_BOOT_SPANS];
static int power_two(uint64_t n){return n && !(n&(n-1));}
static int valid_span(const PciBootSpan *s){
    return power_two(s->length) && !(s->base&(s->length-1)) &&
           s->length<=UINT64_MAX-s->base;
}
static int bar_record(const PciBootSpan *s,unsigned bdf){
    return !s->segment && s->bdf==bdf && s->bar<6 &&
           (s->kind==PCI_BOOT_BAR || s->kind==PCI_BOOT_UNKNOWN);
}
static int needs_refinement(unsigned bdf){
    for(unsigned i=0;i<boot.pci.span_count;i++){
        const PciBootSpan *s=&boot.pci.occupied[i];
        if(!bar_record(s,bdf))continue;
        if(s->kind==PCI_BOOT_UNKNOWN || !valid_span(s))return 1;
        for(unsigned j=0;j<i;j++){
            const PciBootSpan *p=&boot.pci.occupied[j];
            if(bar_record(p,bdf) && p->bar==s->bar &&
               (p->base!=s->base || p->length!=s->length))return 1;
        }
    }
    return 0;
}
static const char *probe_word(unsigned bdf,unsigned bar,uint32_t saved,uint32_t *mask){
    unsigned offset=0x10+bar*4;
    pci_write(bdf,offset,UINT32_MAX);*mask=pci_read(bdf,offset);
    pci_write(bdf,offset,saved);
    return pci_read(bdf,offset)==saved?0:"PCI refinement BAR restore refused";
}
static const char *measure(unsigned bdf,PciBootSpan measured[6],unsigned *count,unsigned *unassigned){
    uint32_t id=pci_read(bdf,0),header=pci_read(bdf,0x0c),class=pci_read(bdf,8);
    if(id==UINT32_MAX || !(id&0xffffU) || (id&0xffffU)==0xffffU)
        return "PCI refinement endpoint unavailable";
    if(((header>>16)&0x7f)!=0)return "PCI refinement requires endpoint header";
    if((class>>16)==0x0600)return "PCI refinement host bridge remains reserved";
    uint32_t saved[6];
    for(unsigned i=0;i<6;i++)saved[i]=pci_read(bdf,0x10+i*4);
    uint32_t command_status=pci_read(bdf,4);
    if(command_status==UINT32_MAX)return "PCI refinement command inaccessible";
    uint16_t command=(uint16_t)command_status;
    pci_write16(bdf,4,command&~3U);
    const char *error=0;
    if((pci_read(bdf,4)&0xffffU)!=(unsigned)(command&~3U))
        error="PCI refinement decode disable refused";
    *count=*unassigned=0;
    if(!error)for(unsigned i=0;i<6;i++){
        uint32_t low=saved[i];
        if(low&1U)continue; /* I/O resources are not MMIO reservations. */
        unsigned type=(low>>1)&3U,wide=type==2;
        if(type!=0 && !wide){error="PCI refinement reserved memory BAR type";break;}
        if(wide && i==5){error="PCI refinement truncated 64-bit BAR";break;}
        uint64_t base=low&~15U;
        if(wide)base|=(uint64_t)saved[i+1]<<32;
        if(!base){*unassigned|=1U<<i;if(wide)i++;continue;} /* Cached unassigned/D3 resources. */
        uint32_t mask_low,mask_high=0;
        error=probe_word(bdf,i,low,&mask_low);if(error)break;
        if(wide){error=probe_word(bdf,i+1,saved[i+1],&mask_high);if(error)break;}
        uint64_t mask=((uint64_t)mask_high<<32)|(mask_low&~15U);
        /* Linux pci_size: least significant implemented address bit gives
         * the extent even when upper address bits are not implemented. */
        uint64_t bytes=mask&(~mask+1);
        uint64_t above=mask+bytes;
        uint64_t width_mask=wide?(UINT64_MAX&~UINT64_C(15)):(UINT32_MAX&~15U);
        if((mask_low&15)!=(low&15) || !bytes ||
           (base==mask && ((base|(bytes-1))&width_mask)!=width_mask) ||
           (above && !power_two(above)) || (base&(bytes-1)) || bytes>UINT64_MAX-base){
            error="PCI refinement invalid size/alignment mask";break;
        }
        measured[(*count)++]=(PciBootSpan){.base=base,.length=bytes,
            .bdf=(uint16_t)bdf,.bar=(uint8_t)i,.kind=PCI_BOOT_BAR};
        if(wide)i++;
    }
    /* Always attempt every original DWORD before restoring the command.
     * Decode remains off if restoration cannot be verified. No output here. */
    unsigned restored=1;
    for(unsigned i=0;i<6;i++){
        if(pci_read(bdf,0x10+i*4)!=saved[i])pci_write(bdf,0x10+i*4,saved[i]);
        if(pci_read(bdf,0x10+i*4)!=saved[i])restored=0;
    }
    if(!restored){pci_write16(bdf,4,command&~3U);return "PCI refinement BAR rollback unverified; decode disabled";}
    pci_write16(bdf,4,command);
    if((pci_read(bdf,4)&0xffffU)!=command)return "PCI refinement command restore refused";
    return error;
}
static const char *replace(unsigned bdf,const PciBootSpan measured[6],unsigned count,unsigned unassigned){
    if(!count)return "PCI refinement no assigned memory BAR remains reserved";
    unsigned used=0;
    for(unsigned i=0;i<boot.pci.span_count;i++){
        const PciBootSpan *s=&boot.pci.occupied[i];
        if(bar_record(s,bdf)){
            unsigned cached=s->kind==PCI_BOOT_BAR && valid_span(s) && (unassigned&(1U<<s->bar));
            /* Firmware may compact 64-bit BAR indices: a measured address
             * supersedes the cached record even when its slot was wrong. */
            for(unsigned j=0;j<count;j++)if(s->base==measured[j].base)cached=0;
            if(!cached)continue;
        }
        if(used==PCI_BOOT_SPANS)return "PCI refinement reservation capacity";
        refined[used++]=*s;
    }
    if(count>PCI_BOOT_SPANS-used)return "PCI refinement reservation capacity";
    for(unsigned i=0;i<count;i++)refined[used++]=measured[i];
    for(unsigned i=0;i<used;i++)boot.pci.occupied[i]=refined[i];
    boot.pci.span_count=used;
    return 0;
}
static const char *incomplete(const char *error,unsigned bdf){
    boot.pci.status=PCI_BOOT_INCOMPLETE;
    unsigned at=0;while(error[at] && at+1<sizeof(boot.pci.reason)){
        boot.pci.reason[at]=error[at];at++;
    }
    const char tail[]=" bdf=";const char digits[]="0123456789abcdef";
    for(unsigned i=0;tail[i] && at+1<sizeof(boot.pci.reason);i++)boot.pci.reason[at++]=tail[i];
    if(at+3<sizeof(boot.pci.reason)){
        boot.pci.reason[at++]=digits[(bdf>>4)&15];boot.pci.reason[at++]=digits[bdf&15];
    }
    boot.pci.reason[at]=0;return boot.pci.reason;
}
const char *pci_resources_refine(void){
    if(attempted)return outcome;
    attempted=1;
    if(boot.pci.version!=1 || boot.pci.span_count>PCI_BOOT_SPANS || boot.pci.window_count>PCI_BOOT_WINDOWS)
        return outcome=incomplete("PCI refinement snapshot bounds/version",0);
    if(boot.pci.status!=PCI_BOOT_COMPLETE){
        boot.pci.status=PCI_BOOT_INCOMPLETE;
        return outcome=boot.pci.reason[0]?boot.pci.reason:incomplete("PCI refinement source inventory incomplete",0);
    }
    const char *first=0;
    unsigned failed_bdf=0;
    for(unsigned bdf=0;bdf<256;bdf++)if(needs_refinement(bdf)){
        PciBootSpan measured[6];unsigned count=0,unassigned=0;
        const char *error=measure(bdf,measured,&count,&unassigned);
        if(!error)error=replace(bdf,measured,count,unassigned);
        if(error && !first){first=error;failed_bdf=bdf;}
    }
    if(first)return outcome=incomplete(first,failed_bdf);
    return 0;
}
