#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/bootinfo.h"
#define RIPLIX_X86_KERNEL_H
#define PCI_ASSIGN_HOST_TEST
BootInfo boot;
typedef struct {
    uint32_t low,high,probe_low,probe_high;
    uint16_t command;
    unsigned writes,command_writes,low_probe,high_probe,readback_fail,command_refuse;
} Fake;
static Fake f;
uint32_t pci_read(unsigned bdf,unsigned offset){
    assert(bdf==0xaa);
    if(offset==4)return f.command;
    if(offset==0x10){
        if(f.low_probe)return f.probe_low;
        if(f.readback_fail && f.low!=4)return f.low^0x1000;
        return f.low;
    }
    assert(offset==0x14);return f.high_probe?f.probe_high:f.high;
}
void pci_write(unsigned bdf,unsigned offset,uint32_t value){
    assert(bdf==0xaa && !(f.command&7));f.writes++;
    if(offset==0x10){f.low_probe=value==UINT32_MAX;if(!f.low_probe)f.low=value;return;}
    assert(offset==0x14);f.high_probe=value==UINT32_MAX;if(!f.high_probe)f.high=value;
}
void pci_write16(unsigned bdf,unsigned offset,uint16_t value){
    assert(bdf==0xaa && offset==4);if(!f.command_refuse)f.command=value;f.command_writes++;
}
#include "../boot/pci-assign.c"
static BootMemoryDescriptor map[2];
static void setup(void){
    memset(&boot,0,sizeof(boot));memset(&f,0,sizeof(f));memset(map,0,sizeof(map));
    f.low=4;f.command=0x407;f.probe_low=0xfffff004;f.probe_high=UINT32_MAX;
    boot.pci.version=1;boot.pci.status=PCI_BOOT_COMPLETE;boot.pci.window_count=1;
    boot.pci.windows[0]=(PciBootWindow){.base=UINT64_C(0x4000000000),.length=0x10000,.bus_last=255};
    boot.pci.span_count=1;
    boot.pci.occupied[0]=(PciBootSpan){.base=UINT64_C(0x400000f000),.length=4096,.bdf=0xab,.kind=PCI_BOOT_BAR};
    map[0]=(BootMemoryDescriptor){.type=7,.physical_start=UINT64_C(0x400000e000),.pages=1};
    map[1]=(BootMemoryDescriptor){.type=11,.physical_start=UINT64_C(0x400000b000),.pages=1};
    boot.memory_map=(uintptr_t)map;boot.memory_map_size=sizeof(map);boot.descriptor_size=sizeof(map[0]);
    boot.framebuffer=UINT64_C(0x400000d000);boot.framebuffer_size=4096;
}
static void restored(void){assert(f.command==0x407 && f.low==4 && !f.high && !f.low_probe && !f.high_probe);}
int main(void){
    uint64_t address=UINT64_MAX;const char *error;
    setup();assert(!pci_assign_bar0(0xaa,0x300,&address));
    assert(address==UINT64_C(0x400000c000) && f.low==0xc004 && f.high==0x40);
    assert(f.command==0x407 && f.writes==6 && f.command_writes==2);
    setup();boot.pci.status=PCI_BOOT_INCOMPLETE;
    assert(pci_assign_bar0(0xaa,0x300,&address) && !f.writes && !f.command_writes);restored();
    setup();f.probe_low=0xfffffc04; /* A1KiB aperture is too small for requested4KiB. */
    assert(pci_assign_bar0(0xaa,4096,&address));restored();
    setup();f.readback_fail=1;
    error=pci_assign_bar0(0xaa,0x300,&address);
    assert(error && !strcmp(error,"PCI BAR0 assignment readback"));restored();
    setup();boot.pci.occupied[0].base=UINT64_C(0x4000000000);boot.pci.occupied[0].length=0x10000;
    assert(pci_assign_bar0(0xaa,0x300,&address));restored();
    setup();boot.pci.target_bar0_base=UINT64_C(0x4000001000);boot.pci.target_bar0_length=4096;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==UINT64_C(0x4000001000));
    setup();boot.pci.target_bar0_base=UINT64_C(0x400000f000);boot.pci.target_bar0_length=4096;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==UINT64_C(0x400000c000));
    setup();f.low=0x12345004;f.high=0x48;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==UINT64_C(0x4812345000));
    assert(f.low==0x12345004 && f.high==0x48 && f.command==0x407);
    setup();f.probe_high=0xff; /* 40-bit decoder, upper mask bits unimplemented. */
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==UINT64_C(0x400000c000));
    setup();f.low=0x12345004;f.high=0x48;f.probe_low=0xffffc004;
    assert(pci_assign_bar0(0xaa,0x300,&address));
    assert(f.low==0x12345004 && f.high==0x48 && f.command==0x407);
    setup();map[1].physical_start=UINT64_C(0x4000000000);map[1].pages=16;
    assert(pci_assign_bar0(0xaa,0x300,&address));restored(); /* Fresh MMIO is reserved. */
    setup();map[1].physical_start=UINT64_C(0x4000000000);map[1].pages=16;
    boot.pci.target_bar0_base=UINT64_C(0x4000001000);boot.pci.target_bar0_length=4096;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==UINT64_C(0x4000001000));
    setup();boot.pci.windows[0].base=0x80000000;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==0x8000f000);
    setup();boot.pci.windows[0].base=0x80000000;map[0].physical_start=0x8000f000;
    assert(!pci_assign_bar0(0xaa,0x300,&address) && address==0x8000e000);
    setup();boot.pci.windows[0].base=I2C_HP_GPIO_BASE;boot.pci.windows[0].length=I2C_HP_GPIO_BYTES;
    assert(pci_assign_bar0(0xaa,0x300,&address));restored();
    setup();f.command_refuse=1;
    assert(pci_assign_bar0(0xaa,0x300,&address) && !f.writes);restored();
    setup();f.low=0x80004;
    assert(pci_assign_bar0(0xaa,0x300,&address) && f.low==0x80004 && f.command==0x407);
    setup();boot.kernel_start=0x200000;boot.kernel_end=0x210000;
    assert(pci_mmio_bounds(0x200000,4096));
    puts("PCI BAR0 size/probe/rollback, cached fallback, low apertures, MMIO/RAM/GPIO and map bounds passed");
}
