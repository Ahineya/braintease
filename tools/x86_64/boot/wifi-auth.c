/* One bounded Open System 802.11 authentication exchange with the selected
 * WPA2 AP. This is NOT WPA authentication: no PSK, EAPOL or keys. Association is a separate explicit profile.
 * AX210 Gen3 TX/TFH and session ABI: pinned OpenBSD iwx and Linux v6.12
 * fw/api/{tx,time-event}.h; see wifi-LICENSE.txt. Native DMA only. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_auth_validate(const WifiImage *fw) {
    if(!fw->auth_probe)return 0;
    if(!fw->prepare_ssid_size || !fw->scan_probe || fw->tx_version!=10 || fw->tx_notification!=7 ||
       fw->session_version!=2 || fw->session_notification!=3 || !(fw->capabilities[1]&(1U<<22)))
        return "unsupported authentication TX/session ABI/profile";
    return 0;
}
const char *wifi_auth_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    if(!fw->auth_probe || !b->prepare_complete || b->command_pending || b->auth_sent)return 0;
    if(!b->auth_enabled) {
        unsigned ch=b->target.channel;
        if(ch<36 || ch>48 || ch%4)return "authentication channel not supported";
        unsigned index=14+(ch-36)/4;uint32_t flags=b->mcc_channel_flags[index];
        o->log(o->opaque,"AX211_AUTH_CHANNEL_FLAGS",flags);
        /* NVM_CHANNEL_ACTIVE is ACTIVE SCANNING permission, not a blanket
         * ban on client auth to an already discovered BSS. Linux mgd_auth ->
         * prep_connection/prep_channel checks CHAN_DISABLED, not NO_IR;
         * OpenBSD iwx_auth likewise does not require ACTIVE/PASSIVE clear.
         * We never probe/beacon; this scope still excludes DFS/radar channels. */
        if(!b->prepare_started || !b->scan_complete || !b->scan_acked || !b->target.details_valid)
            return "authentication requires discovered BSS";
        if(!(flags&1) || (flags&16) || !(b->nvm_channel_flags[index]&1) ||
           b->reg_generation!=b->reg_applied_generation)return "authentication channel TX not permitted";
        if(!(b->nvm_tx_chains&1))return "authentication requires TX antenna A";
        if(b->management_qid!=1)return "authentication management queue missing";
        b->auth_enabled=1;o->log(o->opaque,"AX211_QUEUE_FLAGS_RAW",b->management_flags);
        uint8_t data[24]={0};p32(data+4,1);p32(data+12,900);
        const char *error=wifi_command(b,o,3,5,0,data,sizeof(data));
        if(!error)b->auth_phase=1;
        return error;
    }
    /* Require both the command ACK and successful session-start notification. */
    if(b->auth_phase!=1 || b->auth_session!=3)return 0;
    unsigned rate=0;
    while(rate<8 && !(b->target.basic_rates&(1U<<(rate+4))))++rate;
    if(rate==8)return "authentication has no supported basic OFDM rate";
    uint8_t *cmd=wifi_dma_reserve(b,4096);
    if(!cmd)return "authentication TX DMA allocation";
    cmd[0]=0x1c;cmd[3]=1; /* ordinary four-byte command header: queue1/index0 */
    uint8_t *tx=cmd+4,*frame=tx+28;
    p16(tx,30);p16(tx+2,3); /* command rate + encryption disabled for auth */
    p32(tx+4,12U<<8);p32(tx+16,(1U<<14)|(1U<<8)|rate); /* antennaA, lowest basic OFDM v2 */
    frame[0]=0xb0;
    for(unsigned n=0;n<6;n++){frame[4+n]=frame[16+n]=b->target.bssid[n];frame[10+n]=b->mac[n];}
    p16(cmd+64,0);p16(cmd+66,1);p16(cmd+68,0); /* algorithm0, request1, status0 */
    uint8_t *desc=b->management_tx;
    p16(desc,3);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);
    p16(desc+12,36);p64(desc+14,(uintptr_t)cmd+20);
    p16(desc+22,6);p64(desc+24,(uintptr_t)cmd+64);
    p16(b->management_bc,30); /* Gen3 byte count, one64-byte descriptor fetch */
    b->auth_sent=1;b->auth_phase=2;++b->nvm_progress;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    o->write(o->opaque,0x460,(1U<<16)|1); /* one descriptor; no host retry/reuse */
    o->log(o->opaque,"AX211_AUTH_FRAME_SUBMITTED",1);
    return 0;
}
const char *wifi_auth_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    /* No WANT_RESP: completion/header failure is checked by the transport;
     * Linux/OpenBSD do not interpret an unrequested response body. */
    (void)p;(void)bytes;
    if(b->auth_phase!=1 || b->command_group!=3 || b->command_code!=5 || (b->auth_session&1))
        return "authentication session command reply";
    b->auth_session|=1;return 0;
}
const char *wifi_auth_notification(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(bytes<4)return "authentication notification header";
    if(p[0]!=0xfb || p[1]!=3 || !b->auth_enabled)return 0;
    if(bytes!=20 || u32(p+4) || u32(p+16) || u32(p+8)!=1 || u32(p+12)>1)
        return "authentication session notification status/bounds";
    /* This protects association, not the entire four-way handshake. OpenBSD
     * simply clears TE_ACTIVE on successful end; Linux checks association /
     * beacon state, not installed WPA keys. A completed association keeps its
     * configured channel after this event, including during an EAPOL retry. */
    if(!u32(p+12))return b->auth_received && b->auth_tx_done &&
        (!b->assoc_enabled || b->assoc_complete)?0:"authentication protected session ended";
    if(!(b->auth_session&2)){b->auth_session|=2;++b->nvm_progress;}
    return 0;
}
const char *wifi_auth_tx_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->auth_sent || b->auth_tx_done || bytes<52)
        return "authentication TX completion size/state";
    const uint8_t *d=p+4;
    /* Linux iwl_mvm_rx_tx_cmd_single uses payload tx_queue for this new
     * API, not the legacy RX header sequence. AX210+ SSN is16 bits (older
     * devices use12); unused high bits and trailing bytes are not errors. */
    if(d[0]!=1 || u16(d+36)!=1 || (u32(d+44)&65535)!=1)return "authentication TX completion count/queue/SSN";
    b->auth_tx_status=u16(d+40)&255;
    if(b->auth_tx_status!=1 && b->auth_tx_status!=2)return "authentication frame TX failed";
    b->auth_tx_done=1;++b->nvm_progress;return 0;
}
const char *wifi_auth_mpdu(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->auth_sent)return 0;
    if(bytes<64 || u16(p)>bytes-64)return "authentication RX descriptor bounds";
    unsigned len=u16(p),pad=(p[3]&0x20)?2:0;const uint8_t *f=p+64;
    if((u32(p+12)&3)!=3 || p[42]!=b->target.channel || len<30+pad || f[0]!=0xb0 ||
       (f[1]&0x47) || (u16(f+22)&15))return 0;
    for(unsigned n=0;n<6;n++)if(f[4+n]!=b->mac[n] || f[10+n]!=b->target.bssid[n] ||
        f[16+n]!=b->target.bssid[n])return 0;
    const uint8_t *body=f+24+pad;
    if(u16(body) || u16(body+2)!=2)return "authentication algorithm/transaction mismatch";
    b->auth_status=u16(body+4);
    if(b->auth_status)return "AP rejected 802.11 authentication";
    if(!b->auth_received){b->auth_received=1;++b->nvm_progress;}
    return 0;
}
