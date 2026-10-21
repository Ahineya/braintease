/* Read-only discovery for the HP Meteor Lake/AX211. This is not a driver.
 * No BAR sizing writes, reset, firmware upload, DMA, radio or IRQ changes.
 * Register offsets follow OpenBSD if_iwxreg.h (CSR_HW_REV/HW_RF_ID/GP_CNTRL).
 * In particular, never map a BAR while PCI memory decode or D0 is absent. */
#include "pci.h"
static void wifi_text(const char *s){while(*s)console_write(*s++);}
static void wifi_hex(uint64_t n){
    wifi_text("0x");for(int shift=60;shift>=0;shift-=4)console_write("0123456789abcdef"[(n>>shift)&15]);
}
void wifi_probe(void) {
    int found=pci_find(0x8086,0x7e40);
    if(found<0){wifi_text("WIFI_PROBE_SKIPPED no_ax211\n");return;}
    unsigned bdf=found;
    uint32_t identity=pci_read(bdf,0),subsystem=pci_read(bdf,44),command=pci_read(bdf,4);
    wifi_text("AX211_PCI pci=");wifi_hex(bdf);wifi_text(" identity=");wifi_hex(identity);
    wifi_text(" subsystem=");wifi_hex(subsystem);wifi_text(" command=");wifi_hex(command);wifi_text("\n");
    if(subsystem!=0x00948086U){wifi_text("WIFI_PROBE_SKIPPED different_subsystem\n");return;}
    if((pci_read(bdf,12)>>16&127)!=0){wifi_text("WIFI_PROBE_SKIPPED PCI_header\n");return;}
    uint8_t seen[256]={0};
    if(command&(1U<<20))for(unsigned cap=pci_read(bdf,52)&255;cap;){
        if(cap<64||cap>252||(cap&3)||seen[cap]){wifi_text("WIFI_PROBE_SKIPPED capability_chain\n");return;}
        seen[cap]=1;uint32_t h=pci_read(bdf,cap);
        if((h&255)==1){
            if(cap>248){wifi_text("WIFI_PROBE_SKIPPED PM_bounds\n");return;}
            unsigned state=pci_read(bdf,cap+4)&3;
            if(state){wifi_text("WIFI_PROBE_SKIPPED device_not_D0 state=");wifi_hex(state);wifi_text("\n");return;}
        }
        cap=h>>8&255;
    }
    uint32_t low=pci_read(bdf,16),high=pci_read(bdf,20);
    /* This exact HP endpoint has a 64-bit, non-prefetchable 16KiB BAR. */
    uint64_t base=((uint64_t)high<<32)|(low&~15U);
    if(!(command&2)|| (low&15)!=4 || !base || base%16384 || base>UINT64_MAX-16384){
        wifi_text("WIFI_PROBE_SKIPPED BAR_or_memory_decode\n");return;
    }
    const volatile uint32_t *csr=device_map(base,16384);
    uint32_t revision=csr[0x28/4],rf=csr[0x9c/4],control=csr[0x24/4],interface=csr[0];
    if(revision==UINT32_MAX||!revision){wifi_text("WIFI_PROBE_SKIPPED CSR_unavailable\n");return;}
    wifi_text("AX211_CSR base=");wifi_hex(base);wifi_text(" hw_rev=");wifi_hex(revision);
    wifi_text(" rf_id=");wifi_hex(rf);wifi_text(" gp_cntrl=");wifi_hex(control);wifi_text(" hw_if=");wifi_hex(interface);wifi_text("\n");
    wifi_text("AX211_READONLY_PROBE_OK no_device_writes no_firmware no_dma\n");
}
