/* One-shot EAPOL data exchange over the accepted native non-QoS queue.
 * iwx_tx uses first_data_qid for non-QoS frames; EAPOL is deliberately non-QoS.
 * Fixed basic command-rate/high-priority TX needs no adaptive rate controller.
 * Key command80 ABI matches Linux mld-key.c; never use C16 pointers for DMA. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_wpa_validate(const WifiImage *fw) {
    return fw->wpa_probe && (!fw->assoc_probe || fw->key_version!=1)?"WPA2 requires association and SEC_KEY v1":0;
}
const char *wifi_wpa_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    WifiWpa *w=&b->wpa;
    if(!fw->wpa_probe || !b->assoc_complete || b->command_pending || w->tx_wait || w->complete)return 0;
    if(w->pending) {
        if(w->tx_next>=10 || w->pending_bytes>128 || (w->pending!=2 && w->pending!=4))return "WPA2 TX ownership/retry bound";
        uint8_t *cmd=wifi_dma_reserve(b,4096);if(!cmd)return "WPA2 TX DMA allocation";
        unsigned index=w->tx_next,kind=w->pending,rate=0,len=32+w->pending_bytes;
        while(rate<8 && !(b->target.basic_rates&(1U<<(rate+4))))++rate;
        if(rate==8)return "WPA2 missing basic OFDM rate";
        cmd[0]=0x1c;cmd[2]=index;cmd[3]=1;
        uint8_t *tx=cmd+4,*f=cmd+32,*body=cmd+64;
        p16(tx,len);p16(tx+2,7);p32(tx+4,12U<<8);p32(tx+16,(1U<<14)|(1U<<8)|rate);
        f[0]=8;f[1]=1;p16(f+22,index<<4); /* ordinary DATA, ToDS, no encryption */
        for(unsigned n=0;n<6;n++){f[4+n]=f[16+n]=b->target.bssid[n];f[10+n]=b->mac[n];}
        static const uint8_t llc[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
        for(unsigned n=0;n<8;n++)body[n]=llc[n];
        for(unsigned n=0;n<w->pending_bytes;n++)body[8+n]=w->pending_data[n];
        uint8_t *desc=b->management_tx+index*256;
        p16(desc,3);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);
        p16(desc+12,36);p64(desc+14,(uintptr_t)cmd+20);
        p16(desc+22,8+w->pending_bytes);p64(desc+24,(uintptr_t)body);
        p16(b->management_bc+index*2,len);
        ++w->tx_next;w->tx_wait=w->tx_next;w->tx_kind=kind;w->pending=0;
        if(kind==2)++w->m2_count;else ++w->m4_count;
        ++b->nvm_progress;__atomic_thread_fence(__ATOMIC_SEQ_CST);
        o->write(o->opaque,0x460,(1U<<16)|w->tx_next);
        o->log(o->opaque,kind==2?"AX211_WPA2_M2_SUBMITTED":"AX211_WPA2_M4_SUBMITTED",w->tx_wait);
        return 0;
    }
    /* OpenBSD sends M4 before installing PTK/GTK. Wait for actual TX success
     * too. Retransmitted M3 only resends M4; key commands are never repeated. */
    if(!w->m3_seen || !w->m4_done)return 0;
    if(w->key_installed==2){w->complete=1;return 0;}
    uint8_t key[80]={0};unsigned group=w->key_installed==1;
    if(w->key_step!=w->key_installed || w->key_step>=2)return "WPA2 key command ownership";
    p32(key,1);p32(key+4,1);p32(key+8,group?w->gtk_id:0);
    p32(key+12,group?(b->target.group_cipher==4?0x43:0x42):2);
    if(!group)for(unsigned n=0;n<16;n++)key[16+n]=w->ptk[32+n];
    else if(w->gtk_bytes==16)for(unsigned n=0;n<16;n++)key[16+n]=w->gtk[n];
    else {
        /* wpa_supplicant swaps TKIP MIC subkeys for STA before nl80211.
         * Linux mld_send_key then copies RX/TX subkeys to explicit fields. */
        for(unsigned n=0;n<16;n++)key[16+n]=w->gtk[n];
        for(unsigned n=0;n<8;n++) {
            key[32+n]=w->gtk[24+n];key[40+n]=w->gtk[16+n];
            key[48+n]=w->gtk[16+n];key[56+n]=w->gtk[24+n];
        }
    }
    /* Like both upstream consumers, leave firmware rx_seq/tx_seq0. Retain
     * authenticated GTK RSC in host state for the future RX replay filter. */
    const char *error=wifi_command(b,o,5,24,0,key,sizeof(key));wifi_wipe(key,sizeof(key));
    if(!error)++w->key_step;return error;
}
const char *wifi_wpa_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    (void)p;(void)bytes;WifiWpa *w=&b->wpa;
    if(b->command_group!=5 || b->command_code!=24 || w->key_step!=w->key_installed+1 ||
       w->key_step>2 || !w->m4_done)return "WPA2 key command reply state";
    ++w->key_installed;return 0;
}
const char *wifi_wpa_tx_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    WifiWpa *w=&b->wpa;
    if(!w->tx_wait || bytes<52)return "WPA2 TX completion size/state";
    const uint8_t *d=p+4;
    if(d[0]!=1 || u16(d+36)!=1 || (u32(d+44)&65535)!=w->tx_wait)return "WPA2 TX completion count/queue/SSN";
    w->tx_status=u16(d+40)&255;
    if(w->tx_status!=1 && w->tx_status!=2)return "WPA2 EAPOL TX failed";
    if(w->tx_kind==4)w->m4_done=1;
    w->tx_wait=0;++b->nvm_progress;return 0;
}
const char *wifi_wpa_mpdu(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    WifiWpa *w=&b->wpa;
    if(!w->enabled || !b->assoc_received || w->complete)return 0;
    if(bytes<64 || u16(p)>bytes-64)return "WPA2 RX descriptor bounds";
    unsigned len=u16(p),pad=(p[3]&0x20)?2:0;const uint8_t *f=p+64;
    if((u32(p+12)&3)!=3 || p[42]!=b->target.channel || len<24 ||
       (f[0]!=8 && f[0]!=0x88) || (f[1]&0x47)!=2 || (u16(f+22)&15))return 0;
    unsigned header=f[0]==0x88?26:24;
    if(len<header+pad+8 || (header==26 && (f[24]&128)))return 0; /* no A-MSDU */
    for(unsigned n=0;n<6;n++)if(f[4+n]!=b->mac[n] || f[10+n]!=b->target.bssid[n])return 0;
    /* addr3 is the Ethernet source, not necessarily the radio's BSSID. Both
     * upstream net80211 paths bind the sender by transmitter/BSS instead. */
    const uint8_t *llc=f+header+pad;static const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
    if(!wifi_equal(llc,snap,8))return 0;
    unsigned before=w->m1_seen+w->m3_seen+w->duplicates;
    const char *error=wifi_wpa_input(w,b->mac,&b->target,llc+8,len-header-pad-8);
    if(!error && before!=w->m1_seen+w->m3_seen+w->duplicates) {
        ++b->nvm_progress;
    }
    return error;
}
