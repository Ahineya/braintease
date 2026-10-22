/* Production platform glue with fake PCI/MMIO and module outcomes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/bootinfo.h"
#include "../boot/i2c-hp.h"
#define TOUCHPAD_GLUE_HOST_TEST 1
#define RIPLIX_X86_KERNEL_H
BootInfo boot;
static uint64_t clock_ticks;
static uint64_t rdtsc(void){clock_ticks+=1000;return clock_ticks;}
static void serial(const char *s){(void)s;}
static void console_boot_status(unsigned s,const char *m,const char *d){(void)s;(void)m;(void)d;}
static uint32_t config[64];
static unsigned lose_bar,bad_d0,bad_decode,bad_msi,bad_gpio,mapped,init_calls,stop_calls,assigned,emit_calls,last_buttons;
static const char *init_error,*poll_error;
static uint8_t mmio[4096],pins[65536];
uint32_t pci_read(unsigned bdf,unsigned off){assert(bdf==0x15*8+2 && off<256);return config[(off&252)/4];}
void pci_write(unsigned bdf,unsigned off,uint32_t value){
    assert(bdf==0x15*8+2 && off<256);
    if(off==0x10 || off==0x14)assert(!(config[1]&7));
    config[off/4]=value;
}
void pci_write16(unsigned bdf,unsigned off,uint16_t value){
    assert(bdf==0x15*8+2 && off<256);
    unsigned at=off/4,shift=(off&2)*8;
    if(off==0x44){
        if(bad_d0)return;
        if((config[at]&3)==3 && !(value&3) && lose_bar){config[4]=4;config[5]=0;}
    }
    if(off==4 && (value&2) && bad_decode)value&=~2U;
    if(off==0x52 && bad_msi)return;
    config[at]=(config[at]&~(0xffffU<<shift))|((uint32_t)value<<shift);
}
const char *pci_assign_bar0(unsigned bdf,uint64_t minimum,uint64_t *address){
    assert(bdf==0x15*8+2 && minimum==I2C_HP_CONTROLLER_BYTES && !(config[1]&7));
    assigned++;*address=((uint64_t)config[5]<<32)|(config[4]&~15U);
    if(!*address){*address=0x30000000;config[4]=0x30000004;}
    return 0;
}
const char *pci_mmio_bounds(uint64_t base,uint64_t bytes){
    if(base==I2C_HP_GPIO_BASE)return bad_gpio?"fake GPIO unavailable":0;
    assert(bytes==I2C_HP_CONTROLLER_BYTES);return base>=0x100000?0:"fake MMIO bounds";
}
void *device_map(uint64_t base,size_t bytes){
    mapped++;
    if(base==I2C_HP_GPIO_BASE){assert(bytes==I2C_HP_GPIO_BYTES);return pins;}
    assert(base==0x20000000 || base==0x30000000);assert(bytes==I2C_HP_CONTROLLER_BYTES);return mmio;
}
void graphics_pointer_report(int32_t x,int32_t y,uint32_t buttons,int32_t wheel){
    (void)x;(void)y;(void)wheel;emit_calls++;last_buttons=buttons;
}
const char *i2c_hp_init(I2cHp *d,const I2cDwIo *io,const I2cDwIo *gpio,uint64_t bar){
    init_calls++;assert(io->context==mmio && (bar==0x20000000 || bar==0x30000000));
    assert((config[1]&0x406)==0x402);assert(!(config[0x50/4]&(1U<<16)) && !(config[0x60/4]&0x80000000U));
    assert((gpio!=0)==!bad_gpio);memset(d,0,sizeof(*d));return init_error;
}
const char *i2c_hp_poll(I2cHp *d,HidI2cEmit emit,void *context){
    d->hid.reports++;
    if(!poll_error){HidI2cPointer e={.x=2,.y=-1,.buttons=1};emit(context,&e);}
    return poll_error;
}
void i2c_hp_stop(I2cHp *d){(void)d;stop_calls++;}
#include "../boot/touchpad.c"
static void setup(void){
    if(claimed || module_entered)touchpad_stop();
    memset(config,0,sizeof(config));memset(&touchpad,0,sizeof(touchpad));
    config[0]=0x7e7a8086;config[0x2c/4]=I2C_HP_PCI_SUBSYSTEM;config[1]=1U<<20;
    config[4]=0x20000004;config[0x34/4]=0x40;
    config[0x40/4]=1|(0x50<<8);config[0x44/4]=0x103;
    config[0x50/4]=5|(0x60<<8)|(1U<<16);config[0x60/4]=0x11|(0x8000U<<16);
    lose_bar=bad_d0=bad_decode=bad_msi=bad_gpio=0;mapped=init_calls=stop_calls=assigned=emit_calls=last_buttons=0;
    init_error=poll_error=0;clock_ticks=0;boot=(BootInfo){.width=1280,.height=800,.tsc_hz=1000000};
}
int main(void){
    setup();
    uint8_t map[11]={0x12,0x34,0x56,0x78,0xab,0xcd,0xef,0x90,1,2,3};
    boot.memory_map=(uintptr_t)map;boot.memory_map_size=sizeof(map);boot.descriptor_size=48;
    boot.pci.version=0x12345678;boot.pci.occupied[PCI_BOOT_SPANS-1].kind=0xab;
    assert(touchpad_status(12)==sizeof(boot.pci) && touchpad_status(13)==sizeof(map) && touchpad_status(14)==48);
    assert(touchpad_status(0x10000)==0x12345678 && touchpad_status(0x10000+sizeof(boot.pci)/4-1)==0xab000000);
    assert(touchpad_status(0x10000+sizeof(boot.pci)/4)==UINT32_MAX && touchpad_status(0x1ffff)==UINT32_MAX);
    assert(touchpad_status(0x30000)==0x78563412 && touchpad_status(0x30001)==0x90efcdab);
    assert(touchpad_status(0x30002)==UINT32_MAX && touchpad_status(0x3ffff)==UINT32_MAX);
    assert(touchpad_status(0x20000)==UINT32_MAX && touchpad_status(0x40000)==UINT32_MAX);
    boot.memory_map_size=3;assert(touchpad_status(0x30000)==UINT32_MAX);
    boot.memory_map=0;boot.memory_map_size=48;assert(touchpad_status(0x30000)==UINT32_MAX);
    setup();lose_bar=1;touchpad_init();assert(state==1 && init_calls==1 && assigned==1 && mapped==2);
    assert(config[4]==0x20000004 && bar_before[0]==bar_after[0] && !(config[0x44/4]&0x103));
    touchpad_poll();assert(last_buttons==1);touchpad_stop();assert(!claimed && !last_buttons && stop_calls==1 && !retry_armed);
    setup();config[0x60/4]=0x11|(0x40<<8);touchpad_init();assert(state==2 && strstr(reason,"cycle") && !mapped && !init_calls);
    setup();config[0x34/4]=0xfc;config[0xfc/4]=1;touchpad_init();assert(strstr(reason,"power register") && !mapped);
    setup();bad_d0=1;touchpad_init();assert(state==2 && strstr(reason,"D0") && !mapped && !claimed);
    setup();bad_decode=1;touchpad_init();assert(state==2 && strstr(reason,"memory decode") && !mapped && !(config[1]&7));
    setup();bad_msi=1;touchpad_init();assert(state==2 && strstr(reason,"MSI disable") && !mapped && !(config[1]&7));
    setup();bad_gpio=1;touchpad_init();assert(state==1 && mapped==1 && init_calls==1);
    setup();init_error="fake module init failure";touchpad_init();assert(state==2 && stop_calls==1 && !(config[1]&7));
    unsigned calls=init_calls;touchpad_poll();assert(init_calls==calls);
    for(unsigned i=0;i<3;i++){clock_ticks+=boot.tsc_hz*8;touchpad_poll();}
    assert(recovery_attempts==3 && !retry_armed && init_calls==calls+3);
    clock_ticks+=boot.tsc_hz*32;touchpad_poll();assert(init_calls==calls+3);
    setup();touchpad_init();touchpad_poll();assert(last_buttons==1);
    poll_error="fake fatal transport";clock_ticks+=boot.tsc_hz;touchpad_poll();
    assert(state==2 && !last_buttons && retry_armed && !(config[1]&7));
    uint32_t reports=touchpad_status(1);poll_error=0;clock_ticks+=boot.tsc_hz*2;touchpad_poll();
    assert(state==1 && recovery_attempts==1 && !retry_armed && touchpad_status(1)==reports);
    touchpad_stop();clock_ticks+=boot.tsc_hz*32;calls=init_calls;touchpad_poll();assert(init_calls==calls);
    setup();touchpad_init();poll_error="persistent transport";
    clock_ticks+=boot.tsc_hz;touchpad_poll();
    for(unsigned i=0;i<3;i++) {
        clock_ticks+=boot.tsc_hz*8;touchpad_poll();assert(state==1);
        clock_ticks+=boot.tsc_hz;touchpad_poll();assert(state==2);
    }
    assert(recovery_attempts==3 && !retry_armed);
    calls=init_calls;clock_ticks+=boot.tsc_hz*32;touchpad_poll();assert(init_calls==calls);
    puts("Touchpad glue checks passed: bounded little-endian resource telemetry, complete PCI capability validation, D0/readback and BAR restore, MSI/MSIX quiesce, command unwind, optional GPIO, bounded recovery and contact release");
}
