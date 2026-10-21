/* Same production firmware/scan/setup/auth path; real wire bytes, fake MMIO.
 * Tests do not claim physical TX or access-point acceptance. */
#define WIFI_PREPARE_MAIN wifi_prepare_tests
#include "wifi-prepare.c"
enum {AUTH_OK,AUTH_NO_TX,AUTH_NO_AP,AUTH_TX_FAIL,AUTH_AP_FAIL,AUTH_TX_QID,
      AUTH_TX_SSN,AUTH_TX_SHORT,AUTH_DUP_TX,AUTH_AP_FIRST,AUTH_NO_START,
      AUTH_NO_ACK,AUTH_SESSION_FAIL,AUTH_SESSION_END,AUTH_WRONG_AP,
      AUTH_WRONG_DEST,AUTH_PROTECTED,AUTH_CRC,AUTH_CHANNEL,AUTH_TRANSACTION,
      AUTH_PAD,AUTH_START_FIRST,AUTH_TX_COUNT,AUTH_SESSION_SHORT,AUTH_SESSION_ID,
      AUTH_CMD_FAILED,AUTH_PASSIVE,AUTH_RADAR,AUTH_RATE12,AUTH_NO_ANT,AUTH_COUNTRY,AUTH_COUNTRY_PASSIVE,AUTH_COUNTRY_RACE,AUTH_HP_FLAGS,AUTH_TX_PAYLOAD_QID,AUTH_SSN_HIGH,AUTH_SSN_BITS12,
      AUTH_TX_TAIL,AUTH_ACK_PAYLOAD,AUTH_DISABLED,AUTH_MISSING_BSS,AUTH_TX_HDRINDEX,AUTH_LAST};
static unsigned auth_fault;
static void tx_completion(uint8_t *p) {
    memset(p,0,4096);p32(p,52);p[4]=0x1c;p[7]=1;uint8_t *d=p+8;
    d[0]=1;d[36]=1;d[40]=1;p32(d+44,1);
    if(auth_fault==AUTH_TX_FAIL)d[40]=0x82;
    if(auth_fault==AUTH_TX_QID)p[7]=2; /* ignored legacy header, payload authoritative */
    if(auth_fault==AUTH_TX_HDRINDEX)p[6]=99;
    if(auth_fault==AUTH_TX_PAYLOAD_QID)d[36]=2;
    if(auth_fault==AUTH_SSN_HIGH)p32(d+44,0xa5a50001);
    if(auth_fault==AUTH_SSN_BITS12)p32(d+44,0x1001);
    if(auth_fault==AUTH_TX_TAIL)p32(p,60);
    if(auth_fault==AUTH_TX_SSN)p32(d+44,2);
    if(auth_fault==AUTH_TX_SHORT)p32(p,51);
    if(auth_fault==AUTH_TX_COUNT)d[0]=2;
}
static void ap_auth(uint8_t *p,const WifiBoot *b) {
    memset(p,0,4096);unsigned pad=auth_fault==AUTH_PAD?2:0,len=30+pad;
    p32(p,4+64+len);p[4]=0xc1;p[7]=0x80;uint8_t *d=p+8,*f=d+64;
    d[0]=len;d[3]=pad?0x20:0;p32(d+12,3);d[42]=48;f[0]=0xb0;
    memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);memcpy(f+16,b->target.bssid,6);
    f[24+pad+2]=2;
    if(auth_fault==AUTH_AP_FAIL)f[24+pad+4]=13;
    if(auth_fault==AUTH_WRONG_AP)f[10]^=2;
    if(auth_fault==AUTH_WRONG_DEST)f[4]^=2;
    if(auth_fault==AUTH_PROTECTED)f[1]=0x40;
    if(auth_fault==AUTH_CRC)p32(d+12,0);
    if(auth_fault==AUTH_CHANNEL)d[42]=44;
    if(auth_fault==AUTH_TRANSACTION)f[24+pad+2]=4;
}
static void auth_write(void *opaque,unsigned reg,uint32_t v) {
    Device *d=opaque;WifiBoot *b=d->b;
    if(reg==0x460 && v==0x10001) {
        assert(d->master && b->auth_sent && b->auth_session==3);
        uint8_t *desc=b->management_tx,*cmd=(void *)(uintptr_t)u64(desc+4);
        assert((uintptr_t)cmd>=(uintptr_t)pool && (uintptr_t)cmd+4096<=(uintptr_t)pool+b->used);
        assert((u32(desc)&65535)==3 && u32(desc)>>16==20);
        assert((u32(desc+12)&65535)==36 && u64(desc+14)==(uintptr_t)cmd+20);
        assert((u32(desc+22)&65535)==6 && u64(desc+24)==(uintptr_t)cmd+64);
        assert(u32(cmd)==0x0100001c && u32(cmd+4)==0x0003001e);
        assert(u32(cmd+8)==0xc00 && u32(cmd+20)==(auth_fault==AUTH_RATE12?0x4102U:0x4100U));
        uint8_t *f=cmd+32;assert(f[0]==0xb0 && !f[1]);
        assert(!memcmp(f+4,b->target.bssid,6) && !memcmp(f+10,b->mac,6) && !memcmp(f+16,b->target.bssid,6));
        assert(u32(cmd+64)==0x10000 && !(u32(cmd+68)&65535));
        assert((u32(b->management_bc)&65535)==30);
        unsigned n=b->consumed;
        if(auth_fault==AUTH_AP_FIRST) {
            ap_auth(b->rx+n++*4096,b);tx_completion(b->rx+n++*4096);
        } else {
            if(auth_fault!=AUTH_NO_TX)tx_completion(b->rx+n++*4096);
            if(auth_fault!=AUTH_NO_AP)ap_auth(b->rx+n++*4096,b);
            if(auth_fault==AUTH_DUP_TX)tx_completion(b->rx+n++*4096);
        }
        *(uint16_t *)(void *)b->status=n;return;
    }
    if(reg==0x460 && b->auth_enabled) {
        uint8_t *cmd=b->command_data;assert(cmd[0]==5 && cmd[1]==3 && u32(cmd+4)==24);
        assert(u32(cmd+8)==0 && u32(cmd+12)==1 && u32(cmd+16)==0 && u32(cmd+20)==900);
        unsigned n=b->consumed;uint8_t ack[4096]={0},notice[4096]={0};
        p32(ack,4);if(auth_fault==AUTH_ACK_PAYLOAD){p32(ack,8);p32(ack+8,0xdeadbeef);}ack[4]=5;ack[5]=3;ack[6]=v-1;
        p32(notice,20);notice[4]=0xfb;notice[5]=3;notice[7]=0x80;p32(notice+12,1);p32(notice+16,1);
        if(auth_fault==AUTH_SESSION_FAIL)p32(notice+12,0);
        if(auth_fault==AUTH_SESSION_END)p32(notice+16,0);
        if(auth_fault==AUTH_SESSION_SHORT)p32(notice,19);
        if(auth_fault==AUTH_SESSION_ID)p32(notice+8,1);
        if(auth_fault==AUTH_CMD_FAILED)ack[5]|=0x40;
        if(auth_fault==AUTH_START_FIRST)memcpy(b->rx+n++*4096,notice,4096);
        if(auth_fault!=AUTH_NO_ACK)memcpy(b->rx+n++*4096,ack,4096);
        if(auth_fault!=AUTH_NO_START && auth_fault!=AUTH_START_FIRST)memcpy(b->rx+n++*4096,notice,4096);
        *(uint16_t *)(void *)b->status=n;return;
    }
    prepare_write(opaque,reg,v);
    if(reg==0x460 && b->prepare_started && b->prepare_step==6) {
        if(auth_fault==AUTH_DISABLED)b->mcc_channel_flags[17]&=~1U;
        if(auth_fault==AUTH_MISSING_BSS)b->target.details_valid=0;
    }
    if(reg==0x460 && v==3 && auth_fault==AUTH_NO_ANT)p32(b->rx+b->consumed*4096+20,2);
    if(reg==0x460 && v==12 && auth_fault==AUTH_RATE12) {
        unsigned n=*(uint16_t *)(void *)b->status;
        for(unsigned j=b->consumed;j<n;j++)if(b->rx[j*4096+4]==0xc1 && b->rx[j*4096+8+42]==48)
            b->rx[j*4096+8+64+36+7]&=127; /* 6Mbps supported, lowest basic is12Mbps */
    }
    if(reg==0x460 && (v==9 || (!b->prepare_started && v>12))) {
        uint8_t *p=b->rx+b->consumed*4096;
        if(auth_fault==AUTH_HP_FLAGS)b->nvm_channel_flags[17]=0xf61;
        for(unsigned n=0;n<18;n++)p32(p+28+4*n,auth_fault==AUTH_PASSIVE || auth_fault==AUTH_COUNTRY_PASSIVE || ((auth_fault==AUTH_COUNTRY || auth_fault==AUTH_COUNTRY_RACE) && v==9)?1:auth_fault==AUTH_RADAR?25:auth_fault==AUTH_HP_FLAGS?0xf61:9);
    }
}
#ifndef WIFI_AUTH_MAIN
#define WIFI_AUTH_MAIN main
#endif
int WIFI_AUTH_MAIN(int argc,char **argv) {
    assert(!wifi_prepare_tests(argc,argv));
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);assert(!wifi_image_parse(blob,size,&image));
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
    uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=1;
    image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);assert(!wifi_auth_validate(&image));
    scan_fault=SCAN_OK;fault_kind=0;beacon_group=2;
    for(unsigned n=0;n<5;n++) {
        WifiImage bad=image;
        if(n==0)bad.tx_version=9;if(n==1)bad.tx_notification=6;
        if(n==2)bad.session_version=1;if(n==3)bad.session_notification=2;
        if(n==4)bad.prepare_ssid_size=0;
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};
        WifiOps ops={&d,scan_read,auth_write,write_byte,delay,mastering,prepare_log};
        assert(wifi_firmware_start(&b,&bad,&ops,0x441) && !d.writes && !b.attempted);
    }
    for(auth_fault=AUTH_OK;auth_fault<AUTH_LAST;auth_fault++) {
        expected_basic_ofdm=auth_fault==AUTH_RATE12?0x14:0x15;
        scan_fault=auth_fault==AUTH_COUNTRY_RACE?SCAN_REG_RACE:auth_fault==AUTH_COUNTRY || auth_fault==AUTH_COUNTRY_PASSIVE?SCAN_REG_CHANGE:SCAN_OK;
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
        WifiOps ops={&d,scan_read,auth_write,write_byte,delay,mastering,prepare_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        int success=auth_fault==AUTH_OK || auth_fault==AUTH_AP_FIRST || auth_fault==AUTH_PAD || auth_fault==AUTH_START_FIRST || auth_fault==AUTH_RATE12 || auth_fault==AUTH_COUNTRY || auth_fault==AUTH_COUNTRY_RACE || auth_fault==AUTH_PASSIVE || auth_fault==AUTH_COUNTRY_PASSIVE ||
            auth_fault==AUTH_HP_FLAGS || auth_fault==AUTH_TX_QID || auth_fault==AUTH_TX_HDRINDEX ||
            auth_fault==AUTH_SSN_HIGH || auth_fault==AUTH_TX_TAIL || auth_fault==AUTH_ACK_PAYLOAD;
        if((error==0)!=success)fprintf(stderr,"auth %u: %s\n",auth_fault,error?error:"unexpected success");
        assert((error==0)==success && b.quiesced && !d.master);
        if(auth_fault==AUTH_COUNTRY || auth_fault==AUTH_COUNTRY_RACE)assert(b.reg_applied_country==0x414c &&
            b.reg_applied_source==5 && b.reg_generation==b.reg_applied_generation &&
            b.reg_refreshes==(auth_fault==AUTH_COUNTRY_RACE?2U:1U));
        if(auth_fault==AUTH_COUNTRY_PASSIVE)assert(b.auth_tx_done && b.mcc_channel_flags[17]==1);
        if(auth_fault==AUTH_DISABLED || auth_fault==AUTH_MISSING_BSS)assert(!b.auth_enabled && !b.auth_sent && !b.auth_tx_done);
        if(success)assert(b.auth_tx_done && b.auth_received && b.auth_sent && b.management_flags==1);
    }
    free(blob);free(pnvm);printf("AX211_AUTH_HOST_OK %u TX/session/AP/permission scenarios and ABI guards; RF untested\n",AUTH_LAST);
    return 0;
}
