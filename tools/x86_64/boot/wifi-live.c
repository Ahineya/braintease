/* Polled single-CPU AX211 Ethernet NIC. No ISR calls guest code. AX210 fixed-
 * order RX refill and TX cumulative-SSN reclaim follow pinned iwx/iwlwifi.
 * All DMA pages remain resident until confirmed reset/mastering disabled. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
static void copy(uint8_t *a,const uint8_t *b,unsigned n){for(unsigned i=0;i<n;i++)a[i]=b[i];}
static void increment(WifiLive *l,unsigned i){if(l->info.counters[i]!=UINT32_MAX)++l->info.counters[i];}
static const char *broken(WifiBoot *b,const WifiOps *o,const char *why,uint64_t now) {
    WifiLiveDiag *d=&b->live.diag;wifi_live_diag_fail(d,now);
    b->live.active=b->live.info.link=0;increment(&b->live,8);
    o->log(o->opaque,"AX211_LINK_FAILED",1);
    o->log(o->opaque,"AX211_TX_HEAD",b->live.tx_head);o->log(o->opaque,"AX211_TX_TAIL",b->live.tx_tail);
    o->log(o->opaque,"AX211_TX_SSN",b->live.last_ssn);o->log(o->opaque,"AX211_TX_QUEUED",b->live.tx_count);
    /* Failure-only raw telemetry goes through the existing serial callback.
     * Healthy missed-beacon warnings only update bounded metadata below; no
     * unsolicited framebuffer text, periodic dump or firmware probe is added. */
    o->log(o->opaque,"AX211_LIVE_DIAG_SEEN",d->seen);
    o->log(o->opaque,"AX211_LIVE_CHANNEL",d->channel);
    o->log(o->opaque,"AX211_LIVE_POLLS",d->polls);
    o->log(o->opaque,"AX211_LIVE_FIRST_POLL_US",d->first_poll_us);
    o->log(o->opaque,"AX211_LIVE_FAILURE_US",d->failed_at_us);
    o->log(o->opaque,"AX211_LIVE_POLL_US",d->last_poll_us);
    o->log(o->opaque,"AX211_LIVE_POLL_GAP_US",d->poll_gap_us);
    o->log(o->opaque,"AX211_LIVE_MAX_POLL_GAP_US",d->max_poll_gap_us);
    o->log(o->opaque,"AX211_LIVE_AP_RX_US",d->last_ap_rx_us);
    o->log(o->opaque,"AX211_LIVE_DATA_RX_US",d->last_data_rx_us);
    o->log(o->opaque,"AX211_LIVE_AP_FRAMES",d->ap_frames);
    o->log(o->opaque,"AX211_LIVE_DATA_FRAMES",d->data_frames);
    o->log(o->opaque,"AX211_LIVE_AP_AGE_US",wifi_live_diag_ap_age(d));
    o->log(o->opaque,"AX211_LIVE_DATA_AGE_US",wifi_live_diag_data_age(d));
    o->log(o->opaque,"AX211_LIVE_CLOCK_BACKWARDS",d->clock_backwards);
    o->log(o->opaque,"AX211_BEACON_NOTIFICATIONS",d->missed_notifications);
    o->log(o->opaque,"AX211_BEACON_AT_US",d->missed_at_us);
    o->log(o->opaque,"AX211_BEACON_MISSED_SINCE_RX",d->missed_since_rx);
    o->log(o->opaque,"AX211_BEACON_MISSED_CONSECUTIVE",d->missed_consecutive);
    o->log(o->opaque,"AX211_BEACON_EXPECTED",d->missed_expected);
    o->log(o->opaque,"AX211_BEACON_RECEIVED",d->missed_received);
    wifi_firmware_stop(b,o);return why;
}
const char *wifi_live_start(WifiBoot *b) {
    if(!b->wpa.complete || b->wpa.key_installed!=2 || b->wpa.tx_wait || b->command_pending || b->live.active)
        return "live NIC startup state";
    WifiLive *l=&b->live;l->tx_pages=wifi_dma_reserve(b,256*4096);
    if(!l->tx_pages)return "live NIC TX DMA allocation";
    l->tx_head=l->tx_tail=b->wpa.tx_next&65535;
    l->sequence=b->wpa.tx_next&4095;
    for(unsigned i=0;i<16;i++)b->data.group_pn[i]=b->wpa.gtk_rsc;
    const char *error=wifi_rekey_start(b);if(error)return error;
    wifi_live_diag_init(&l->diag,b->target.channel);
    copy(l->info.mac,b->mac,6);l->active=l->info.link=1;return 0;
}
static const char *completion(WifiBoot *b,const WifiOps *o,const uint8_t *p,unsigned n) {
    WifiLive *l=&b->live;
    if(n<48 || p[0]!=1 || u16(p+36)!=1)return "live TX completion size/count/queue";
    /* AX210 has a 65536-entry hardware counter, independently of our 256
     * resident DMA slots: iwx cur_hw/tail_hw and Linux gen2 write/read_ptr.
     * Keep the full low16 SSN, masking only when indexing native arrays. */
    unsigned end=u32(p+44)&65535,done=(end-l->tx_tail)&65535;l->last_ssn=end;
    if(done>l->tx_count) {
        /* Linux iwl_pcie_reclaim rejects an out-of-window last_to_free without
         * reclaiming or stopping the NIC. Preserve all owners/timestamps; a
         * truly stuck queue still hits the original oldest-TX watchdog. */
        increment(l,8); /* existing mailbox backend_errors, never rx_oversized */
        if(l->ignored_ssn!=UINT32_MAX)++l->ignored_ssn;
        if(l->ignored_ssn<=4) {
            o->log(o->opaque,"AX211_TX_SSN_IGNORED",end);
            o->log(o->opaque,"AX211_TX_HOST_WINDOW",(uint64_t)l->tx_head<<32|(uint64_t)l->tx_tail<<16|l->tx_count);
        }
        return 0;
    }
    unsigned at=l->tx_tail&255;
    for(unsigned i=0;i<done;i++,at=(at+1)&255)if(!l->tx_owned[at])return "live TX descriptor ownership";
    unsigned status=u16(p+40)&255;
    if(status!=1 && status!=2)increment(l,5); /* normal radio retry failure */
    for(unsigned i=0;i<done;i++) {
        unsigned slot=l->tx_tail&255;l->tx_tail=(l->tx_tail+1)&65535;
        l->tx_octets-=l->tx_size[slot];l->tx_owned[slot]=0;--l->tx_count;increment(l,4);
        if(b->rekey.stage==4 && l->tx_tail==b->rekey.tx_wait) {
            const char *error=wifi_rekey_tx_done(b,o,l->tx_tail,status);if(error)return error;
        }
    }
    return 0; /* repeated cumulative SSN reclaims nothing, as upstream */
}
static const uint8_t *eapol_clear(const WifiBoot *b,const uint8_t *p,unsigned n,unsigned *bytes) {
    if(n<64 || u16(p)>n-64 || u16(p)<32)return 0;
    const uint8_t *f=p+64;unsigned h=f[0]==0x88?26:24,pad=(p[3]&0x20)?2:0;
    if((f[0]!=8 && f[0]!=0x88) || (f[1]&0xc7)!=2 || (u32(p+12)&3)!=3 ||
       p[42]!=b->target.channel || (u16(f+22)&15) || !wifi_equal(f+4,b->mac,6) || !wifi_equal(f+10,b->target.bssid,6) || u16(p)<h+pad+8)return 0;
    static const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
    if(!wifi_equal(f+h+pad,snap,8))return 0;
    *bytes=u16(p)-h-pad-8;return f+h+pad+8;
}
/* Observed RF liveness, not authenticated data delivery: count CRC/overrun-
 * valid management or FromDS data frames from this frozen AP/channel only.
 * A frame can still fail MIC/replay/payload validation afterwards. No policy
 * decision (including the firmware's since-RX threshold) uses this clock. */
static int selected_ap_frame(const WifiBoot *b,const uint8_t *p,unsigned n) {
    if(n<64 || u16(p)>n-64 || u16(p)<24 || (u32(p+12)&3)!=3 || p[42]!=b->target.channel)return 0;
    const uint8_t *f=p+64;unsigned type=f[0]&0x0c;
    if((f[0]&3) || !wifi_equal(f+10,b->target.bssid,6))return 0;
    if(!type)return !(f[1]&3) && wifi_equal(f+16,b->target.bssid,6);
    return type==8 && (f[1]&3)==2 && (!(f[0]&0x80) || u16(p)>=26);
}
static const char *packet(WifiBoot *b,const WifiOps *o,const uint8_t *p,unsigned n,uint32_t flags,uint64_t now) {
    WifiLive *l=&b->live;unsigned code=p[0],group=p[1];p+=4;n-=4;
    if(code==0x1c && group==0)return completion(b,o,p,n);
    if(code==24 && (group&~0x40U)==5)return wifi_rekey_reply(b,p-4,n+4);
    if((code==2 && group==0) || (group==4 && code==0xfe))return "live firmware error/thermal kill";
    if(code==0xc9 && group==0) {
        if(n!=4 || u16(p)!=b->reg_applied_country || p[2]!=b->reg_applied_source)return "live regulatory profile changed";
        return 0;
    }
    if(code==0xa2 && group==0) {
        /* Linux v6.12 mac.h API4: link, since-last-RX, consecutive,
         * expected, received. mvm/mac-ctxt.c NEW-TX path only reports CQM
         * above8 since RX; it disconnects at19 consecutive AND4 since RX.
         * Do not turn one moderate warning into an unconditional radio reset.
         * OpenBSD iwx probes before loss; no scan/reconnect is added here. */
        if(n!=20 || u32(p)!=0)return "live missed-beacon notification bounds/link";
        unsigned since_rx=u32(p+4),consecutive=u32(p+8);
        wifi_live_diag_missed(&l->diag,now,since_rx,consecutive,u32(p+12),u32(p+16));
        if(consecutive>=19 && since_rx>=4)return "live prolonged beacon loss without recent RX";
        return 0;
    }
    if(code!=0xc1 || group!=0)return 0;
    if(selected_ap_frame(b,p,n))wifi_live_diag_ap_rx(&l->diag,now);
    const char *error=wifi_assoc_mpdu(b,p,n);if(error)return error;
    unsigned eapol_bytes=0;const uint8_t *eapol=eapol_clear(b,p,n,&eapol_bytes);
    if(eapol)return wifi_rekey_input(b,eapol,eapol_bytes,now);
    uint8_t eth[NIC_FRAME];unsigned length=0,bad=b->data.group_mic_bad,drops=b->data.drops;
    unsigned accepted=wifi_data_decode_flags(b,p,n,eth,&length,flags);
    if(drops!=b->data.drops)increment(l,2);
    if(bad!=b->data.group_mic_bad)return "live TKIP MIC failure; link stopped";
    if(!accepted)return 0;
    if(eth[12]==0x88 && eth[13]==0x8e)return wifi_rekey_input(b,eth+14,length-14,now);
    wifi_live_diag_data_rx(&l->diag,now);
    increment(l,0);
    if(l->count==NIC_FRAMES || length>NIC_OCTETS-l->octets){increment(l,2);return 0;}
    unsigned tail=(l->head+l->count)%NIC_FRAMES;
    copy(l->received[tail],eth,length);l->lengths[tail]=length;++l->count;l->octets+=length;return 0;
}
const char *wifi_live_poll(WifiBoot *b,const WifiOps *o,uint64_t now) {
    WifiLive *l=&b->live;if(!l->active)return 0;
    wifi_live_diag_poll(&l->diag,now);
    uint32_t causes=o->read(o->opaque,8);
    if(causes==UINT32_MAX || (causes&0x22000001) || !(o->read(o->opaque,0x24)&0x08000000))
        return broken(b,o,"live firmware reset/error/radio kill",now);
    if(causes)o->write(o->opaque,8,causes);
    unsigned producer=*(volatile uint16_t *)(void *)b->status&511;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned handled=0;
    while((b->consumed&511)!=producer && handled<64) {
        uint8_t *buf=b->rx+(b->consumed&511)*4096;
        /* iwx_rx_pkt explicitly stops after ONE packet for AX210+, including
         * command notifications. Never interpret trailing DMA as another one. */
        uint32_t flags=u32(buf);unsigned n=flags&0x3fff;
        if(n<4 || n>4092 || flags==0x55550000 || !(u32(buf+4)&0x7fffffffU))
            return broken(b,o,"live RX packet bounds",now);
        const char *error=packet(b,o,buf+4,n,flags,now);if(error)return broken(b,o,error,now);
        wifi_wipe(buf,4096);p32(buf,0x55550000);++b->consumed;
        ++handled;
    }
    if(handled) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        o->write(o->opaque,0x1c80,((b->consumed-1)&511)&~7U);
    }
    if(causes&0x90000008){o->write(o->opaque,0x10,(1U<<17)|(1U<<16)|(1U<<30));o->write8(o->opaque,5,0);}
    const char *error=wifi_rekey_poll(b,o,now);if(error)return broken(b,o,error,now);
    if(l->tx_count && now-l->tx_when[l->tx_tail&255]>=5000000)return broken(b,o,"live TX watchdog timeout",now);
    return 0;
}
static unsigned transmit(WifiBoot *b,const WifiOps *o,const uint8_t *data,unsigned n,uint64_t now,unsigned control) {
    WifiLive *l=&b->live;if(!l->active)return NIC_DOWN;
    if(n<14 || n>NIC_FRAME)return NIC_DOWN;
    /* Briefly defer new data while a key reply owns the queue tail. Existing
     * DMA and TCP state stay intact; cumulative TX status can then be bound
     * to that last control frame without guessing earlier-frame status. */
    if(!control && b->rekey.stage){increment(l,6);return NIC_WOULD_BLOCK;}
    if(l->tx_count==NIC_FRAMES || n>NIC_OCTETS-l->tx_octets){increment(l,6);return NIC_WOULD_BLOCK;}
    unsigned slot=l->tx_head&255;
    if(l->tx_owned[slot]){broken(b,o,"live TX reuse before completion",now);return NIC_DOWN;}
    uint8_t *cmd=l->tx_pages+slot*4096,frame[NIC_FRAME+18];
    unsigned len=wifi_data_encode(b,data,n,frame,sizeof(frame));if(!len)return NIC_DOWN;
    unsigned rate=0;while(rate<8 && !(b->target.basic_rates&(1U<<(rate+4))))++rate;
    if(rate==8){broken(b,o,"live no basic OFDM rate",now);return NIC_DOWN;}
    wifi_wipe(cmd,4096);cmd[0]=0x1c;cmd[2]=slot;cmd[3]=1;p16(cmd+4,len);p16(cmd+6,1);
    p32(cmd+8,12U<<8);p32(cmd+20,(1U<<14)|(1U<<8)|rate);
    copy(cmd+32,frame,24);p16(cmd+54,l->sequence<<4);copy(cmd+64,frame+24,len-24);
    uint8_t *desc=b->management_tx+slot*256;wifi_wipe(desc,256);
    p16(desc,3);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);p16(desc+12,36);p64(desc+14,(uintptr_t)cmd+20);
    p16(desc+22,len-24);p64(desc+24,(uintptr_t)cmd+64);p16(b->management_bc+slot*2,len);
    l->tx_owned[slot]=1;l->tx_size[slot]=n;l->tx_when[slot]=now;l->tx_head=(l->tx_head+1)&65535;
    l->sequence=(l->sequence+1)&4095;
    ++l->tx_count;l->tx_octets+=n;increment(l,3);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);o->write(o->opaque,0x460,(1U<<16)|l->tx_head);return NIC_OK;
}
unsigned wifi_live_tx(WifiBoot *b,const WifiOps *o,const uint8_t *p,unsigned n,uint64_t now){return transmit(b,o,p,n,now,0);}
unsigned wifi_live_control_tx(WifiBoot *b,const WifiOps *o,const uint8_t *p,unsigned n,uint64_t now){return transmit(b,o,p,n,now,1);}
unsigned wifi_live_rx(WifiBoot *b,uint8_t *out,unsigned capacity,unsigned *length) {
    WifiLive *l=&b->live;*length=0;
    if(!l->count)return l->active?NIC_EMPTY:NIC_DOWN;
    *length=l->lengths[l->head];if(capacity<*length)return NIC_TOO_SMALL;
    copy(out,l->received[l->head],*length);l->octets-=*length;
    wifi_wipe(l->received[l->head],*length);l->head=(l->head+1)%NIC_FRAMES;--l->count;increment(l,1);return NIC_OK;
}
void wifi_live_info(WifiBoot *b,NicInfo *out) {
    WifiLive *l=&b->live;*out=l->info;out->rx_frames=l->count;out->rx_octets=l->octets;
    out->tx_frames=l->tx_count;out->tx_octets=l->tx_octets;
}
