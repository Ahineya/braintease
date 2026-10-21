/* Verify that discovery cannot program the radio, PCI BARs or DMA. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define RIPLIX_X86_KERNEL_H
static uint32_t config[64],registers[4096];
static int found;static unsigned maps;static char logbuf[2048];
static void console_write(char c){size_t n=strlen(logbuf);assert(n+1<sizeof(logbuf));logbuf[n]=c;logbuf[n+1]=0;}
static void *device_map(uint64_t at,size_t n){assert(at==0x48192c4000ULL&&n==16384);++maps;return registers;}
#include "../boot/wifi-probe.c"
int pci_find(unsigned vendor,unsigned device){assert(vendor==0x8086&&device==0x7e40);return found;}
uint32_t pci_read(unsigned bdf,unsigned off){assert(bdf==0xa3&&off%4==0&&off<256);return config[off/4];}
/* No pci_write/pci_write16 definition: accidental writes fail to link. */
static void setup(void){
    memset(config,0,sizeof(config));memset(registers,0,sizeof(registers));
    found=0xa3;maps=0;logbuf[0]=0;
    config[0]=0x7e408086;config[1]=0x00100002;config[11]=0x00948086;
    config[4]=0x192c4004;config[5]=0x48;config[13]=0xc8;
    config[0xc8/4]=1;registers[0x28/4]=0x441;registers[0x9c/4]=0x2010d000;
}
static void run(const char *expected,unsigned mapped){
    uint32_t before_config[64],before_registers[4096];
    memcpy(before_config,config,sizeof(config));memcpy(before_registers,registers,sizeof(registers));
    wifi_probe();assert(maps==mapped&&strstr(logbuf,expected));
    assert(!memcmp(before_config,config,sizeof(config))&&!memcmp(before_registers,registers,sizeof(registers)));
}
int main(void){
    setup();run("AX211_READONLY_PROBE_OK",1);
    setup();found=-1;run("no_ax211",0);
    setup();config[11]^=1;run("different_subsystem",0);
    setup();config[3]=0x10000;run("PCI_header",0);
    setup();config[13]=0xc9;run("capability_chain",0);
    setup();config[0xc8/4]=0xc801;run("capability_chain",0);
    setup();config[0xcc/4]=3;run("device_not_D0",0);
    setup();config[1]&=~2U;run("BAR_or_memory_decode",0);
    setup();config[4]|=1;run("BAR_or_memory_decode",0);
    setup();config[4]+=4096;run("BAR_or_memory_decode",0);
    setup();config[4]=4;config[5]=0;run("BAR_or_memory_decode",0);
    setup();registers[0x28/4]=UINT32_MAX;run("CSR_unavailable",1);
    setup();registers[0x28/4]=0;run("CSR_unavailable",1);
    puts("PASS: read-only AX211 discovery, high BAR, power/decode guards and unchanged PCI/CSR state");
}
