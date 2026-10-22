/* Exact HP profile; bounded polling, no GPIO configuration or DMA. */
#include "kernel.h"
#include "pci.h"
#include "i2c-hp.h"
#include "touchpad.h"
#include "pci-assign.h"
extern void graphics_pointer_report(int32_t,int32_t,uint32_t,int32_t);
static I2cHp touchpad;
static volatile uint8_t *controller,*gpio;
static unsigned state,contact,contact_id,claimed,module_entered;
static unsigned retry_armed,retry_count,recovery_attempts;
static uint16_t saved_command;
static int32_t last_x,last_y;
static int64_t pointer_x,pointer_y;
static uint64_t last_poll,retry_at;
static uint32_t report_base,malformed_base;
static const char *reason;
static uint32_t bar_before[2],bar_after[2];
static unsigned device_bdf=0x15*8+2;
static uint32_t read32(void *address,unsigned offset) {
    return *(const volatile uint32_t *)((const volatile uint8_t *)address+offset);
}
static void write32(void *address,unsigned offset,uint32_t value) {
    *(volatile uint32_t *)((volatile uint8_t *)address+offset)=value;
}
static void delay(void *ignored,unsigned us) {
    (void)ignored;
    uint64_t ticks=(boot.tsc_hz/1000000)*us;
    uint64_t start=rdtsc();while(rdtsc()-start<ticks) {
#ifndef TOUCHPAD_GLUE_HOST_TEST
        __asm__ volatile("pause");
#endif
    }
}
/* Complete the standard capability walk before changing any device state. */
static const char *capabilities(unsigned *pm,unsigned *msi,unsigned *msix) {
    uint32_t command=pci_read(device_bdf,4);
    if(command==UINT32_MAX || !(command&(1U<<20)))return "PCI power capability missing";
    uint64_t seen=0;
    unsigned cap=pci_read(device_bdf,0x34)&255;
    for(unsigned n=0;cap && n<48;n++) {
        if(cap<0x40 || cap>0xfc || (cap&3))return "PCI capability bounds";
        uint64_t bit=UINT64_C(1)<<(cap/4);
        if(seen&bit)return "PCI capability cycle";
        seen|=bit;
        uint32_t entry=pci_read(device_bdf,cap);
        if(entry==UINT32_MAX)return "PCI capability inaccessible";
        unsigned *slot=0;
        if((entry&255)==1){if(cap>0xf8)return "PCI power register bounds";slot=pm;}
        else if((entry&255)==5)slot=msi;
        else if((entry&255)==0x11)slot=msix;
        if(slot){if(*slot)return "PCI duplicate capability";*slot=cap;}
        cap=(entry>>8)&255;
    }
    if(cap)return "PCI capability chain incomplete";
    return *pm?0:"PCI power capability missing";
}
static void release_contact(void) {
    contact=0;contact_id=0;
    graphics_pointer_report(pointer_x/65536,pointer_y/65536,0,0);
}
static void unwind(void) {
    if(module_entered){i2c_hp_stop(&touchpad);module_entered=0;}
    if(claimed){pci_write16(device_bdf,4,(saved_command|0x400U)&~4U);claimed=0;}
    controller=gpio=0;
}
static const char *initialize(void) {
    unsigned pm=0,msi=0,msix=0;
    const char *error=capabilities(&pm,&msi,&msix);if(error)return error;
    saved_command=(uint16_t)pci_read(device_bdf,4);
    bar_before[0]=pci_read(device_bdf,0x10);bar_before[1]=pci_read(device_bdf,0x14);
    uint16_t power=(uint16_t)pci_read(device_bdf,pm+4);
    if(power==UINT16_MAX)return "PCI power state inaccessible";
    /* Linux pci_power_up: D0, PME disabled, mandatory D3/D2 transition delay. */
    pci_write16(device_bdf,pm+4,power&~0x8103U);
    if((power&3)==3)delay(0,10000);else if((power&3)==2)delay(0,200);
    uint16_t after=(uint16_t)pci_read(device_bdf,pm+4);
    if(after==UINT16_MAX || (after&3))return "PCI refused D0";
    /* Quiesce decode/master before restoring a BAR reset by the D0 transition. */
    claimed=1;pci_write16(device_bdf,4,saved_command&~7U);
    if(pci_read(device_bdf,4)&7U)return "PCI decode disable refused";
    uint32_t low=pci_read(device_bdf,0x10),high=pci_read(device_bdf,0x14);
    uint64_t before=((uint64_t)bar_before[1]<<32)|(bar_before[0]&~15U);
    uint64_t now=((uint64_t)high<<32)|(low&~15U);
    if(!now && before && (bar_before[0]&15)==4 && !pci_mmio_bounds(before,I2C_HP_CONTROLLER_BYTES)) {
        pci_write(device_bdf,0x14,bar_before[1]);pci_write(device_bdf,0x10,bar_before[0]);
        if(pci_read(device_bdf,0x10)!=bar_before[0] || pci_read(device_bdf,0x14)!=bar_before[1])
            return "PCI D0 BAR restore refused";
    }
    bar_after[0]=pci_read(device_bdf,0x10);bar_after[1]=pci_read(device_bdf,0x14);
    /* INTx-disable does not disable MSI/MSI-X. Polling owns no IRQ route. */
    if(msi){uint16_t control=pci_read(device_bdf,msi)>>16;pci_write16(device_bdf,msi+2,control&~1U);if(pci_read(device_bdf,msi)&(1U<<16))return "PCI MSI disable refused";}
    if(msix){uint16_t control=pci_read(device_bdf,msix)>>16;pci_write16(device_bdf,msix+2,(control&~0x8000U)|0x4000U);if(pci_read(device_bdf,msix)&0x80000000U)return "PCI MSI-X disable refused";}
    uint64_t base=0;
    error=pci_assign_bar0(device_bdf,I2C_HP_CONTROLLER_BYTES,&base);if(error)return error;
    error=pci_mmio_bounds(base,I2C_HP_CONTROLLER_BYTES);if(error)return error;
    uint16_t command=(saved_command|0x402U)&~4U;
    pci_write16(device_bdf,4,command);
    if(((uint16_t)pci_read(device_bdf,4)&0x406U)!=(command&0x406U))return "PCI memory decode refused";
    controller=device_map(base,I2C_HP_CONTROLLER_BYTES);
    /* Unconfigured/uncertain GPIO IRQ resources do not preclude polled HID. */
    if(!pci_mmio_bounds(I2C_HP_GPIO_BASE,I2C_HP_GPIO_BYTES))gpio=device_map(I2C_HP_GPIO_BASE,I2C_HP_GPIO_BYTES);
    I2cDwIo io={(void *)controller,read32,write32,delay};
    I2cDwIo pins={(void *)gpio,read32,0,delay};
    report_base+=touchpad.hid.reports;malformed_base+=touchpad.hid.malformed;
    module_entered=1;
    return i2c_hp_init(&touchpad,&io,gpio?&pins:0,base);
}
static void failed(const char *error,unsigned booting) {
    reason=error;unwind();release_contact();state=2;
    serial(booting?"TOUCHPAD_UNAVAILABLE ":"TOUCHPAD_STOPPED ");serial(reason);serial("\n");
    if(booting)console_boot_status(2,"Touchpad unavailable",reason);
}
void touchpad_init(void) {
    retry_armed=retry_count=recovery_attempts=0;
    state=0;reason=0;report_base=malformed_base=0;last_poll=0;contact=contact_id=0;
    pointer_x=(int64_t)(boot.width/2)*65536;pointer_y=(int64_t)(boot.height/2)*65536;
    if(pci_read(device_bdf,0)!=0x7e7a8086 || pci_read(device_bdf,0x2c)!=I2C_HP_PCI_SUBSYSTEM)return;
    const char *error=initialize();
    if(error){failed(error,1);retry_armed=1;retry_at=rdtsc()+boot.tsc_hz;return;}
    state=1;serial("TOUCHPAD_READY HID_I2C polling\n");console_boot_status(1,"Touchpad initialized",0);
}
static void pointer_event(void *ignored,const HidI2cPointer *e) {
    (void)ignored;
    if(e->absolute) {
        if(e->contact && contact && e->contact_id==contact_id) {
            int64_t dx=(int64_t)e->x-last_x,dy=(int64_t)e->y-last_y;
            int64_t xr=(int64_t)e->x_max-e->x_min,yr=(int64_t)e->y_max-e->y_min;
            if(xr>0)pointer_x+=dx*boot.width*65536/xr;
            if(yr>0)pointer_y+=dy*boot.height*65536/yr;
        }
        contact=e->contact;contact_id=e->contact_id;last_x=e->x;last_y=e->y;
    } else {pointer_x+=(int64_t)e->x*65536;pointer_y+=(int64_t)e->y*65536;}
    int64_t xmax=(int64_t)(boot.width-1)*65536,ymax=(int64_t)(boot.height-1)*65536;
    if(pointer_x<0)pointer_x=0;if(pointer_x>xmax)pointer_x=xmax;
    if(pointer_y<0)pointer_y=0;if(pointer_y>ymax)pointer_y=ymax;
    int64_t wheel=(int64_t)e->wheel*65536;
    if(wheel>INT32_MAX)wheel=INT32_MAX;if(wheel<INT32_MIN)wheel=INT32_MIN;
    graphics_pointer_report(pointer_x/65536,pointer_y/65536,e->buttons,(int32_t)wheel);
}
void touchpad_poll(void) {
    uint64_t now=rdtsc();
    if(state==2 && retry_armed) {
        if(now<retry_at)return;
        ++retry_count;++recovery_attempts;
        const char *error=initialize();
        if(!error){state=1;reason=0;retry_armed=0;serial("TOUCHPAD_RECOVERED\n");return;}
        failed(error,0);
        if(retry_count>=3){retry_armed=0;return;}
        retry_at=rdtsc()+boot.tsc_hz*(UINT64_C(1)<<retry_count);return;
    }
    if(state!=1 || now-last_poll<boot.tsc_hz/100)return;
    last_poll=now;
    uint64_t reports=touchpad.hid.reports;
    const char *error=i2c_hp_poll(&touchpad,pointer_event,0);
    if(error){
        failed(error,0);
        retry_armed=retry_count<3;
        retry_at=rdtsc()+boot.tsc_hz*(UINT64_C(1)<<retry_count);
    } else if(touchpad.hid.reports>reports)retry_count=0;
    /* A successful reinitialization alone cannot reset the recovery budget:
     * require a valid input report before treating a persistent bus as healed. */
}
void touchpad_stop(void) {retry_armed=0;unwind();release_contact();state=0;}
const char *touchpad_error(void) {return reason?reason:"";}
uint32_t touchpad_status(unsigned field) {
    /* Read-only firmware resource telemetry, never a caller-supplied address.
     * Full words only; both namespaces are capped at 65,536 word indices. */
    const uint8_t *bytes=0;
    uint64_t length=0;
    if(field>=0x10000U && field<0x20000U){bytes=(const uint8_t *)&boot.pci;length=sizeof(boot.pci);}
    else if(field>=0x30000U && field<0x40000U){bytes=(const uint8_t *)(uintptr_t)boot.memory_map;length=boot.memory_map_size;}
    if(bytes) {
        uint64_t offset=(uint64_t)(field&0xffffU)*4;
        if(length<4 || offset>length-4)return UINT32_MAX;
        return (uint32_t)bytes[offset]|(uint32_t)bytes[offset+1]<<8|
               (uint32_t)bytes[offset+2]<<16|(uint32_t)bytes[offset+3]<<24;
    }
    switch(field) {
    case 0:return state;
    case 1:return report_base+touchpad.hid.reports;
    case 2:return malformed_base+touchpad.hid.malformed;
    case 3:return (uint32_t)touchpad.controller.abort_source;
    case 4:return pointer_x/65536;
    case 5:return pointer_y/65536;
    case 7:return bar_before[0];case 8:return bar_before[1];
    case 9:return bar_after[0];case 10:return bar_after[1];
    case 11:return recovery_attempts;
    case 12:return sizeof(boot.pci);
    case 13:return boot.memory_map_size;
    case 14:return boot.descriptor_size;
    default:return UINT32_MAX;
    }
}
