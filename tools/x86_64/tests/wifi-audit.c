/* Consumer-derived compatibility cases. References/functions and intentional
 * bootstrap limits are recorded in hardware/hp-wifi-checks.txt. No RF claim. */
#include "../boot/wifi.h"
#include <stdio.h>
#include <string.h>
static uint8_t pool[131072] __attribute__((aligned(4096)));
static unsigned failures,writes;
static void check(int okay,const char *name) {
    printf("%s %s\n",okay?"PASS":"FAIL",name);failures+=!okay;
}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(n*8);}
static void write_reg(void *opaque,unsigned reg,uint32_t value) {
    (void)opaque;(void)reg;(void)value;++writes;
}
static void log_value(void *opaque,const char *name,uint64_t value) {
    (void)opaque;(void)name;(void)value;
}
int main(void) {
    uint8_t queue[8]={1,0,1,0,0,0,0xef,0xbe};
    WifiBoot b={.prepare_started=1,.prepare_step=7};
    check(!wifi_prepare_reply(&b,queue,sizeof(queue)),"queue RX padding ignored, ID/write pointer still checked");
    uint8_t nvm[468]={0};nvm[4]=1;nvm[7]=2;
    p32(nvm+8,3);p32(nvm+12,3);p32(nvm+16,3);p32(nvm+20,1);p32(nvm+24,110);
    for(unsigned n=0;n<110;n++)p32(nvm+28+4*n,1);
    p32(nvm,1);nvm[7]=0;nvm[4]=0;p32(nvm+20,3);b=(WifiBoot){0};
    check(!wifi_nvm_parse(&b,nvm,sizeof(nvm),4),"NVM empty OTP/address count/version informational; LAR is nonzero boolean");
    nvm[4]=nvm[5]=255;b=(WifiBoot){0};
    check(!wifi_nvm_parse(&b,nvm,sizeof(nvm),4),"NVM version retained without undocumented sentinel rejection");
    memset(pool,0,sizeof(pool));writes=0;
    b=(WifiBoot){.pool=pool,.used=65536,.capacity=sizeof(pool),.tx=pool,
        .prepare_started=1,.prepare_complete=1,.management_qid=1,.nvm_tx_chains=3,
        .scan_complete=1,.scan_acked=1,.reg_generation=1,.reg_applied_generation=1,
        .target={.channel=48,.details_valid=1,.ssid_size=3,.ssid={'a','b','c'}}};
    b.mcc_channel_flags[17]=b.nvm_channel_flags[17]=0xf61;
    WifiImage fw={.auth_probe=1};WifiOps ops={.write=write_reg,.log=log_value};
    check(!wifi_auth_advance(&b,&fw,&ops) && writes==1 && b.auth_phase==1,
        "photographed0xf61 permits client auth to discovered BSS; ACTIVE is scan permission");
    b=(WifiBoot){.auth_phase=1,.command_group=3,.command_code=5};uint8_t ack[4]={0x12,0x34,0x56,0x78};
    check(!wifi_auth_reply(&b,ack,sizeof(ack)),"session command completion ignores unrequested ACK payload");
    b=(WifiBoot){.prepare_started=1,.prepare_step=2};
    check(!wifi_prepare_reply(&b,ack,sizeof(ack)),"context command completion ignores unrequested ACK payload");
    uint8_t tx[60]={0x1c,0,99,0x82};uint8_t *d=tx+4;
    d[0]=1;d[36]=1;d[40]=1;p32(d+44,0xa5a50001);b=(WifiBoot){.auth_sent=1};
    check(!wifi_auth_tx_reply(&b,tx,sizeof(tx)) && b.auth_tx_done,
        "new TX API uses payload queue and AX21016-bit SSN, not legacy header or trailing data");
    p32(d+44,0x1001);b=(WifiBoot){.auth_sent=1};
    check(wifi_auth_tx_reply(&b,tx,sizeof(tx))!=0,"AX210 SSN bits12..15 are significant");
    uint8_t mcc[20+18*4]={4,0,0,0,'L','A',0,0,0,0,0,0,5,0,0,0,18,0,0,0};
    b=(WifiBoot){.command_code=0xc8,.nvm_channels=18,.nvm_mac_flags=3};
    for(unsigned n=0;n<18;n++){p32(mcc+20+4*n,1);b.nvm_channel_flags[n]=1;}
    check(!wifi_scan_reply(&b,mcc,sizeof(mcc)),"MCC ILLEGAL status uses returned actual profile as Linux does");
    uint8_t chub[8]={0xc9,0,0,0x80,'L','A',5,0xa5};b=(WifiBoot){0};
    check(!wifi_scan_notification(&b,chub,sizeof(chub)),"CHUB receive padding ignored; country/source remain parsed");
    printf("AX211_AUDIT_HOST_%s failures=%u\n",failures?"FAILED":"OK",failures);
    return failures?1:0;
}
