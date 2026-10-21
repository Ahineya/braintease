/* HP-only adapter for the startup transport. No BAR sizing or Ubuntu access.
 * This executes only AFTER ExitBootServices in an explicit Wi-Fi profile. */
#include "wifi.h"
#include "pci.h"
#include "dma.h"
#include "wifi-restart.h"
static uint8_t pool[WIFI_DMA_BYTES] __attribute__((aligned(4096)));
static WifiBoot state;
static WifiImage firmware;
static unsigned endpoint,msi,msix;
#ifdef BOOT_WIFI_LIVE
static unsigned device_present;
#endif
static volatile uint8_t *registers;
extern const uint8_t wifi_payload_start[],wifi_payload_end[];
#ifdef BOOT_WIFI_PNVM
extern const uint8_t wifi_pnvm_start[],wifi_pnvm_end[];
#endif
#ifdef BOOT_WIFI_PREPARE
#include "wifi-target.h"
#endif
#ifdef BOOT_WIFI_WPA2
#include "wifi-pmk.h"
/* Fresh CPU entropy on every boot. Never use TSC/MAC/embedded nonce seeds. */
static int nonce(uint8_t out[32]) {
    unsigned a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    unsigned max=a;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    unsigned random=c&(1U<<30),seed=0;
    if(max>=7) {
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(7),"c"(0));
        seed=b&(1U<<18);
    }
    if(!seed && !random)return 0;
    uint64_t previous=0;
    for(unsigned i=0;i<4;i++) {
        uint64_t word=0;unsigned char ready=0;
        for(unsigned n=0;n<64 && !ready;n++) {
            if(seed)__asm__ volatile("rdseed %0; setc %1":"=r"(word),"=qm"(ready)::"cc");
            else __asm__ volatile("rdrand %0; setc %1":"=r"(word),"=qm"(ready)::"cc");
            if(!ready)__asm__ volatile("pause");
        }
        if(!ready || !word || word==UINT64_MAX || (i && word==previous)){wifi_wipe(out,32);return 0;}
        for(unsigned n=0;n<8;n++)out[i*8+n]=word>>(8*n);previous=word;
    }
    return 1;
}
#endif
#ifdef BOOT_WIFI_DATA
#include "wifi-data-target.h"
#endif

static unsigned trace_only, progress;
static const char *startup_error;
static char startup_detail[64];
static int reason_equal(const char *a,const char *b) {
    if(!a || !b)return a==b;
    while(*a && *a==*b){++a;++b;}return *a==*b;
}
static const char *startup_reason(const char *cause) {
    if(!reason_equal(cause,"target SSID not found in completed scan") &&
       !reason_equal(cause,"target SSID not found after bounded passive scans"))return cause;
    WifiLiveDiagText t={startup_detail,sizeof(startup_detail),0};
    wifi_live_diag_text(&t,"SSID absent scans=");wifi_live_diag_decimal(&t,state.scan_attempts);
    wifi_live_diag_text(&t," AP=");wifi_live_diag_decimal(&t,state.scan_count);
    wifi_live_diag_text(&t," drop=");wifi_live_diag_decimal(&t,state.scan_dropped);
    wifi_live_diag_text(&t," 5gmask=");wifi_live_diag_decimal(&t,state.scan_channels_5);
    wifi_live_diag_char(&t,'/');wifi_live_diag_decimal(&t,state.scan_requested_channels_5);
    unsigned n=t.used<sizeof(startup_detail)?t.used:sizeof(startup_detail)-1;
    if(t.used>=sizeof(startup_detail))startup_detail[n-1]='~';
    startup_detail[n]=0;return startup_detail;
}
static void wifi_status(unsigned status,const char *message,const char *detail) {
#ifdef BOOT_WIFI_USERLAND
    /* The supervised service records state/reason/backoff in wifi.log. Native
     * retry telemetry belongs on serial, not in an interactive editor/shell.
     * Fatal unconfirmed DMA stops still use the platform fail diagnostic. */
    serial(status==0?"WIFI_STATUS starting ":status==1?"WIFI_STATUS ready ":"WIFI_STATUS error ");
    serial(message);if(detail){serial(": ");serial(detail);}serial("\n");
#else
    console_boot_status(status,message,detail);
#endif
}
static void emit(char c){if(trace_only)serial_char(c);else console_write(c);}
static void text(const char *s){while(*s)emit(*s++);}
static int named(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void hex_value(uint64_t value) {
    text("0x");
    for(int shift=60;shift>=0;shift-=4)emit("0123456789abcdef"[(value>>shift)&15]);
}
static void log_value(void *opaque,const char *name,uint64_t value) {
    (void)opaque;
    unsigned saved=trace_only;trace_only=1;
    text(name);text("=");hex_value(value);text("\n");trace_only=saved;
    if(named(name,"AX211_INIT_COMPLETE") && value && state.nvm_complete && !(progress&1)) {
        progress|=1;
        wifi_status(1,"WiFi device initialized",0);
        wifi_status(0,"Finding and authenticating WiFi network",0);
    }
}
static uint32_t read32(void *opaque,unsigned offset) {
    (void)opaque;return *(volatile uint32_t *)(registers+offset);
}
static void write32(void *opaque,unsigned offset,uint32_t value) {
    (void)opaque;*(volatile uint32_t *)(registers+offset)=value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static void write8(void *opaque,unsigned offset,uint8_t value) {
    (void)opaque;registers[offset]=value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#ifndef WIFI_RELAX
#define WIFI_RELAX() __asm__ volatile("pause")
#endif
static void delay(void *opaque,unsigned us) {
    (void)opaque;uint64_t start=rdtsc(),duration=(boot.tsc_hz/1000000+1)*us;
    while(rdtsc()-start<duration)WIFI_RELAX();
}
static int master(void *opaque,int enable) {
    (void)opaque;
    /* Cut off DMA first even if an inherited IRQ capability later fails its
     * readback. INTx must be disabled before choosing legacy CSR causes. */
    unsigned original=pci_read(endpoint,4)&65535;
    pci_write16(endpoint,4,(original|0x400)&~4U);
    if((pci_read(endpoint,4)&0x406)!=((original|0x400)&~4U&0x406))return 0;
    if(msi) {
        pci_write16(endpoint,msi+2,(pci_read(endpoint,msi)>>16)&~1U);
        if(pci_read(endpoint,msi)&(1U<<16))return 0;
    }
    if(msix) {
        unsigned control=pci_read(endpoint,msix)>>16;
        pci_write16(endpoint,msix+2,(control|0x4000)&~0x8000U);
        if((pci_read(endpoint,msix)>>16&0xc000)!=0x4000)return 0;
    }
    unsigned command=(pci_read(endpoint,4)&65535)|0x400; /* INTx disable */
    command=(command&~4U)|(enable?4:0);
    pci_write16(endpoint,4,command);
    unsigned actual=pci_read(endpoint,4)&65535;
    return actual==command && (!enable || (actual&2));
}
static const char *discover(void) {
    int found=pci_find(0x8086,0x7e40);
    if(found<0)return "no_ax211";
    endpoint=found;
    if(pci_read(endpoint,44)!=0x00948086 || (pci_read(endpoint,12)>>16&127))return "different_endpoint";
    uint32_t command=pci_read(endpoint,4);
    uint8_t seen[256]={0};
    if(!(command&(1U<<20)))return "missing_PCI_capabilities";
    for(unsigned cap=pci_read(endpoint,52)&255;cap;) {
        if(cap<64 || cap>252 || (cap&3) || seen[cap])return "capability_chain";
        seen[cap]=1;uint32_t header=pci_read(endpoint,cap);
        unsigned id=header&255;
        if(id==1 && (cap>248 || (pci_read(endpoint,cap+4)&3)))return "device_not_D0";
        if(id==5){if(msi)return "duplicate_MSI";msi=cap;}
        if(id==17){if(msix)return "duplicate_MSIX";msix=cap;}
        cap=(header>>8)&255;
    }
    uint32_t low=pci_read(endpoint,16),high=pci_read(endpoint,20);
    uint64_t base=((uint64_t)high<<32)|(low&~15U);
    /* Print raw firmware assignment even when validation fails. Memory decode
     * is driver-owned setup, not evidence that this BAR is unassigned. */
    log_value(0,"AX211_PCI_COMMAND_BEFORE",command&65535);
    log_value(0,"AX211_BAR0_LOW",low);log_value(0,"AX211_BAR0_HIGH",high);
    if((low&15)!=4)return "BAR_type_expected_64bit_memory";
    if(!base)return "BAR_unassigned";
    if(base%16384)return "BAR_alignment";
    if(base>UINT64_MAX-16384)return "BAR_range";
    const char *error=dma_identity_check(endpoint);
    if(error)return error;
    /* Quiesce inherited PCI DMA and IRQ delivery before enabling BAR access.
     * Never size/move the BAR, enable DMA, or change IOMMU translations here. */
    if(!master(0,0))return "PCI_DMA_or_interrupt_disable_readback";
    unsigned decode=(pci_read(endpoint,4)&65535)|2;
    pci_write16(endpoint,4,decode);
    unsigned actual=pci_read(endpoint,4)&65535;
    log_value(0,"AX211_PCI_COMMAND_AFTER",actual);
    if(actual!=decode || (actual&0x406)!=0x402)return "PCI_memory_decode_readback";
    registers=device_map(base,16384);
    uint32_t revision=read32(0,0x28),rf=read32(0,0x9c);
    log_value(0,"AX211_BAR",base);log_value(0,"AX211_HW_REV",revision);log_value(0,"AX211_RF_ID",rf);
    if(((revision&0xfff0)>>4)!=0x44 || ((rf&0xfff000)>>12)!=0x10d || (rf&0x10000000))
        return "different_MAC_RF_variant";
    return 0;
}
static void number(unsigned n) {
    if(n>=10)number(n/10);
    emit('0'+n%10);
}
static void scan_results(void) {
    text("Passive scan results (2.4/5 GHz test channels):\n");
    for(unsigned n=0;n<state.scan_count;n++) {
        WifiAp *ap=&state.aps[n];text("  ch ");number(ap->channel);text("  ");
        if(ap->energy){text("-");number(ap->energy);text(" dBm  ");}else text("signal unknown  ");
        text(ap->rsn?"RSN  ":ap->privacy?"privacy  ":"open  ");
        if(!ap->ssid_size)text("<hidden>");
        for(unsigned j=0;j<ap->ssid_size;j++) {
            unsigned c=ap->ssid[j];
            if(c>=32 && c<=126 && c!='\\')console_write(c);
            else {text("\\x");console_write("0123456789abcdef"[c>>4]);console_write("0123456789abcdef"[c&15]);}
        }
        text("\n");
    }
    text("APs: ");number(state.scan_count);text("; table drops: ");number(state.scan_dropped);
    text("; malformed beacons: ");number(state.scan_bad_frames);text("\n");
}
static void target_result(void) {
    if(!state.target.ssid_size)return;
    /* Repeat beside the FINAL outcome, after verbose transport diagnostics.
     * No SSID/password/keys, raw IEs or nearby AP details need to be printed. */
    const WifiAp *ap=&state.target;
    text("AX211_TARGET_SECURITY group=");
    text(ap->group_cipher==16?"CCMP":ap->group_cipher==4?"TKIP":"unknown");
    text(" pair-CCMP=");number(!!(ap->pair_ciphers&16));
    text(" PSK=");number(!!(ap->akms&4));
    text(" parsed=");number(ap->details_valid);text("\n");
    log_value(0,"AX211_TARGET_GROUP_SUITE",ap->group_suite);
    if(ap->channel>=36 && ap->channel<=48 && !(ap->channel%4)) {
        unsigned index=14+(ap->channel-36)/4;
        text("AX211_CHANNEL_PERMISSION ch=");number(ap->channel);
        text(" MCC=");hex_value(state.mcc_channel_flags[index]);
        text(" NVM=");hex_value(state.nvm_channel_flags[index]);text("\n");
        text("AX211_REG_PROFILE detected=");hex_value(state.reg_country);
        text(" source=");number(state.reg_source);
        text(" applied=");hex_value(state.reg_applied_country);
        text(" source=");number(state.reg_applied_source);
        text(" status=");number(state.reg_profile_status);
        text(" aux=");hex_value(state.reg_profile_aux);
        text(" generation=");number(state.reg_applied_generation);text("/");number(state.reg_generation);text("\n");
    }
    if(state.prepare_complete) {
        text("AX211_QUEUE_REPLY qid=");number(state.management_qid);
        text(" raw-flags=");number(state.management_flags);
        text(" TX-verified=");number(state.auth_tx_done);
        text(" aux=");hex_value(state.management_aux);text("\n");
    }
    if(state.auth_enabled) {
        text("AX211_AUTH_RESULT tx=");number(state.auth_tx_status);
        text(" AP-status=");number(state.auth_status);
        text(" reply=");number(state.auth_received);text("\n");
    }
    if(state.assoc_enabled) {
        text("AX211_ASSOC_RESULT tx=");number(state.assoc_tx_status);text(" AP-status=");number(state.assoc_status);
        text(" reply=");number(state.assoc_received);text(" AID=");number(state.assoc_aid);
        text(" updates=");number(state.assoc_step);text(" complete=");number(state.assoc_complete);text("\n");
    }
    if(state.wpa.enabled) {
        text("AX211_WPA2_RESULT M1=");number(state.wpa.m1_seen);text(" M2=");number(state.wpa.m2_count);
        text(" M3=");number(state.wpa.m3_seen);text(" M4=");number(state.wpa.m4_count);
        text(" TX=");number(state.wpa.tx_status);text(" keys=");number(state.wpa.key_installed);
        text(" complete=");number(state.wpa.complete);text(" info=");hex_value(state.wpa.last_info);text("\n");
    }
    if(state.data.enabled) {
        text("AX211_DATA_RESULT tx=");number(state.data.tx_status);text(" rx=");number(state.data.rx_packets);
        text(" ARP=");number(state.data.reply);text(" complete=");number(state.data.complete);
        text(" drops=");number(state.data.drops);text(" replays=");number(state.data.replays);
        text(" status=");hex_value(state.data.last_status);text(" PN=");hex_value(state.data.last_pn);text("\n");
    }
    if(ap->group_cipher==4)text(state.wpa.key_installed==2?
        "AX211_GROUP_TKIP GTK firmware install replied; protected traffic unverified.\n":
        "AX211_GROUP_TKIP_ADVERTISED station-only test; group encryption not implemented.\n");
}
void wifi_bringup(void) {
    startup_error=0;progress=0;
    if(state.attempted && !state.quiesced) {
        startup_error="device stop required before WiFi startup";
        wifi_status(2,"WiFi restart blocked",startup_error);return;
    }
    msi=msix=0;registers=0;
    wifi_status(0,"Initializing WiFi",0);
    const char *error=wifi_image_parse(wifi_payload_start,(size_t)(wifi_payload_end-wifi_payload_start),&firmware);
    if(error){startup_error=error;serial("AX211_FIRMWARE_REJECTED ");serial(error);serial("\n");wifi_status(2,"WiFi firmware rejected",error);return;}
#ifdef BOOT_WIFI_PNVM
    firmware.pnvm=(WifiSection){wifi_pnvm_start,(unsigned)(wifi_pnvm_end-wifi_pnvm_start)};
    firmware.nvm_probe=1;
#endif
#ifdef BOOT_WIFI_SCAN
    firmware.scan_probe=1;
#endif
#ifdef BOOT_WIFI_PREPARE
    for(unsigned n=0;n<sizeof(wifi_target_ssid);n++)firmware.prepare_ssid[n]=wifi_target_ssid[n];
    firmware.prepare_ssid_size=sizeof(wifi_target_ssid);
#endif
#ifdef BOOT_WIFI_AUTH
    firmware.auth_probe=1;
#endif
#ifdef BOOT_WIFI_ASSOC
    firmware.assoc_probe=1;
#endif
#ifdef BOOT_WIFI_DATA
    firmware.data_probe=1;
    for(unsigned n=0;n<4;n++)firmware.data_target[n]=wifi_data_target[n];
#endif
#ifdef BOOT_WIFI_LIVE
    firmware.live=1;
#endif
    error=discover();
    if(error){startup_error=error;serial("WIFI_BRINGUP_SKIPPED ");serial(error);serial("\n");wifi_status(2,"WiFi device unavailable",error);return;}
#ifdef BOOT_WIFI_WPA2
    firmware.wpa_probe=1;
    for(unsigned n=0;n<32;n++)firmware.pmk[n]=wifi_test_pmk[n];
    if(!nonce(firmware.snonce)) {
        startup_error="no fresh CPU entropy for WPA2 nonce";wifi_wipe(firmware.pmk,32);serial("AX211_STARTUP_FAILED no fresh CPU entropy for WPA2 nonce\n");wifi_status(2,"WiFi authentication unavailable","no fresh CPU entropy for WPA2 nonce");return;
    }
#endif
    /* Static pool is entirely in boot.kernel_start..kernel_end, so the existing
     * DMA guard's RMRR exclusion covers EVERY DMA target, not just metadata. */
    if((uintptr_t)pool<boot.kernel_start || (uintptr_t)pool+sizeof(pool)>boot.kernel_end)
        fail("Wi-Fi DMA outside checked kernel range",(uintptr_t)pool);
    state=(WifiBoot){.pool=pool,.capacity=sizeof(pool)};
    WifiOps ops={0,read32,write32,write8,delay,master,log_value};
    error=wifi_firmware_start(&state,&firmware,&ops,(uint16_t)read32(0,0x28));
    startup_error=startup_reason(error);
    if(state.attempted && !state.quiesced && !state.live.active)fail("AX211 stop unconfirmed; DMA retained",state.used);
    wifi_wipe(firmware.pmk,32);wifi_wipe(firmware.snonce,32);
    trace_only=1;target_result();trace_only=0;
    if(error){serial("AX211_STARTUP_FAILED ");serial(error);serial("\n");wifi_status(2,"WiFi initialization failed",startup_error);}
    else if(state.live.active) {
        serial("AX211_NIC_READY protected Ethernet interface active; userland owns IP configuration.\n");
        wifi_status(1,"WiFi link authenticated; interface ready",0);
    }
    else if(state.data.complete) {
        text("AX211_DATA_OK protected TX and matching CCMP ARP reply passed. Device stopped; persistent IP interface pending.\n");
    }
    else if(state.wpa.complete) {
        text("AX211_WPA2_OK authenticated M3, M4 TX and PTK/GTK command replies passed. Device stopped; protected data/IP untested.\n");
    }
    else if(state.assoc_complete) {
        text("AX211_ASSOC_OK AP accepted association; LINK/STA/MAC updated. Device stopped; WPA2 keys/IP still pending.\n");
    }
    else if(state.auth_tx_done && state.auth_received) {
        text("AX211_AUTH_OK TX completion and matching AP authentication reply. Device stopped; association/WPA2 still pending.\n");
    }
    else if(state.prepare_complete) {
        text("AX211_STATION_SETUP_REPLIES_OK station replies consumed; actual queue TX unverified. Device stopped.\n");
    }
    else if(state.scan_complete && state.scan_acked) {
        scan_results();text("AX211_SCAN_OK passive scan completed; device stopped. Association pending.\n");
    }
    else if(state.nvm_complete)text("AX211_NVM_READ_OK firmware init and NVM replied; device stopped. Scan pending.\n");
    else if(state.pnvm_complete)text("AX211_PNVM_NOTIFIED raw status retained; device stopped. NVM unchecked.\n");
    else text("AX211_STARTUP_OK firmware ALIVE; device stopped. Wi-Fi link still pending.\n");
    console_flush();
}
#ifdef BOOT_WIFI_LIVE
static WifiRestart restart;
/* Only read PCI identity during boot. Userland requests all association and
 * retry work after PID1 starts; no IP/gateway probe can prevent a local shell. */
void wifi_runtime_init(void) {
    int found=pci_find(0x8086,0x7e40);
    device_present=found>=0;
    if(!device_present)serial("WIFI_BRINGUP_SKIPPED no_ax211\n");
    console_boot_status(device_present?0:2,"WiFi managed by userland",
                        device_present?"waiting for wifi service":"no_ax211");
}
static WifiOps live_ops(void) {
    return (WifiOps){0,read32,write32,write8,delay,master,log_value};
}
static uint64_t now_us(void) {
    uint64_t t=rdtsc(),hz=boot.tsc_hz;
    if(!hz)fail("AX211 missing calibrated clock",0);
    return t/hz*1000000+(t%hz)*1000000/hz;
}
static char live_detail[64];
static void live_poll(void) {
    if(!state.live.active)return;
    WifiOps ops=live_ops();const char *error=wifi_live_poll(&state,&ops,now_us());
    if(error) {
        restart.error=error;
        if(reason_equal(error,"live prolonged beacon loss without recent RX")) {
            wifi_live_diag_summary(&state.live.diag,"beacon loss",live_detail,sizeof(live_detail));
            restart.error=live_detail;
        }
        serial("AX211_LINK_FAILED ");serial(error);serial("\n");wifi_status(2,"WiFi link stopped",restart.error);
        if(!state.quiesced)fail("AX211 stop unconfirmed; DMA retained",state.used);
    }
}
static void restart_read(void *opaque,WifiRestartState *out) {
 (void)opaque;*out=(WifiRestartState){device_present || registers!=0,state.live.active,state.attempted,state.quiesced};
}
static void restart_stop(void *opaque) {
 (void)opaque;WifiOps ops=live_ops();wifi_firmware_stop(&state,&ops);
}
static const char *restart_start(void *opaque) {
 (void)opaque;
 /* The controller has already proved DMA is quiescent. Full startup resets
  * metadata and derives a fresh CPU SNonce; no WPA key/session is reused. */
 wifi_bringup();return startup_error;
}
static char refresh_status[64];
static char *decimal(char *p,uint32_t value) {
 char digits[10];unsigned n=0;do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);
 while(n)*p++=digits[--n];return p;
}
static const char *refresh_reason(void) {
 const char *label="GTK refreshes=";char *p=refresh_status;
 while(*label)*p++=*label++;p=decimal(p,state.rekey.rotations);
 label=" dup=";while(*label)*p++=*label++;p=decimal(p,state.rekey.duplicates);
 label=" rejected=";while(*label)*p++=*label++;p=decimal(p,state.rekey.rejected);*p=0;
 return refresh_status; /* At most59 ASCII characters, including u32 maxima. */
}
void wifi_control(unsigned operation,WifiRestartReply *out) {
 if(operation==WIFI_CONTROL_STATUS)live_poll();
 WifiRestartOps ops={0,restart_read,restart_stop,restart_start};
 wifi_restart(&restart,&ops,operation,out);
 if(out->state==WIFI_STATE_UP && !out->error) {
  out->error=refresh_reason(); /* ABI1 bounded status/reason text, no key data. */
 }
}
void nic_init(void){live_poll();}
void nic_info(NicInfo *out){live_poll();wifi_live_info(&state,out);}
unsigned nic_tx(const uint8_t *data,unsigned n) {
    live_poll();WifiOps ops=live_ops();unsigned result=wifi_live_tx(&state,&ops,data,n,now_us());
    if(state.attempted && !state.live.active && !state.quiesced)fail("AX211 stop unconfirmed; DMA retained",state.used);
    return result;
}
unsigned nic_rx(uint8_t *out,unsigned capacity,unsigned *length) {
    live_poll();return wifi_live_rx(&state,out,capacity,length);
}
void nic_stop(void) {
    if(!state.attempted || state.quiesced)return;
    WifiOps ops=live_ops();wifi_firmware_stop(&state,&ops);
    if(!state.quiesced)fail("AX211 stop unconfirmed; DMA retained",state.used);
    text("NETWORK_QUIESCED\n");console_flush();
}
#endif
