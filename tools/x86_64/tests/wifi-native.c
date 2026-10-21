/* Native PCI handoff tested through production adapter functions. Kernel I/O
 * primitives are replaced here; transport/parser implementations stay real. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RIPLIX_X86_KERNEL_H
#define WIFI_RELAX() ((void)0)
static struct {uint64_t kernel_start,kernel_end,tsc_hz;} boot;
static uint64_t rdtsc(void){static uint64_t ticks;return ticks+=1000000;}
static char output[8192];
static unsigned output_size, console_status_calls;
static void console_write(char c){assert(output_size+1<sizeof(output));output[output_size++]=c;output[output_size]=0;}
static void console_flush(void){}
static void serial_char(char c){console_write(c);}
static void serial(const char *s){while(*s)serial_char(*s++);}
static void console_boot_status(unsigned status,const char *message,const char *detail){
 ++console_status_calls;serial(status==0?".....  ":status==1?"[ OK ] ":"[ERR ] ");serial(message);
 if(detail){serial(": ");serial(detail);}serial("\n");
}
static _Noreturn void fail(const char *s,uint64_t v){(void)s;(void)v;abort();}
static uint32_t config[64],csr[4096];
static unsigned writes,mappings,guarded,absent;
static const char *dma_error;
static unsigned failed_readback,deny_decode;
static void *device_map(uint64_t base,size_t size) {
    assert(base==0x48192c4000 && size==16384);
    assert(guarded && (config[1]&0x406)==0x402);
    assert(!(config[0x50/4]&(1U<<16)) && (config[0x60/4]&0xc0000000)==0x40000000);
    ++mappings;return csr;
}
const uint8_t wifi_payload_start[1]={0},wifi_payload_end[1]={0};
#include "../boot/wifi-native.c"
uint32_t pci_read(unsigned bdf,unsigned at){assert(bdf==0xa3 && at%4==0 && at<256);return config[at/4];}
void pci_write(unsigned bdf,unsigned at,uint32_t v){(void)bdf;(void)at;(void)v;assert(!"PCI dword write");}
void pci_write16(unsigned bdf,unsigned at,uint16_t value) {
    assert(bdf==0xa3 && at%2==0 && at<256);++writes;
    if(at==4 && (value&4))assert(guarded);
    assert(at==4 || at==0x52 || at==0x62); /* no BAR/status writes */
    if(at==4)assert(guarded);
    if(at==failed_readback)return;
    if(at==4 && deny_decode)value&=~2U;
    unsigned shift=(at&2)*8;
    config[at/4]=(config[at/4]&~(0xffffU<<shift))|(uint32_t)value<<shift;
}
int pci_find(unsigned vendor,unsigned device){assert(vendor==0x8086 && device==0x7e40);return absent?-1:0xa3;}
const char *dma_identity_check(unsigned bdf){assert(bdf==0xa3);guarded=1;return dma_error;}
static void reset_fixture(void) {
    memset(config,0,sizeof(config));memset(csr,0,sizeof(csr));
    writes=mappings=guarded=absent=deny_decode=output_size=0;output[0]=0;
    failed_readback=UINT32_MAX;dma_error=0;
    endpoint=msi=msix=0;registers=0;
    config[0]=0x7e408086;config[1]=0xa5100007;config[44/4]=0x00948086;
    config[16/4]=0x192c4004;config[20/4]=0x48;config[52/4]=0x40;
    config[0x40/4]=0x00005001;config[0x50/4]=0x00016005;config[0x60/4]=0x80000011;
    csr[0x28/4]=0x441;csr[0x9c/4]=0x2010d000;
}
int main(void) {
    assert(startup_reason(0)==0);
    const char *other="firmware response timed out";
    assert(startup_reason(other)==other);
    state=(WifiBoot){.scan_attempts=3,.scan_count=32,.scan_dropped=4,
                     .scan_channels_5=15,.scan_requested_channels_5=8};
    assert(!strcmp(startup_reason("target SSID not found after bounded passive scans"),
                   "SSID absent scans=3 AP=32 drop=4 5gmask=15/8"));
    assert(!strcmp(startup_reason("target SSID not found in completed scan"),
                   "SSID absent scans=3 AP=32 drop=4 5gmask=15/8"));
    state=(WifiBoot){.scan_attempts=UINT32_MAX,.scan_count=UINT32_MAX,
        .scan_dropped=UINT32_MAX,.scan_channels_5=UINT32_MAX,.scan_requested_channels_5=UINT32_MAX};
    const char *summary=startup_reason("target SSID not found after bounded passive scans");
    assert(summary==startup_detail && strlen(summary)==63 && summary[62]=='~');
    /* Runtime retries must not scribble over an interactive TUI. Diagnostic
     * startup profiles retain their visible progress, with serial in both. */
    console_status_calls=0;
    wifi_status(2,"WiFi initialization failed","test reason");
#ifdef BOOT_WIFI_USERLAND
    assert(console_status_calls==0);
#else
    assert(console_status_calls==1);
#endif
    assert(strstr(output,"WiFi initialization failed: test reason"));
    reset_fixture();assert(!discover() && guarded && mappings==1 && writes>0);
    assert(msi==0x50 && msix==0x60);
    assert(master(0,1));assert((config[1]&0xffff)==0x407 && (config[1]>>16)==0xa510);
    assert(!(config[0x50/4]&(1U<<16)) && (config[0x60/4]&0xc0000000)==0x40000000);
    assert(master(0,0));assert((config[1]&0xffff)==0x403);
    /* Firmware may assign the BAR but leave MSE disabled: enable it before
     * the first MMIO map, without bus mastering, IRQ delivery or BAR writes. */
    for(unsigned inherited=0;inherited<4;inherited++) {
        reset_fixture();config[1]=(config[1]&~7U)|inherited*2;
        assert(!discover() && mappings==1 && guarded);
        assert((config[1]&0x406)==0x402 && (config[1]>>16)==0xa510);
        assert(config[16/4]==0x192c4004 && config[20/4]==0x48);
        assert(strstr(output,"AX211_PCI_COMMAND_BEFORE=") && strstr(output,"AX211_PCI_COMMAND_AFTER="));
    }
    for(unsigned offset=0x52;offset<=0x62;offset+=0x10) {
        reset_fixture();failed_readback=offset;
        assert(discover());assert(!(config[1]&4) && !mappings);
    }
    reset_fixture();failed_readback=4;assert(discover() && !mappings);
    reset_fixture();config[1]&=~2U;deny_decode=1;
    assert(!strcmp(discover(),"PCI_memory_decode_readback") && !mappings && !(config[1]&4));
    for(unsigned n=0;n<13;n++) {
        reset_fixture();
        switch(n) {
        case 0:absent=1;break;
        case 1:config[44/4]=0;break;
        case 2:config[12/4]=1<<16;break;
        case 3:config[1]&=~(1U<<20);break;
        case 4:config[0x40/4]=0x4001;break; /* capability cycle */
        case 5:config[0x40/4]=0x5101;break; /* unaligned next */
        case 6:config[0x44/4]=3;break;
        case 7:config[16/4]=4;config[20/4]=0;break; /* unassigned */
        case 8:config[16/4]|=8;break;
        case 9:config[16/4]+=4096;break;
        case 10:csr[0x28/4]=UINT32_MAX;break;
        case 11:csr[0x9c/4]|=0x10000000;break;
        case 12:dma_error="IOMMU translation active";break;
        }
        const char *error=discover();assert(error);
        if(n==10 || n==11)assert(writes>0 && !(config[1]&4));
        else assert(writes==0 && mappings==0);
        if(n==7)assert(!strcmp(error,"BAR_unassigned"));
        if(n==8)assert(!strcmp(error,"BAR_type_expected_64bit_memory"));
        if(n==9)assert(!strcmp(error,"BAR_alignment"));
        if(n>=7 && n<=9)assert(strstr(output,"AX211_BAR0_LOW="));
    }
    reset_fixture();state=(WifiBoot){.scan_count=1};
    state.aps[0]=(WifiAp){.channel=6,.ssid_size=4,.ssid={27,10,'\\',128},.energy=50,.rsn=1};
    scan_results();
    assert(!strchr(output,27) && strstr(output,"\\x1b\\x0a\\x5c\\x80"));
    reset_fixture();state=(WifiBoot){0};target_result();assert(!output_size);
    for(unsigned group=0;group<3;group++) {
        reset_fixture();
        state.target=(WifiAp){.ssid_size=4,.ssid={27,10,'\\',128},.details_valid=1,
            .group_cipher=group==0?16:group==1?4:0,.group_suite=group==1?0x02ac0f00:0x04ac0f00,
            .pair_ciphers=16,.akms=4};
        target_result();
        assert(!strchr(output,27) && !strstr(output,"\\x1b") && strstr(output,"pair-CCMP=1 PSK=1 parsed=1"));
        assert(strstr(output,group==0?"group=CCMP":group==1?"group=TKIP":"group=unknown"));
        assert((strstr(output,"group encryption not implemented")!=0)==(group==1));
        assert(strstr(output,"AX211_TARGET_GROUP_SUITE="));
    }
    reset_fixture();state.prepare_complete=1;state.management_qid=1;state.management_flags=1;
    state.auth_enabled=1;state.auth_tx_done=0;state.auth_tx_status=0;state.auth_received=0;
    target_result();assert(strstr(output,"raw-flags=1 TX-verified=0") && strstr(output,"tx=0 AP-status=0 reply=0"));
    reset_fixture();state.auth_tx_done=state.auth_received=state.auth_tx_status=1;
    target_result();assert(strstr(output,"TX-verified=1") && strstr(output,"tx=1 AP-status=0 reply=1"));
    reset_fixture();state.auth_enabled=0;state.target.channel=48;
    state.mcc_channel_flags[17]=0x343;state.nvm_channel_flags[17]=0x34b;
    state.reg_country=0x414c;state.reg_source=5;state.reg_applied_country=0x3030;
    state.reg_applied_source=16;state.reg_applied_generation=state.reg_generation=1;
    target_result();
    assert(strstr(output,"ch=48 MCC=0x0000000000000343 NVM=0x000000000000034b"));
    assert(strstr(output,"detected=0x000000000000414c source=5 applied=0x0000000000003030 source=16"));
    assert(!strstr(output,"AX211_AUTH_RESULT")); /* permission failure before auth starts */
    reset_fixture();state.assoc_enabled=state.assoc_received=state.assoc_tx_status=state.assoc_complete=1;
    state.assoc_aid=7;state.assoc_step=3;target_result();
    assert(strstr(output,"AX211_ASSOC_RESULT tx=1 AP-status=0 reply=1 AID=7 updates=3 complete=1"));
    reset_fixture();state.wpa=(WifiWpa){.enabled=1,.m1_seen=1,.m2_count=2,.m3_seen=1,.m4_count=2,
        .tx_status=1,.key_installed=2,.complete=1,.last_info=0x13ca};
    state.target.group_cipher=4;
    memset(state.wpa.pmk,'Z',sizeof(state.wpa.pmk));memset(state.wpa.gtk,'Z',sizeof(state.wpa.gtk));
    target_result();
    assert(strstr(output,"AX211_WPA2_RESULT M1=1 M2=2 M3=1 M4=2 TX=1 keys=2 complete=1"));
    assert(strstr(output,"GTK firmware install replied; protected traffic unverified.") && !strstr(output,"ZZZZ"));
    reset_fixture();state.data=(WifiData){.enabled=1,.tx_status=1,.rx_packets=2,.reply=1,.complete=1,
        .drops=3,.replays=1,.last_status=0xa43,.last_pn=7};target_result();
    assert(strstr(output,"AX211_DATA_RESULT tx=1 rx=2 ARP=1 complete=1 drops=3 replays=1"));
    assert(strstr(output,"status=0x0000000000000a43 PN=0x0000000000000007"));
#ifdef BOOT_WIFI_LIVE
    state.rekey.rotations=state.rekey.duplicates=state.rekey.rejected=UINT32_MAX;
    assert(!strcmp(refresh_reason(),"GTK refreshes=4294967295 dup=4294967295 rejected=4294967295"));
    assert(strlen(refresh_reason())<64);
    reset_fixture();state=(WifiBoot){0};boot.tsc_hz=1000000000;
    NicInfo info;unsigned bytes=123;uint8_t frame[14]={0};unsigned before=writes;
    nic_init();nic_info(&info);assert(!info.link && !info.rx_frames && !info.tx_frames);
    assert(nic_tx(frame,sizeof(frame))==NIC_DOWN);
    assert(nic_rx(frame,sizeof(frame),&bytes)==NIC_DOWN && !bytes);
    nic_stop();assert(writes==before); /* absent endpoint never touches MMIO */
    WifiRestartReply control;
    wifi_control(WIFI_CONTROL_STATUS,&control);assert(!control.outcome && control.state==WIFI_STATE_ABSENT);
    wifi_control(WIFI_CONTROL_DOWN,&control);assert(!control.outcome && control.state==WIFI_STATE_ABSENT);
    wifi_control(WIFI_CONTROL_RECONNECT,&control);assert(control.outcome==WIFI_RESULT_ABSENT && control.state==WIFI_STATE_ABSENT);
    assert(writes==before);
#endif
    puts("AX211_NATIVE_HANDOFF_HOST_OK discovery/D0/BAR/variant/IOMMU/IRQ-mask/mastering/disabled-decode handoff guards");
}
