/* Production live RX + startup/stop fixtures; no RF or console dependencies.
 * References reviewed without changing policy: Linux v6.12 mac.h API4,
 * mvm/mac-ctxt.c iwl_mvm_handle_missed_beacons_notif and mvm.h (19 AND4);
 * https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/intel/iwlwifi
 * OpenBSD if_iwx.c rev1.230, iwx_rx_bmiss, source SHA256
 * ade02a314dd9aa63ccc7c87df76099c22f76ec172d42e05a7ad72e0d39450d57
 * (build/x86_64-network/wifi-reference/sources.json). OpenBSD's directed-probe
 * behavior is NOT implemented by a diagnostics change. */
#define WIFI_LIVE_MAIN existing_live_checks
#include "wifi-live.c"
static struct {const char *name;uint64_t value;} diagnostic_log[32];
static unsigned diagnostic_count;
static void capture(void *opaque,const char *name,uint64_t value) {
    (void)opaque;assert(diagnostic_count<32 && !strncmp(name,"AX211_",6));
    diagnostic_log[diagnostic_count].name=name;diagnostic_log[diagnostic_count++].value=value;
}
static uint64_t logged(const char *name) {
    for(unsigned i=0;i<diagnostic_count;i++)if(!strcmp(name,diagnostic_log[i].name))return diagnostic_log[i].value;
    assert(!"missing diagnostic field");return 0;
}
static void missed(WifiBoot *b,unsigned since,unsigned consecutive,unsigned expected,unsigned received,unsigned link) {
    uint8_t *p=next(b);p32(p,24);p[4]=0xa2;p[7]=0x80;
    p32(p+8,link);p32(p+12,since);p32(p+16,consecutive);
    p32(p+20,expected);p32(p+24,received);publish(b,1);
}
static void diagnostics(void) {
    WifiBoot b;Device device;start_live(&b,&device);WifiOps ops=live_ops(&device);ops.log=capture;
    diagnostic_count=0;assert(b.live.diag.channel==48 && !b.live.diag.seen);
    assert(!wifi_live_poll(&b,&ops,0));
    assert(b.live.diag.polls==1 && (b.live.diag.seen&WIFI_LIVE_DIAG_POLL));
    assert(wifi_live_diag_ap_age(&b.live.diag)==UINT64_MAX && !diagnostic_count);
    uint8_t *p=next(&b);arp(p,&b,1);publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1000000));
    assert(b.live.diag.ap_frames==1 && b.live.diag.data_frames==1);
    assert(b.live.diag.last_ap_rx_us==1000000 && b.live.diag.last_data_rx_us==1000000);
    p=next(&b);arp(p,&b,2);p32(p+8+12,0);publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1500000));assert(b.live.diag.ap_frames==1);
    p=next(&b);arp(p,&b,2);p[8+42]=44;publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1700000));assert(b.live.diag.ap_frames==1);
    p=next(&b);arp(p,&b,2);p32(p+8+12,0x203);publish(&b,1); /* RF frame, failed MIC. */
    assert(!wifi_live_poll(&b,&ops,2000000));
    assert(b.live.diag.ap_frames==2 && b.live.diag.data_frames==1);
    assert(b.live.diag.last_ap_rx_us==2000000 && b.live.diag.last_data_rx_us==1000000);
    missed(&b,9,9,UINT32_MAX,UINT32_MAX-1,0);
    assert(!wifi_live_poll(&b,&ops,3000000));
    assert(!diagnostic_count && b.live.active && b.live.diag.missed_notifications==1);
    assert(b.live.diag.missed_expected==UINT32_MAX && b.live.diag.missed_received==UINT32_MAX-1);
    missed(&b,3,19,0,UINT32_MAX,0);assert(!wifi_live_poll(&b,&ops,3500000));
    assert(!diagnostic_count && b.live.diag.missed_notifications==2 && b.live.diag.missed_expected==0);
    missed(&b,4,19,1,0,0);const char *error=wifi_live_poll(&b,&ops,4000000);
    assert(error && !strcmp(error,"live prolonged beacon loss without recent RX"));
    assert(b.quiesced && !device.master && !b.live.active && b.live.diag.failed);
    assert(b.live.diag.failed_at_us==4000000 && b.live.diag.missed_notifications==3);
    assert(wifi_live_diag_ap_age(&b.live.diag)==2000000 && wifi_live_diag_data_age(&b.live.diag)==3000000);
    assert(logged("AX211_BEACON_MISSED_CONSECUTIVE")==19 && logged("AX211_BEACON_MISSED_SINCE_RX")==4);
    assert(logged("AX211_BEACON_EXPECTED")==1 && logged("AX211_BEACON_RECEIVED")==0);
    assert(logged("AX211_LIVE_AP_AGE_US")==2000000 && logged("AX211_LIVE_POLL_GAP_US")==500000);
    char summary[64];wifi_live_diag_summary(&b.live.diag,"beacon loss",summary,sizeof(summary));
    assert(!strcmp(summary,"beacon loss ch48 m19/r4 gap500ms age2000ms"));
    WifiLiveDiag frozen=b.live.diag;unsigned logs=diagnostic_count;
    assert(!wifi_live_poll(&b,&ops,5000000));
    assert(!memcmp(&frozen,&b.live.diag,sizeof(frozen)) && logs==diagnostic_count);
    /* An invalid link's raw counts must not overwrite the last valid report,
     * and its validation failure must retain its exact original cause. */
    start_live(&b,&device);ops=live_ops(&device);ops.log=capture;diagnostic_count=0;
    missed(&b,3,19,10,9,0);assert(!wifi_live_poll(&b,&ops,100));
    missed(&b,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,7);error=wifi_live_poll(&b,&ops,200);
    assert(error && !strcmp(error,"live missed-beacon notification bounds/link"));
    assert(b.live.diag.missed_notifications==1 && b.live.diag.missed_expected==10);
}
static void clocks_counters_and_format(void) {
    WifiLiveDiag d;wifi_live_diag_init(&d,48);
    wifi_live_diag_poll(&d,UINT64_MAX-5);wifi_live_diag_ap_rx(&d,UINT64_MAX-3);
    wifi_live_diag_poll(&d,5);assert(d.poll_gap_us==11 && !d.clock_backwards);
    assert(wifi_live_diag_ap_age(&d)==9);
    wifi_live_diag_poll(&d,1000);wifi_live_diag_ap_rx(&d,1000);wifi_live_diag_data_rx(&d,1000);
    wifi_live_diag_poll(&d,900);assert(!d.poll_gap_us && d.clock_backwards==1);
    assert(wifi_live_diag_ap_age(&d)==UINT64_MAX && wifi_live_diag_data_age(&d)==UINT64_MAX);
    wifi_live_diag_poll(&d,900);assert(!d.poll_gap_us && d.clock_backwards==1);
    d.polls=d.ap_frames=d.data_frames=d.missed_notifications=d.clock_backwards=UINT32_MAX;
    wifi_live_diag_poll(&d,800);wifi_live_diag_ap_rx(&d,800);wifi_live_diag_data_rx(&d,800);
    wifi_live_diag_missed(&d,800,UINT32_MAX,UINT32_MAX,UINT32_MAX,0);
    assert(d.polls==UINT32_MAX && d.ap_frames==UINT32_MAX && d.data_frames==UINT32_MAX);
    assert(d.missed_notifications==UINT32_MAX && d.clock_backwards==UINT32_MAX);
    wifi_live_diag_missed(&d,801,0,0,0,UINT32_MAX);
    assert(!d.missed_expected && d.missed_received==UINT32_MAX && !d.missed_since_rx);
    wifi_live_diag_fail(&d,900);WifiLiveDiag before=d;
    wifi_live_diag_poll(&d,5000);wifi_live_diag_ap_rx(&d,5000);wifi_live_diag_data_rx(&d,5000);
    wifi_live_diag_missed(&d,5000,1,1,1,1);wifi_live_diag_fail(&d,5000);
    assert(!memcmp(&before,&d,sizeof(d)));
    const char *cause="original cause";char complete[256];
    unsigned needed=wifi_live_diag_summary(&d,cause,complete,sizeof(complete));
    assert(needed==strlen(complete) && !strncmp(complete,cause,strlen(cause)));
    for(unsigned capacity=0;capacity<needed+3;capacity++) {
        unsigned char guarded[256];memset(guarded,0xa5,sizeof(guarded));
        assert(wifi_live_diag_summary(&d,cause,capacity?(char *)guarded+1:0,capacity)==needed);
        assert(guarded[0]==0xa5 && guarded[capacity+1]==0xa5);
        if(capacity){assert(strlen((char *)guarded+1)<capacity);assert(!guarded[capacity<needed+1?capacity:needed+1]);}
        if(capacity>1 && capacity<=needed)assert(guarded[capacity-1]=='~');
    }
    assert(!memcmp(&before,&d,sizeof(d)) && !strcmp(cause,"original cause"));
    wifi_live_diag_init(&d,1);assert(!d.failed && !d.seen && !d.polls && d.channel==1);
    wifi_live_diag_summary(&d,0,complete,sizeof(complete));
    assert(!strcmp(complete,"link stopped ch1 gap0ms age?"));
}
int main(int argc,char **argv) {
    assert(argc==3);clocks_counters_and_format();
    FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
    blob=malloc(size);assert(blob && fread(blob,1,size,file)==(size_t)size);fclose(file);
    assert(!wifi_image_parse(blob,size,&image));file=fopen(argv[2],"rb");assert(file);
    fseek(file,0,SEEK_END);size=ftell(file);rewind(file);uint8_t *pnvm=malloc(size);
    assert(pnvm && fread(pnvm,1,size,file)==(size_t)size);fclose(file);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};
    image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=image.wpa_probe=image.live=1;
    memcpy(image.pmk,vector_pmk,32);memcpy(image.snonce,vector_snonce,32);
    image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);
    auth_fault=AUTH_OK;assoc_fault=ASSOC_OK;scan_fault=SCAN_OK;wpa_fault=WPA_OK;
    key_fault=0;beacon_group=2;fault_kind=update_fault=0;expected_basic_ofdm=0x15;
    diagnostics();free(blob);free(pnvm);
    puts("AX211_LIVE_DIAG_HOST_OK raw counters, valid AP versus delivered data, original failures, frozen stop context, quiet warnings, clock wrap/backwards, saturation, bounded formatter; RF untested");
}
