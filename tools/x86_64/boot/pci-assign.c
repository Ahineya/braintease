#include "kernel.h"
#include "pci.h"
#include "pci-resource.h"
#include "pci-assign.h"
#include "i2c-hp.h"
#ifndef PCI_ASSIGN_HOST_TEST
#include "acpi.h"
#endif
static PciResourceRange windows[PCI_BOOT_WINDOWS];
static PciResourceRange occupied[PCI_RESOURCE_MAX_OCCUPIED];
static uint64_t address_limit(void) {
#ifdef PCI_ASSIGN_HOST_TEST
    return 1ULL<<47;
#else
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000U),"c"(0));
    unsigned bits=36;
    if(a>=0x80000008U){
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000008U),"c"(0));
        bits=a&255;
    }
    if(bits<32 || bits>52)return 0;
    return 1ULL<<(bits>47?47:bits);
#endif
}
static int overlaps(uint64_t base,uint64_t length,uint64_t other,uint64_t bytes) {
    return length && bytes && base<other+bytes && other<base+length;
}
const char *pci_mmio_bounds(uint64_t base,uint64_t bytes) {
    uint64_t limit=address_limit();
    if(!limit || base<0x100000 || base>=limit || !bytes || bytes>limit-base)
        return "PCI MMIO address/mapping bounds";
    if(boot.kernel_end<boot.kernel_start ||
       overlaps(base,bytes,boot.kernel_start,boot.kernel_end-boot.kernel_start))
        return "PCI MMIO overlaps kernel";
    if(boot.framebuffer_size>UINT64_MAX-boot.framebuffer ||
       overlaps(base,bytes,boot.framebuffer,boot.framebuffer_size))
        return "PCI MMIO overlaps display";
    if(boot.descriptor_size<sizeof(BootMemoryDescriptor) ||
       boot.memory_map_size%boot.descriptor_size)return "PCI firmware memory-map bounds";
    for(uint64_t at=0;at<boot.memory_map_size;at+=boot.descriptor_size){
        const BootMemoryDescriptor *m=(const void *)(boot.memory_map+at);
        if(m->pages>(UINT64_MAX-m->physical_start)/4096)return "PCI firmware memory range overflow";
        /* Types 1..10 describe code/data/RAM, including ACPI NVS. Reserved
         * and MMIO descriptors may legitimately describe this assigned BAR. */
        if(m->type>=1 && m->type<=10 && overlaps(base,bytes,m->physical_start,m->pages*4096))
            return "PCI MMIO overlaps RAM/firmware data";
    }
    return 0;
}
static const char *append(size_t *count,uint64_t base,uint64_t length) {
    if(!length)return 0;
    if(length>UINT64_MAX-base)return "PCI reserved range overflow";
    if(*count==PCI_RESOURCE_MAX_OCCUPIED)return "PCI reserved range capacity";
    occupied[(*count)++]=(PciResourceRange){base,base+length};return 0;
}
static const char *reservations(size_t *used,unsigned bdf,int cached) {
    const char *e;*used=0;
    for(unsigned i=0;i<boot.pci.span_count;i++){
        const PciBootSpan *s=&boot.pci.occupied[i];
        if(cached && s->segment==0 && s->bdf==bdf && s->bar==0 && s->kind==PCI_BOOT_BAR)continue;
        if((e=append(used,s->base,s->length)))return e;
    }
    if(boot.descriptor_size<sizeof(BootMemoryDescriptor) ||
       boot.memory_map_size%boot.descriptor_size)return "PCI firmware memory-map bounds";
    for(uint64_t at=0;at<boot.memory_map_size;at+=boot.descriptor_size){
        const BootMemoryDescriptor *m=(const void *)(boot.memory_map+at);
        if(m->pages>UINT64_MAX/4096)return "PCI firmware memory range overflow";
        /* MMIO is reserved, not free. Only a verified cached assignment can
         * reclaim its own exact range from an MMIO container descriptor. */
        if(cached && m->type==11)continue;
        if((e=append(used,m->physical_start,m->pages*4096)))return e;
    }
    if(boot.kernel_end<boot.kernel_start)return "PCI kernel range bounds";
    if((e=append(used,boot.kernel_start,boot.kernel_end-boot.kernel_start)) ||
       (e=append(used,boot.framebuffer,boot.framebuffer_size)) ||
       (e=append(used,I2C_HP_GPIO_BASE,I2C_HP_GPIO_BYTES)))return e;
#ifndef PCI_ASSIGN_HOST_TEST
    if((e=append(used,acpi.lapic,acpi.lapic?4096:0)))return e;
    for(unsigned i=0;i<acpi.io_count;i++)if((e=append(used,acpi.io[i].address,4096)))return e;
    if(acpi.mcfg){
        if(acpi.mcfg_length<44 || (acpi.mcfg_length-44)%16)return "PCI MCFG resource bounds";
        for(unsigned at=44;at<acpi.mcfg_length;at+=16){
            const uint8_t *p=acpi.mcfg+at;uint64_t base=0;
            for(unsigned j=0;j<8;j++)base|=(uint64_t)p[j]<<(j*8);
            if(p[10]>p[11])return "PCI MCFG bus bounds";
            if((e=append(used,base,((uint64_t)p[11]+1)<<20)))return e;
        }
    }
#endif
    return 0;
}
const char *pci_assign_bar0(unsigned bdf,uint64_t minimum,uint64_t *address) {
    if(!address || !minimum || (bdf>>8))return "PCI BAR0 assignment arguments/root bus";
    uint32_t low=pci_read(bdf,0x10),high=pci_read(bdf,0x14);
    if((low&15)!=4)return "PCI BAR0 is not nonprefetchable 64-bit memory";
    uint64_t base=((uint64_t)high<<32)|(low&~15U),original=base;
    PciBootResources *r=&boot.pci;
    if(!base && (r->version!=1 || r->status!=PCI_BOOT_COMPLETE))
        return r->reason[0]?r->reason:"PCI firmware resource inventory unavailable";
    if(!base && (r->window_count>PCI_BOOT_WINDOWS || r->span_count>PCI_BOOT_SPANS))
        return "PCI assignment snapshot bounds";
    uint16_t command=pci_read(bdf,4);
    pci_write16(bdf,4,command&~7U);
    if(pci_read(bdf,4)&7){pci_write16(bdf,4,command);return "PCI BAR0 decode disable refused";}
    pci_write(bdf,0x10,UINT32_MAX);pci_write(bdf,0x14,UINT32_MAX);
    uint64_t mask=((uint64_t)pci_read(bdf,0x14)<<32)|(pci_read(bdf,0x10)&~15U);
    pci_write(bdf,0x14,high);pci_write(bdf,0x10,low);
    /* Linux pci_size: the lowest implemented address bit encodes size;
     * unimplemented upper address bits need not read back as ones. */
    uint64_t bytes=mask&(~mask+1),limit=address_limit();
    const char *error=0;
    if(pci_read(bdf,0x14)!=high || pci_read(bdf,0x10)!=low)
        error="PCI BAR0 probe restore readback";
    if(!mask || !bytes || (bytes&(bytes-1)) || bytes<minimum)error="PCI BAR0 size probe";
    if(!error && original && (original&(bytes-1)))error="PCI BAR0 address/size alignment";
    if(!error && original)error=pci_mmio_bounds(base,bytes);
    if(!error && !original){
        size_t wc=0,used=0;
        for(unsigned i=0;i<r->window_count;i++){
            const PciBootWindow *w=&r->windows[i];
            if(w->segment || w->bus_first || w->prefetch || !w->length)continue;
            if(w->length>UINT64_MAX-w->base){error="PCI root aperture overflow";break;}
            uint64_t begin=w->base<0x100000?0x100000:w->base;
            if(begin<w->base+w->length)windows[wc++]=(PciResourceRange){begin,w->base+w->length};
        }
        if(!error && !wc)error="PCI root has no nonprefetchable aperture";
        if(!error){
            unsigned cached_ok=0;
            /* The loader cache describes the exact HP target, not arbitrary BDFs. */
            if(bdf==0xaa && r->target_bar0_base && r->target_bar0_length>=bytes &&
               bytes<=UINT64_MAX-r->target_bar0_base){
                uint64_t cached=r->target_bar0_base;
                unsigned inside=0;
                for(size_t i=0;i<wc;i++)if(cached>=windows[i].start && cached<windows[i].end &&
                    bytes<=windows[i].end-cached)inside=1;
                if(inside && !reservations(&used,bdf,1)){
                    PciResourceRange one={cached,cached+bytes};
                    if(!pci_resource_select(&one,1,occupied,used,bytes,bytes,limit,&base) &&
                       !pci_mmio_bounds(base,bytes))cached_ok=1;
                }
            }
            if(!cached_ok){
                error=reservations(&used,bdf,0);
                if(!error)error=pci_resource_select(windows,wc,occupied,used,bytes,bytes,limit,&base);
            }
            if(!error)error=pci_mmio_bounds(base,bytes);
        }
    }
    if(!error && !original){
        pci_write(bdf,0x14,(uint32_t)(base>>32));pci_write(bdf,0x10,(uint32_t)base|(low&15));
        if(pci_read(bdf,0x14)!=(uint32_t)(base>>32) ||
           pci_read(bdf,0x10)!=((uint32_t)base|(low&15)))error="PCI BAR0 assignment readback";
    }
    if(error || original){pci_write(bdf,0x14,high);pci_write(bdf,0x10,low);}
    pci_write16(bdf,4,command);
    if(error)return error;
    *address=base;return 0;
}
