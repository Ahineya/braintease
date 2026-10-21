/* Bounded legacy 5GHz association, following OpenBSD net80211 output1.148 /
 * input1.263 and iwx1.230 MLD run path. See wifi-LICENSE.txt. No keys/data TX.
 * One fresh management descriptor after authentication, then LINK/STA/MAC.
 * This deliberately advertises no WMM/HT/PMF and leaves the port unauthorized. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_assoc_validate(const WifiImage *fw) {
    return fw->assoc_probe && !fw->auth_probe?"association requires authentication profile":0;
}
static const char *submit(WifiBoot *b,const WifiOps *o) {
    uint8_t *cmd=wifi_dma_reserve(b,4096);
    if(!cmd)return "association TX DMA allocation";
    cmd[0]=0x1c;cmd[2]=1;cmd[3]=1; /* queue1/index1, never reuse auth index0 */
    uint8_t *tx=cmd+4,*f=cmd+32,*body=cmd+64;
    for(unsigned n=0;n<6;n++){f[4+n]=f[16+n]=b->target.bssid[n];f[10+n]=b->mac[n];}
    p16(f+22,16); /* sequence1; ordinary unprotected ASSOC_REQ subtype0 */
    p16(body,0x411);p16(body+2,1); /* ESS/privacy/short slot; no power saving */
    unsigned at=4;body[at++]=0;body[at++]=b->target.ssid_size;
    for(unsigned n=0;n<b->target.ssid_size;n++)body[at++]=b->target.ssid[n];
    body[at++]=1;unsigned count_at=at++; /* Supported Rates, up to8 OFDM rates */
    static const uint8_t rates[8]={12,18,24,36,48,72,96,108};
    for(unsigned n=0;n<8;n++)if(b->target.rates&(1U<<(n+4))) {
        body[at++]=rates[n]|((b->target.basic_rates&(1U<<(n+4)))?128:0);++body[count_at];
    }
    /* Select exactly CCMP pairwise/PSK; retain AP's actual TKIP/CCMP group.
     * RSN caps0: no PMF, one replay counter, no preauthentication/PMKID. */
    at+=wifi_rsn_request(&b->target,body+at);
    unsigned len=24+at,rate=0;
    while(rate<8 && !(b->target.basic_rates&(1U<<(rate+4))))++rate;
    if(rate==8)return "association missing basic OFDM rate";
    p16(tx,len);p16(tx+2,3);p32(tx+4,12U<<8);p32(tx+16,(1U<<14)|(1U<<8)|rate);
    uint8_t *desc=b->management_tx+256;
    p16(desc,3);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);
    p16(desc+12,36);p64(desc+14,(uintptr_t)cmd+20);
    p16(desc+22,at);p64(desc+24,(uintptr_t)body);
    p16(b->management_bc+2,len);
    b->assoc_sent=1;++b->nvm_progress;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    o->write(o->opaque,0x460,(1U<<16)|2);
    o->log(o->opaque,"AX211_ASSOC_FRAME_SUBMITTED",len);
    return 0;
}
const char *wifi_assoc_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    if(!fw->assoc_probe || !b->auth_tx_done || !b->auth_received || b->command_pending || b->assoc_complete)return 0;
    if(!b->assoc_sent)return submit(b,o);
    if(!b->assoc_tx_done || !b->assoc_received)return 0;
    uint8_t data[208]={0};unsigned code,bytes;
    switch(b->assoc_step) {
    case 0: /* iwx_mld_add_sta_cmd(update1), legacy LINK0 MODIFY. */
        code=9;bytes=208;p32(data,2);
        for(unsigned n=0;n<6;n++)data[16+n]=b->mac[n];
        /* No QoS was negotiated: leave EDCA unchanged (omit MODIFY_QOS).
         * Protection0, no HT; beacon timing is updated only after AP reply. */
        p32(data+24,0x17);p32(data+28,1);
        unsigned ack=b->assoc_basic>>4,lowest=0;
        while(lowest<8 && !(ack&(1U<<lowest)))++lowest;
        /* iwx_ack_rates: mandatory OFDM fallback below the lowest basic. */
        ack|=1;if(lowest>2)ack|=4;if(lowest>4)ack|=16;
        p32(data+40,ack);
        p32(data+44,1);p32(data+48,1); /* net80211 11A short preamble/slot */
        p32(data+136,b->target.beacon_interval);p32(data+140,b->target.beacon_interval*b->target.dtim_period);break;
    case 1: /* STA_CONFIG v1: same station0/link0, now with AP-assigned AID. */
        code=10;bytes=96;p32(data+28,b->assoc_aid);
        for(unsigned n=0;n<6;n++)data[8+n]=data[16+n]=b->target.bssid[n];break;
    case 2: /* iwx_mld_mac_ctxt_cmd(MODIFY,assoc1), client v2. */
        code=8;bytes=52;p32(data+4,2);p32(data+8,5);
        for(unsigned n=0;n<6;n++)data[12+n]=b->mac[n];
        p32(data+20,b->target.dtim_period?4:12);data[36]=1;p16(data+40,b->assoc_aid);break;
    default:return "association update state";
    }
    const char *error=wifi_command(b,o,3,code,0,data,bytes);
    if(!error)++b->assoc_step;
    return error;
}
const char *wifi_assoc_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    (void)p;(void)bytes; /* no WANT_RESP: transport checks ACK tags/failure */
    static const unsigned codes[3]={9,10,8};
    if(!b->assoc_step || b->assoc_step>3 || b->assoc_complete || !b->assoc_received ||
       b->command_group!=3 || b->command_code!=codes[b->assoc_step-1])return "association update reply state";
    if(b->assoc_step==3)b->assoc_complete=1;
    return 0;
}
const char *wifi_assoc_tx_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->assoc_sent || b->assoc_tx_done || bytes<52)return "association TX completion size/state";
    const uint8_t *d=p+4;
    if(d[0]!=1 || u16(d+36)!=1 || (u32(d+44)&65535)!=2)return "association TX completion count/queue/SSN";
    b->assoc_tx_status=u16(d+40)&255;
    if(b->assoc_tx_status!=1 && b->assoc_tx_status!=2)return "association frame TX failed";
    b->assoc_tx_done=1;++b->nvm_progress;return 0;
}
const char *wifi_assoc_mpdu(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->assoc_sent)return 0;
    if(bytes<64 || u16(p)>bytes-64)return "association RX descriptor bounds";
    unsigned len=u16(p),pad=(p[3]&0x20)?2:0;const uint8_t *f=p+64;
    if((u32(p+12)&3)!=3 || p[42]!=b->target.channel || len<24+pad ||
       (f[0]!=0x10 && f[0]!=0xa0 && f[0]!=0xc0) || (f[1]&0x47) || (u16(f+22)&15))return 0;
    for(unsigned n=0;n<6;n++)if(f[4+n]!=b->mac[n] || f[10+n]!=b->target.bssid[n] ||
        f[16+n]!=b->target.bssid[n])return 0;
    if(f[0]!=0x10)return "AP disconnected during association";
    if(len<30+pad)return "association fixed fields truncated";
    const uint8_t *body=f+24+pad;
    b->assoc_status=u16(body+2);
    if(b->assoc_status)return "AP rejected association";
    unsigned aid=u16(body+4)&0x7ff; /* Linux recv_assoc_resp masks all five reserved MSBs. */
    if(!aid || aid>2007)return "association AID outside legacy range";
    WifiAp rates={0};const char *error=wifi_ap_details(&rates,body+6,len-30-pad);
    if(error)return error;
    if(rates.unknown_basic || (rates.basic_rates&~b->target.rates) ||
       (rates.basic_rates&15) || !(rates.rates&b->target.rates&0xff0) || !(rates.basic_rates&0xff0))
        return "association unsupported basic rates";
    if(b->assoc_received)return 0; /* matching retries cannot restart updates */
    b->assoc_aid=aid;b->assoc_caps=u16(body);b->assoc_rates=rates.rates&b->target.rates;
    b->assoc_basic=rates.basic_rates;b->assoc_received=1;++b->nvm_progress;
    return 0;
}
